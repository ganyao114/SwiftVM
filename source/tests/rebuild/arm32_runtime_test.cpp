#include "runtime/execution/vm.h"
#include "runtime/platform/linux/abi_arm32.h"
#include "runtime/platform/linux/host_runtime.h"
#include "runtime/platform/linux/signal_frame.h"
#include "tests/support/check.h"
#include "tests/support/test_mode.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <elf.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
using namespace swift::runtime;
namespace exe = execution;
namespace linux32 = platform::abi32;
constexpr std::uint32_t Base = 0x20000000, Entry = Base + 0x100;

std::vector<std::uint8_t> Image(std::span<const std::uint8_t> code, bool thumb = false) {
    std::vector<std::uint8_t> bytes(0x100 + code.size());
    Elf32_Ehdr header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS32;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_type = ET_EXEC;
    header.e_machine = EM_ARM;
    header.e_version = EV_CURRENT;
    header.e_entry = Entry | unsigned(thumb);
    header.e_phoff = sizeof(header);
    header.e_ehsize = sizeof(header);
    header.e_phentsize = sizeof(Elf32_Phdr);
    header.e_phnum = 1;
    header.e_flags = 0x05000000;
    Elf32_Phdr segment{};
    segment.p_type = PT_LOAD;
    segment.p_vaddr = segment.p_paddr = Base;
    segment.p_filesz = segment.p_memsz = bytes.size();
    segment.p_flags = PF_R | PF_X;
    segment.p_align = 4096;
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), &segment, sizeof(segment));
    std::memcpy(bytes.data() + 0x100, code.data(), code.size());
    return bytes;
}

std::vector<std::uint8_t> Interworking() {
    constexpr std::uint32_t arm[]{0xe28f300c, 0xe2833001, 0xe12fff33, 0xe3a07001, 0xef000000};
    constexpr std::uint16_t thumb[]{0x2000, 0x3001, 0x282a, 0xd1fc, 0x4770};
    std::vector<std::uint8_t> code(sizeof(arm) + sizeof(thumb));
    std::memcpy(code.data(), arm, sizeof(arm));
    std::memcpy(code.data() + sizeof(arm), thumb, sizeof(thumb));
    return Image(code);
}

std::uint64_t StackPointer(exe::VM& vm) { return vm.Arm32State().r[13]; }
}  // namespace

TEST_CASE("ARM32 ELF32 stack, interworking and mixed promotion share one frontend",
          "[native][arm32]") {
    using Mode = exe::ExecutionMode;
    for (const auto mode : {Mode::InterpretOnly, Mode::JitOnly, Mode::Mixed}) {
        for (const auto scope : {ir::Scope::Block, ir::Scope::Function}) {
            INFO("mode=" << unsigned(mode) << " scope=" << unsigned(scope));
            platform::HostRuntime host{true};
            exe::RuntimeOptions options;
            options.mode = mode;
            options.scope = scope;
            options.hot_threshold = 3;
            options.decode_timing = true;
            exe::RuntimeStats stats;
            exe::VM vm{options, stats, host.Bind()};
            const std::array<std::string, 1> argv{"arm32-fixture"};
            vm.PrepareGuest(Interworking(), "arm32-fixture", argv);
            REQUIRE(vm.ISA() == GuestISA::Arm32);
            const auto sp = StackPointer(vm);
            CHECK(sp < (UINT64_C(1) << 32) && !(sp & 7));
            std::uint32_t words[3];
            REQUIRE(vm.Memory().CopyIn(words, sp, sizeof(words)));
            CHECK(words[0] == 1 && words[1] < sp + (8u << 20) && words[2] == 0);
            CHECK(vm.RunPrepared() == 42);
            if (mode == Mode::InterpretOnly)
                CHECK(stats.compile_attempts == 0);
            else
                CHECK(stats.compile_attempts != 0);
            if (mode == Mode::Mixed) CHECK(stats.interpreted_steps != 0);
            CHECK(vm.Close().Complete());
        }
    }
}

