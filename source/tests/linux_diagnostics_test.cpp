#include <array>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/signal_handler.h"
#include "runtime/common/svm_config.h"
#include "translator/arm64/translator.h"
#include "translator/linux/diagnostics.h"
#include "translator/x86/translator.h"

namespace {

volatile sig_atomic_t alarm_seen{};
void OnAlarm(int) { alarm_seen = 1; }

struct RestoreAlarm {
    struct sigaction previous{};
    RestoreAlarm() { sigaction(SIGALRM, nullptr, &previous); }
    ~RestoreAlarm() { sigaction(SIGALRM, &previous, nullptr); }
};

std::string CaptureDump(const std::function<void()>& dump) {
    auto file = std::unique_ptr<FILE, decltype(&std::fclose)>(std::tmpfile(), &std::fclose);
    REQUIRE(file);
    std::fflush(stderr);
    const int saved = dup(STDERR_FILENO);
    REQUIRE(saved >= 0);
    REQUIRE(dup2(fileno(file.get()), STDERR_FILENO) >= 0);
    dump();
    std::fflush(stderr);
    const int restored = dup2(saved, STDERR_FILENO);
    close(saved);
    REQUIRE(restored >= 0);
    std::rewind(file.get());
    std::string result;
    char buffer[256];
    while (const auto count = std::fread(buffer, 1, sizeof(buffer), file.get())) {
        result.append(buffer, count);
    }
    return result;
}

template <typename Instance, typename Core>
void CheckTraceLifetime() {
    swift::runtime::InitSvmConfig();
    std::unique_ptr<Instance, decltype(&Instance::Destroy)> instance(
            Instance::Make(), &Instance::Destroy);
    std::unique_ptr<Core, decltype(&Core::Destroy)> first(
            Core::Make(instance.get()), &Core::Destroy);
    std::unique_ptr<Core, decltype(&Core::Destroy)> second(
            Core::Make(instance.get()), &Core::Destroy);
    auto first_dump = first->MakeExecutionTraceDumper();
    auto expired_dump = second->MakeExecutionTraceDumper();
    second.reset();
    const auto live_output = CaptureDump(first_dump);
    if (swift::runtime::GetSvmConfig().exec_trace) {
        REQUIRE(live_output.find("[sys-trace]") != std::string::npos);
    } else {
        REQUIRE(live_output.empty());
    }
    REQUIRE(CaptureDump(expired_dump).empty());
    first.reset();
    instance.reset();
    REQUIRE(CaptureDump(first_dump).empty());
}

struct WatchedMapping {
    swift::linux::GuestMemory memory;
    swift::VAddr address = memory.MapAnywhere(swift::linux::GuestMemory::kHostPageSize);
    ~WatchedMapping() {
        if (address) memory.Unmap(address, swift::linux::GuestMemory::kHostPageSize);
    }
};

}  // namespace

TEST_CASE("fresh guest mmap leaves data pages uncommitted", "[linux-memory]") {
    using namespace swift;
    linux::GuestMemory memory;
    linux::SyscallHandler syscalls{&memory, 0, linux::GuestISA::kArm64};
    constexpr auto size = 32 * linux::GuestMemory::kHostPageSize;
    const auto result = syscalls.Handle(222, 0, size, 3, 0x22, ~0ull, 0);
    REQUIRE(result.ret > 0);
    const auto address = static_cast<VAddr>(result.ret);
    const auto cleanup = [&memory, address](void*) { memory.Unmap(address, size); };
    std::unique_ptr<void, decltype(cleanup)> mapping(memory.ToHost(address), cleanup);
#if defined(__APPLE__)
    std::array<char, 32> residency{};
#else
    std::array<unsigned char, 32> residency{};
#endif
    REQUIRE(mincore(mapping.get(), size, residency.data()) == 0);
    REQUIRE(std::none_of(residency.begin(), residency.end(), [](auto page) { return page & 1; }));
    const auto* bytes = static_cast<const u8*>(mapping.get());
    REQUIRE(bytes[0] == 0);
    REQUIRE(bytes[size - 1] == 0);
}

