#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "runtime/backend/address_space.h"
#include "runtime/backend/smc_tracker.h"
#include "runtime/common/slab_alloc.h"
#include "translator/linux/guest_memory.h"
#include "translator/x86/translator.h"

namespace {
using namespace swift;
using namespace swift::translator::x86;

struct CompileGate {
    std::mutex mutex;
    std::condition_variable cv;
    unsigned entered{};
    unsigned active{};
    unsigned peak{};
    bool release{};

    static void Observe(void* opaque, uint64_t) {
        auto& gate = *static_cast<CompileGate*>(opaque);
        std::unique_lock lock(gate.mutex);
        ++gate.entered;
        ++gate.active;
        gate.peak = std::max(gate.peak, gate.active);
        gate.cv.notify_all();
        gate.cv.wait(lock, [&] { return gate.release; });
        --gate.active;
    }

    bool WaitFor(unsigned count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return entered >= count; });
    }

    void Release() {
        std::lock_guard lock(mutex);
        release = true;
        cv.notify_all();
    }
};

struct ScopedSmc {
    const bool prior = runtime::backend::SmcTracker::IsEnabled();
    explicit ScopedSmc(bool enabled = false) { runtime::backend::SmcTracker::SetEnabled(enabled); }
    ~ScopedSmc() { runtime::backend::SmcTracker::SetEnabled(prior); }
};
}  // namespace