TEST_CASE("ARM32 signal frame restores edited integer, VFP and full Thumb IT location",
          "[compile][arm32][signals]") {
    platform::HostRuntime host{false};
    exe::RuntimeStats stats;
    exe::VM vm{{}, stats, host.Bind()};
    vm.PrepareGuest(Interworking(), "arm32-frame");
    auto state = vm.Arm32State();
    state.pc = arm32::Location::Make(Entry + 20, true, 0x18, 1u << 22).Key();
    state.fpscr = 1u << 22;
    state.r[0] = 99;
    state.q[0][7] = 0xaa;
    GuestExceptionInfo event;
    event.isa = GuestISA::Arm32;
    event.address = 0x1234;
    event.address_valid = true;
    platform::GuestSignalInfo info;
    info.number = 11;
    for (const bool realtime : {false, true}) {
        platform::GuestSignalAction action{Entry + 21, realtime ? 4u : 0u, 0, 0};
        platform::SignalFrameContext saved{0x123456789abc, {}};
        platform::EncodedSignalFrame encoded;
        arm32::GuestState handler;
        REQUIRE(platform::EncodeArm32SignalFrame(
                state, event, info, action, saved, 0xffff0fe0, encoded, handler));
        CHECK(encoded.bytes.size() == (realtime ? 880 : 752));
        CHECK(handler.r[0] == 11);
        if (realtime) CHECK(handler.r[1] == encoded.address);
        CHECK(arm32::Location{handler.pc}.Thumb() && !arm32::Location{handler.pc}.IT().Value());
        const std::uint32_t changed_r0 = 0xdeadbeef;
        std::memcpy(encoded.bytes.data() + (realtime ? 128 : 0) + 20 + 12, &changed_r0, 4);
        REQUIRE(vm.Memory().CopyOut(encoded.address, encoded.bytes.data(), encoded.bytes.size()));
        arm32::GuestState restored;
        platform::SignalFrameContext restored_context;
        REQUIRE(platform::DecodeArm32SignalFrame(
                vm.Memory(), handler, restored, restored_context, realtime));
        CHECK(restored.pc == state.pc && restored.r[0] == changed_r0);
        CHECK(restored.q[0][7] == 0xaa && restored.fpscr == state.fpscr);
        CHECK(restored_context.mask == saved.mask);
    }
    CHECK(vm.Close().Complete());
}