TEST_CASE("fixed guest mmap clears only the replaced guest pages", "[linux-memory]") {
    using namespace swift;
    linux::GuestMemory memory;
    linux::SyscallHandler syscalls{&memory, 0, linux::GuestISA::kArm64};
    constexpr auto page = linux::GuestMemory::kGuestPageSize;
    constexpr auto size = 2 * linux::GuestMemory::kHostPageSize;
    const auto result = syscalls.Handle(222, 0, size, 3, 0x22, ~0ull, 0);
    REQUIRE(result.ret > 0);
    const auto address = static_cast<VAddr>(result.ret);
    const auto cleanup = [&memory, address](void*) { memory.Unmap(address, size); };
    std::unique_ptr<void, decltype(cleanup)> mapping(memory.ToHost(address), cleanup);
    auto* bytes = static_cast<u8*>(mapping.get());
    std::fill_n(bytes, size, 0xa5);

    auto replacement = syscalls.Handle(222, address + page, 7, 3, 0x32, ~0ull, 0);
    REQUIRE(replacement.ret == static_cast<s64>(address + page));
    for (u64 offset = 0; offset < size; ++offset) {
        REQUIRE(bytes[offset] == (offset >= page && offset < 2 * page ? 0 : 0xa5));
    }

    auto file = std::unique_ptr<FILE, decltype(&std::fclose)>(std::tmpfile(), &std::fclose);
    REQUIRE(file);
    REQUIRE(std::fwrite("abc", 1, 3, file.get()) == 3);
    REQUIRE(std::fflush(file.get()) == 0);
    std::fill_n(bytes, size, 0xa5);
    replacement = syscalls.Handle(222, address + page, 7, 3, 0x12, fileno(file.get()), 0);
    REQUIRE(replacement.ret == static_cast<s64>(address + page));
    REQUIRE(std::equal(bytes + page, bytes + page + 3, "abc"));
    for (u64 offset = 0; offset < size; ++offset) {
        if (offset >= page && offset < page + 3) continue;
        REQUIRE(bytes[offset] == (offset >= page && offset < 2 * page ? 0 : 0xa5));
    }
}

TEST_CASE("large invalid mmap requests return guest errors for both ISAs", "[linux-diagnostics]") {
    using namespace swift;
    for (const auto isa : {linux::GuestISA::kArm64, linux::GuestISA::kX86_64}) {
        linux::GuestMemory memory;
        linux::SyscallHandler syscalls{&memory, 0, isa};
        x86::ThreadContext64 context{};
        if (isa == linux::GuestISA::kX86_64) syscalls.SetX86Context(&context);
        const auto result = syscalls.Handle(isa == linux::GuestISA::kX86_64 ? 9 : 222,
                                            0, 1ull << 41, 0, 0x22, ~0ull, 1);
        REQUIRE(result.ret == -22);
        REQUIRE_FALSE(result.exited);
    }
}

TEST_CASE("runtime signal installation preserves the host alarm handler", "[linux-diagnostics]") {
    RestoreAlarm restore;
    alarm_seen = 0;
    std::signal(SIGALRM, &OnAlarm);
    swift::runtime::backend::SignalHandler::Install();
    std::raise(SIGALRM);
    REQUIRE(alarm_seen == 1);
}

TEST_CASE("execution trace callbacks expire with their own Core", "[linux-diagnostics]") {
    SECTION("x86") {
        CheckTraceLifetime<swift::translator::x86::X86Instance,
                           swift::translator::x86::X86Core>();
    }
    SECTION("arm64") {
        CheckTraceLifetime<swift::translator::arm64::Arm64Instance,
                           swift::translator::arm64::Arm64Core>();
    }
}

TEST_CASE("guest diagnostics tolerate inaccessible and unmapped memory", "[linux-diagnostics]") {
    WatchedMapping mapping;
    REQUIRE(mapping.address != 0);
    std::array<swift::u8, 64> bytes{};
    REQUIRE(swift::linux::ReadGuestBytes(mapping.memory, mapping.address, bytes));
    REQUIRE(mapping.memory.Protect(mapping.address, swift::linux::GuestMemory::kHostPageSize,
                                   false, false, false));
    REQUIRE_FALSE(swift::linux::ReadGuestBytes(mapping.memory, mapping.address, bytes));
    mapping.memory.Unmap(mapping.address, swift::linux::GuestMemory::kHostPageSize);
    REQUIRE_FALSE(swift::linux::ReadGuestBytes(mapping.memory, mapping.address, bytes));
}

TEST_CASE("guest memory watchers stop after their mapping disappears", "[linux-diagnostics]") {
    WatchedMapping mapping;
    REQUIRE(mapping.address != 0);
    auto process = std::make_shared<swift::linux::SyscallProcessState>(&mapping.memory, 0);
    const auto start = std::chrono::steady_clock::now();
    {
        swift::linux::GuestMemoryWatch watch(mapping.memory, process, mapping.address, 1, 0x401000);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        {
            std::lock_guard guard(process->memory_mutex);
            mapping.memory.Unmap(mapping.address, swift::linux::GuestMemory::kHostPageSize);
        }
        watch.SetLocation(0x402000);
        // Destructor requests stop and joins while the process is still alive.
    }
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}