TEST_CASE("shared slab nodes have one owner during concurrent reuse", "[compile-concurrency][allocation]") {
    constexpr unsigned count = 64;
    runtime::SlabAllocator allocator;
    std::array<runtime::SlabAllocator::Node, count> nodes{};
    std::array<std::atomic_bool, count> owned{};
    for (auto& node : nodes) allocator.Free(&node);
    std::atomic<unsigned> collisions{};
    std::array<std::thread, 4> workers;
    for (auto& worker : workers) {
        worker = std::thread([&] {
            for (unsigned iteration = 0; iteration < 1000; ++iteration) {
                runtime::SlabAllocator::Node* node{};
                while (!(node = static_cast<runtime::SlabAllocator::Node*>(allocator.Allocate()))) {
                    std::this_thread::yield();
                }
                const auto index = node - nodes.data();
                if (owned[index].exchange(true)) ++collisions;
                std::this_thread::yield();
                owned[index].store(false);
                allocator.Free(node);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    REQUIRE(collisions.load() == 0);
    std::set<void*> returned;
    for (unsigned index = 0; index < count; ++index) {
        const auto node = allocator.Allocate();
        REQUIRE(node != nullptr);
        REQUIRE(returned.insert(node).second);
    }
    REQUIRE(allocator.Allocate() == nullptr);
}

TEST_CASE("cold compilation overlaps only independent bounded regions", "[compile-concurrency]") {
#if defined(__aarch64__)
    const bool smc_enabled = GENERATE(false, true);
    CAPTURE(smc_enabled);
    ScopedSmc smc{smc_enabled};
    for (const unsigned scenario : {0u, 1u, 2u, 3u}) {
        // Independent lazy regions, overlapping lazy regions, identical PCs,
        // and independent eager functions, respectively.
        CAPTURE(scenario);
        linux::GuestMemory memory;
        const auto base = memory.MapAnywhere(512 * 1024);
        REQUIRE(base != 0);
        std::array<u64, 2> addresses{base + 0x100, base + 128 * 1024};
        if (scenario == 1) addresses[1] = addresses[0] + 0x100;
        if (scenario == 2) addresses[1] = addresses[0];
        constexpr u64 fingerprint = 0xfedcba9876543210;
        std::array<u8, 11> code{0x48, 0xb8}; // movabs rax, fingerprint; hlt
        std::memcpy(code.data() + 2, &fingerprint, sizeof(fingerprint));
        code.back() = 0xf4;
        for (const auto pc : addresses) {
            std::memcpy(memory.ToHost(pc), code.data(), code.size());
        }
        std::unique_ptr<X86Instance, decltype(&X86Instance::Destroy)> instance{
                X86Instance::Make(reinterpret_cast<void*>(memory.GetBias())),
                X86Instance::Destroy};
        instance->SetFunctionDecodeBudget(scenario == 3 ? 1024 : 32);
        instance->PrepareForMultithreading();
        CompileGate gate;
        instance->SetCompileObserver(&CompileGate::Observe, &gate);
        std::array<void*, 2> entries{};
        std::array<std::exception_ptr, 2> errors{};
        auto compile = [&](unsigned index) {
            try {
                entries[index] = instance->CompileAt(addresses[index]);
            } catch (...) {
                errors[index] = std::current_exception();
            }
        };
        std::thread first{compile, 0};
        const bool first_entered = gate.WaitFor(1, std::chrono::seconds{2});
        std::thread second{compile, 1};
        const bool both_entered = gate.WaitFor(2, scenario == 0
                ? std::chrono::milliseconds{2000} : std::chrono::milliseconds{100});
        gate.Release();
        first.join();
        second.join();
        instance->SetCompileObserver(nullptr, nullptr);
        REQUIRE(first_entered);
        REQUIRE(both_entered == (scenario == 0));
        REQUIRE(gate.peak == (scenario == 0 ? 2 : 1));
        REQUIRE(gate.entered == (scenario == 2 ? 1 : 2));
        for (unsigned index = 0; index < addresses.size(); ++index) {
            REQUIRE_FALSE(errors[index]);
            REQUIRE(entries[index] != nullptr);
            REQUIRE(instance->CompileAt(addresses[index]) ==
                    instance->GetAddressSpace()->GetCodeCache(addresses[index]));
            std::unique_ptr<X86Core, decltype(&X86Core::Destroy)> core{
                    X86Core::Make(instance.get()), X86Core::Destroy};
            auto& context = core->GetContext();
            context.rip.qword = addresses[index];
            context.rsp.qword = base + 500 * 1024;
            core->Run();
            REQUIRE(context.rax.qword == fingerprint);
        }
    }
#else
    SUCCEED("Concurrent native compilation requires an AArch64 host");
#endif
}

TEST_CASE("bounded block decoding stops at an instruction boundary", "[compile-concurrency][decode]") {
#if defined(__aarch64__)
    ScopedSmc smc;
    linux::GuestMemory memory;
    const auto base = memory.MapAnywhere(32 * 1024);
    REQUIRE(base != 0);
    // Repeated three-byte instructions cross the 8 KiB limit. Their successor
    // must remain an external entry instead of extending the protected range.
    auto* bytes = static_cast<u8*>(memory.ToHost(base));
    for (unsigned offset = 0; offset < 16 * 1024; offset += 3) {
        std::memcpy(bytes + offset, "\x48\x89\xc0", 3); // mov rax, rax
    }
    std::unique_ptr<X86Instance, decltype(&X86Instance::Destroy)> instance{
            X86Instance::Make(reinterpret_cast<void*>(memory.GetBias())), X86Instance::Destroy};
    instance->SetFunctionDecodeBudget(1);
    REQUIRE(instance->CompileAt(base) != nullptr);
    auto node = instance->GetAddressSpace()->GetDefaultModule()->GetNode(base);
    REQUIRE(runtime::backend::IsFunction(node));
    const auto function = runtime::backend::GetFunction(node);
    const auto block = function->EntryBlock();
    REQUIRE(block != nullptr);
    const auto terminal = boost::get<runtime::ir::terminal::ExternalLinkBlock>(
            block->GetTerminal());
    REQUIRE(terminal.next.Value() == base + 8193);
    REQUIRE(instance->GetAddressSpace()->GetCodeCache(base + 8193) == nullptr);
#else
    SUCCEED("Bounded native compilation requires an AArch64 host");
#endif
}