TEST_CASE("ARM EABI converts pointer sized structures and private TLS calls",
          "[compile][arm32][syscall]") {
    platform::HostRuntime host{false};
    exe::RuntimeStats stats;
    exe::VM vm{{}, stats, host.Bind()};
    vm.PrepareGuest(Interworking(), "arm32-abi");
    const auto scratch =
            vm.Memory().Map(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    REQUIRE(scratch < (UINT64_C(1) << 32));
    platform::LinuxSyscalls service{vm};
    const auto invoke = [&](std::uint32_t number, std::array<std::uint32_t, 6> args) {
        auto& state = vm.Arm32State();
        state.service_pc = arm32::Location::Make(Entry, true, 0x18).Key();
        state.pc = arm32::Location::Make(Entry + 2, true).Key();
        state.r[7] = number;
        for (unsigned i = 0; i < 6; ++i) state.r[i] = args[i];
        service.Service();
        return std::int32_t(state.r[0]);
    };
    REQUIRE(invoke(0xf0005, {0x12345678}) == 0);
    CHECK(vm.Arm32State().tpidruro == 0x12345678);
    CHECK(std::uint32_t(invoke(0xf0006, {})) == 0x12345678);
    const linux32::Action action{Entry + 21, 4, 0, {0x2000, 0x200}};
    REQUIRE(vm.Memory().CopyOut(scratch, &action, sizeof(action)));
    REQUIRE(invoke(174, {10, std::uint32_t(scratch), 0, 8}) == 0);
    REQUIRE(invoke(174, {10, 0, std::uint32_t(scratch + 32), 8}) == 0);
    linux32::Action output;
    REQUIRE(vm.Memory().CopyIn(&output, scratch + 32, sizeof(output)));
    CHECK(output.handler == action.handler && output.mask == action.mask);
    const std::uint32_t sentinel = 0x11223344;
    REQUIRE(vm.Memory().CopyOut(scratch + 72, &sentinel, 4));
    REQUIRE(invoke(263, {1, std::uint32_t(scratch + 64)}) == 0);
    std::uint32_t observed;
    REQUIRE(vm.Memory().CopyIn(&observed, scratch + 72, 4));
    CHECK(observed == sentinel);
    CHECK(vm.Close().Complete());
}

TEST_CASE("Thumb SVC restart preserves its original IT and exact successor location",
          "[native][arm32][signals]") {
    for (const bool restart : {false, true}) {
        platform::HostRuntime host{false};
        exe::RuntimeOptions options;
        options.mode = exe::ExecutionMode::InterpretOnly;
        exe::RuntimeStats stats;
        exe::VM vm{options, stats, host.Bind()};
        vm.PrepareGuest(Interworking(), "thumb-restart");
        const auto scratch =
                vm.Memory().Map(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
        REQUIRE(scratch < (UINT64_C(1) << 32));
        platform::LinuxSyscalls service{vm};
        auto& state = vm.Arm32State();
        const auto before = arm32::Location::Make(Entry + 20, true, 0x18).Key();
        const auto after = arm32::Location::Make(Entry + 22, true).Key();
        const auto invoke = [&](std::uint32_t number, std::array<std::uint32_t, 6> args) {
            state.service_pc = before;
            state.pc = after;
            state.r[7] = number;
            for (unsigned i = 0; i < 6; ++i) state.r[i] = args[i];
            service.Service();
            return std::int32_t(state.r[0]);
        };
        const linux32::Action action{Entry, restart ? 0x10000004u : 4u, 0, {0, 0}};
        REQUIRE(vm.Memory().CopyOut(scratch, &action, sizeof(action)));
        REQUIRE(invoke(174, {SIGUSR1, std::uint32_t(scratch), 0, 8}) == 0);
        REQUIRE(invoke(37, {std::uint32_t(getpid()), SIGUSR1}) == 0);
        REQUIRE(invoke(240, {std::uint32_t(scratch + 256), 0, 0}) == -EINTR);
        // Delivery chooses restart before the frame is encoded. The handler
        // exits deliberately so the frame can be inspected without a host hook.
        REQUIRE(vm.RunPrepared() == 42);
        arm32::GuestState saved;
        platform::SignalFrameContext context;
        REQUIRE(platform::DecodeArm32SignalFrame(vm.Memory(), state, saved, context));
        CHECK(saved.pc == (restart ? before : after));
        CHECK(saved.r[0] == (restart ? std::uint32_t(scratch + 256) : std::uint32_t(-EINTR)));
        CHECK(vm.Close().Complete());
    }
}

TEST_CASE("ARM32 non-SIGINFO handlers return through the legacy EABI signal frame",
          "[native][arm32][signals]") {
    for (const auto mode : {exe::ExecutionMode::InterpretOnly, exe::ExecutionMode::JitOnly}) {
        for (const bool custom_restorer : {false, true}) {
            platform::HostRuntime host{false};
            exe::RuntimeOptions options;
            options.mode = mode;
            exe::RuntimeStats stats;
            exe::VM vm{options, stats, host.Bind()};
            vm.PrepareGuest(Interworking(), "arm32-legacy-signal");
            const auto scratch =
                    vm.Memory().Map(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
            REQUIRE(scratch < (UINT64_C(1) << 32));
            const std::uint32_t code[]{0xe12fff1e,
                                       0xe320f000,
                                       0xe320f000,
                                       0xe320f000,
                                       0xe3a07077,
                                       0xef000000,
                                       0xe7f000f0};
            REQUIRE(vm.Memory().CopyOut(scratch, code, sizeof(code)));
            const linux32::Action action{std::uint32_t(scratch),
                                         custom_restorer ? 0x04000000u : 0u,
                                         custom_restorer ? std::uint32_t(scratch + 16) : 0u,
                                         {0, 0}};
            REQUIRE(vm.Memory().CopyOut(scratch + 128, &action, sizeof(action)));
            platform::LinuxSyscalls service{vm};
            auto& state = vm.Arm32State();
            state.r[0] = SIGUSR1;
            state.r[1] = scratch + 128;
            state.r[2] = 0;
            state.r[3] = 8;
            state.r[7] = 174;
            service.Service();
            REQUIRE(state.r[0] == 0);
            REQUIRE(vm.Memory().Protect(scratch, vm.Memory().PageSize(), PROT_READ | PROT_EXEC) ==
                    0);
            state.r[0] = getpid();
            state.r[1] = SIGUSR1;
            state.r[7] = 37;
            service.Service();
            REQUIRE(state.r[0] == 0);
            REQUIRE(vm.RunPrepared() == 42);
            CHECK(stats.unsupported_syscalls == 0);
            CHECK(vm.Close().Complete());
        }
    }
}
