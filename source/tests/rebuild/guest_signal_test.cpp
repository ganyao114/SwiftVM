#include "runtime/arch/x86/flags.h"
#include "runtime/execution/memory.h"
#include "runtime/execution/vm.h"
#include "runtime/platform/linux/host_runtime.h"
#include "runtime/platform/linux/signal_frame.h"
#include "runtime/platform/linux/wait_syscall.h"
#include "tests/support/check.h"

#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
using namespace swift::runtime;
using namespace swift::runtime::platform;

void CancelWaitSignal(int, siginfo_t*, void* context) { CancelHostWait(context); }

volatile sig_atomic_t saw_futex_signal{};

void InterruptFutex(int) { saw_futex_signal = 1; }

TEST_CASE("Guest signal queues retain blocked ignores and isolate realtime capacity",
          "[compile][signals]") {
    HostRuntime host{false};
    execution::RuntimeStats stats;
    execution::VM vm{{}, stats, host.Bind()};
    const auto memory =
            vm.Memory().Map(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    REQUIRE(memory != UINT64_MAX);
    LinuxSyscalls service{vm};
    const auto invoke = [&](std::uint64_t number, std::array<std::uint64_t, 6> args) {
        auto& state = vm.State();
        state.pc = 0x400002;
        state.gprs[x86::RAX] = number;
        constexpr unsigned registers[]{x86::RDI, x86::RSI, x86::RDX, x86::R10, x86::R8, x86::R9};
        for (unsigned i = 0; i < 6; ++i) state.gprs[registers[i]] = args[i];
        service.Service();
        return static_cast<std::int64_t>(state.gprs[x86::RAX]);
    };
    constexpr auto bit = [](unsigned number) { return UINT64_C(1) << (number - 1); };
    const GuestSignalAction ignored{1, 0, 0, 0};
    REQUIRE(vm.Memory().CopyOut(memory, &ignored, sizeof(ignored)));
    REQUIRE(invoke(13, {SIGUSR1, memory, 0, 8}) == 0);
    const std::uint64_t mask = bit(SIGUSR1) | bit(34) | bit(SIGTSTP) | bit(SIGCONT);
    REQUIRE(vm.Memory().CopyOut(memory + 64, &mask, 8));
    REQUIRE(invoke(14, {0, memory + 64, 0, 8}) == 0);
    const auto queue = [&](unsigned number, std::uint64_t value) {
        GuestSignalInfo info;
        info.number = number;
        info.code = -1;
        std::memcpy(info.payload.data() + 8, &value, 8);
        vm.Memory().CopyOut(memory + 128, &info, sizeof(info));
        return invoke(129, {static_cast<std::uint64_t>(getpid()), number, memory + 128});
    };
    const auto pending = [&] {
        REQUIRE(invoke(127, {memory + 256, 8}) == 0);
        std::uint64_t value{};
        REQUIRE(vm.Memory().CopyIn(&value, memory + 256, 8));
        return value;
    };
    const auto wait = [&](unsigned number, std::uint64_t expected) {
        const auto set = bit(number);
        REQUIRE(vm.Memory().CopyOut(memory + 72, &set, 8));
        REQUIRE(invoke(128, {memory + 72, memory + 384, 0, 8}) == number);
        GuestSignalInfo observed;
        REQUIRE(vm.Memory().CopyIn(&observed, memory + 384, sizeof(observed)));
        std::uint64_t value{};
        std::memcpy(&value, observed.payload.data() + 8, 8);
        CHECK(value == expected);
    };
    REQUIRE(queue(SIGUSR1, 17) == 0);
    CHECK((pending() & bit(SIGUSR1)) != 0);
    wait(SIGUSR1, 17);
    CHECK((pending() & bit(SIGUSR1)) == 0);

    for (unsigned i = 0; i < 4; ++i) REQUIRE(queue(34, i + 1) == 0);
    CHECK(queue(34, 99) == -EAGAIN);
    REQUIRE(queue(35, 71) == 0);
    wait(35, 71);  // A's full, masked FIFO cannot consume B's capacity.
    for (unsigned i = 0; i < 4; ++i) wait(34, i + 1);
    CHECK((pending() & bit(34)) == 0);

    REQUIRE(queue(SIGTSTP, 1) == 0);
    REQUIRE(queue(SIGCONT, 2) == 0);
    CHECK((pending() & bit(SIGTSTP)) == 0);
    REQUIRE(queue(SIGTSTP, 3) == 0);
    CHECK((pending() & bit(SIGCONT)) == 0);
    CHECK(vm.Close().Complete());
}

TEST_CASE("Timed futex internal EINTR retries retain the first deadline", "[compile][signals]") {
    HostRuntime host{false};
    execution::RuntimeStats stats;
    execution::VM vm{{}, stats, host.Bind()};
    const auto memory =
            vm.Memory().Map(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    REQUIRE(memory != UINT64_MAX);
    const std::uint32_t word{};
    const timespec timeout{0, 200000000};
    REQUIRE(vm.Memory().CopyOut(memory, &word, sizeof(word)));
    REQUIRE(vm.Memory().CopyOut(memory + 16, &timeout, sizeof(timeout)));

    struct ActionOwner {
        struct sigaction previous{};
        bool installed{};

        ~ActionOwner() {
            if (installed) sigaction(SIGUSR1, &previous, nullptr);
        }
    } owner;
    struct sigaction action{};
    action.sa_handler = InterruptFutex;
    sigemptyset(&action.sa_mask);
    REQUIRE(sigaction(SIGUSR1, &action, &owner.previous) == 0);
    owner.installed = true;
    saw_futex_signal = 0;
    auto& state = vm.State();
    state.pc = 0x400002;
    state.gprs[x86::RAX] = 202;
    state.gprs[x86::RDI] = memory;
    state.gprs[x86::RSI] = FUTEX_WAIT | FUTEX_PRIVATE_FLAG;
    state.gprs[x86::RDX] = 0;
    state.gprs[x86::R10] = memory + 16;
    LinuxSyscalls service{vm};
    const auto thread = syscall(SYS_gettid);
    std::thread sender{[thread] {
        for (int i = 0; i < 6; ++i) {
            usleep(30000);
            syscall(SYS_tgkill, getpid(), thread, SIGUSR1);
        }
    }};
    const auto begin = std::chrono::steady_clock::now();
    const auto result = service.Service();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    sender.join();
    CHECK(result == SyscallServiceResult::Completed);
    CHECK(static_cast<std::int64_t>(state.gprs[x86::RAX]) == -ETIMEDOUT);
    CHECK(saw_futex_signal != 0);
    // A restarted 200ms interval would finish about 380ms after six signals.
    // Keep a wide scheduler margin; this checks timeout semantics, not speed.
    CHECK(elapsed < std::chrono::milliseconds{340});
    CHECK(vm.Close().Complete());
}

TEST_CASE("HostWait closes the unblock-before-syscall window without consuming IO",
          "[compile][signals]") {
    struct Owner {
        int pipe[2]{-1, -1};
        struct sigaction previous{};
        sigset_t saved{};
        bool mask_saved{}, action_saved{};

        ~Owner() {
            if (action_saved) sigaction(SIGUSR1, &previous, nullptr);
            if (mask_saved) sigprocmask(SIG_SETMASK, &saved, nullptr);
            for (int fd : pipe)
                if (fd >= 0) close(fd);
        }
    } owner;

    REQUIRE(pipe(owner.pipe) == 0);
    sigset_t signal;
    sigemptyset(&signal);
    sigaddset(&signal, SIGUSR1);
    REQUIRE(sigprocmask(SIG_BLOCK, &signal, &owner.saved) == 0);
    owner.mask_saved = true;
    struct sigaction action{};
    action.sa_sigaction = CancelWaitSignal;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    REQUIRE(sigaction(SIGUSR1, &action, &owner.previous) == 0);
    owner.action_saved = true;
    constexpr char input[] = "abc";
    REQUIRE(write(owner.pipe[1], input, 3) == 3);
    REQUIRE(raise(SIGUSR1) == 0);  // Already pending before the stub restores mask.
    char output[8]{};
    HostWaitRequest request;
    request.number = SYS_read;
    request.arguments[0] = owner.pipe[0];
    request.arguments[1] = reinterpret_cast<std::uint64_t>(output);
    request.arguments[2] = sizeof(output);
    for (int n = 1; n <= 64; ++n)
        if (n != SIGUSR1 && sigismember(&owner.saved, n) == 1)
            request.mask |= std::uint64_t{1} << (n - 1);
    CHECK(SvmHostWaitSyscall(&request) == CancelledBeforeCall);
    CHECK(SvmHostWaitSyscall(&request) == 3);
    CHECK(std::memcmp(output, input, 3) == 0);
}

TEST_CASE("Linux x86 signal frame preserves xstate and restores edited context",
          "[compile][signals]") {
    execution::GuestMemory memory;
    const auto buffer = memory.Map(0, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    REQUIRE(buffer != UINT64_MAX);
    x86::GuestState original;
    original.pc = 0x400000;
    original.gprs[x86::RSP] = buffer + 16384;
    original.gprs[x86::RAX] = 0x1234;
    original.fs_base = 0x7890;
    original.packed_flags = x86::flags::Carry | x86::flags::Direction;
    original.mxcsr = 0x3f85;
    original.xmm[0][0] = 0x31;
    original.ymm_high[0][0] = 0x72;
    original.x87.status = 3u << 11;
    original.x87.tags &= ~(3u << 6);
    original.x87.registers[3].significand = UINT64_C(0x8000000000000000);
    original.x87.registers[3].sign_exp = 0x3fff;
    GuestExceptionInfo event;
    event.kind = GuestExceptionKind::FloatingPoint;
    event.exception_vector = 16;  // x87 #MF differs from SIMD #XM (19).
    event.syndrome = 4;           // Raised FP causes are not sigcontext ERR bits.
    event.syndrome_valid = true;
    event.address_valid = true;
    event.address = 1;
    GuestSignalInfo info;
    info.number = 11;
    GuestSignalAction action{0x500000, 4, 0, 0};
    SignalFrameContext saved{0x1122, {}};
    EncodedSignalFrame frame;
    x86::GuestState handler;
    REQUIRE(EncodeX86SignalFrame(original, event, info, action, saved, 0x600000, frame, handler));
    CHECK((handler.gprs[x86::RSP] & 15) == 8);
    CHECK(handler.mxcsr == 0x1f80);
    CHECK(handler.x87.control == 0x37f);
    CHECK(handler.x87.tags == 0xffff);
    CHECK(handler.xmm[0][0] == 0);
    CHECK(handler.ymm_high[0][0] == 0);
    CHECK_FALSE(x86::flags::GetDF(handler.packed_flags));
    REQUIRE(memory.CopyOut(frame.address, frame.bytes.data(), frame.bytes.size()));
    std::uint64_t error_code{}, trap_number{};
    REQUIRE(memory.CopyIn(&error_code, frame.address + 8 + 40 + 19 * 8, 8));
    REQUIRE(memory.CopyIn(&trap_number, frame.address + 8 + 40 + 20 * 8, 8));
    CHECK(error_code == 0);
    CHECK(trap_number == 16);
    // Linux x86 sigcontext RIP is ucontext+40+16*8, not a private struct offset.
    const std::uint64_t edited_pc = 0x400007;
    REQUIRE(memory.CopyOut(frame.address + 8 + 40 + 16 * 8, &edited_pc, 8));
    std::uint64_t fp_address{};
    REQUIRE(memory.CopyIn(&fp_address, frame.address + 8 + 40 + 23 * 8, 8));
    CHECK((fp_address & 63) == 0);
    const std::uint8_t upper = 0x5a;
    REQUIRE(memory.CopyOut(fp_address + 576, &upper, 1));
    handler.gprs[x86::RSP] += 8;  // Handler RET popped the restorer address.
    x86::GuestState restored;
    SignalFrameContext context;
    REQUIRE(DecodeX86SignalFrame(memory, handler, restored, context));
    CHECK(restored.pc == edited_pc);
    CHECK(restored.gprs[x86::RAX] == original.gprs[x86::RAX]);
    CHECK(restored.fs_base == original.fs_base);
    CHECK(restored.mxcsr == original.mxcsr);
    CHECK(restored.xmm[0][0] == 0x31);
    CHECK(restored.ymm_high[0][0] == upper);
    CHECK(restored.x87.tags == original.x87.tags);
    CHECK(restored.x87.registers[3].significand == original.x87.registers[3].significand);
    CHECK(context.mask == saved.mask);
    const std::uint64_t unknown_feature = 8;
    REQUIRE(memory.CopyOut(fp_address + 512, &unknown_feature, 8));
    CHECK_FALSE(DecodeX86SignalFrame(memory, handler, restored, context));
}

TEST_CASE("Linux ARM64 signal frame restores edited FPSIMD and validates records",
          "[compile][signals]") {
    execution::GuestMemory memory;
    const auto buffer = memory.Map(0, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    REQUIRE(buffer != UINT64_MAX);
    arm64::GuestState original;
    original.pc = 0x400000;
    original.gprs[31] = buffer + 16384;
    original.gprs[19] = 0x1234;
    original.packed_flags = 0xa0000000;
    original.q[2][0] = 0x31;
    original.fpcr = 1u << 22;
    original.fpsr = 8;
    GuestExceptionInfo event;
    event.isa = GuestISA::Arm64;
    GuestSignalInfo info;
    info.number = 11;
    GuestSignalAction action{0x500000, 4, 0, 0};
    SignalFrameContext saved{0x1122, {}};
    EncodedSignalFrame frame;
    arm64::GuestState handler;
    REQUIRE(EncodeArm64SignalFrame(original, event, info, action, saved, 0x600000, frame, handler));
    CHECK((handler.gprs[31] & 15) == 0);
    CHECK(handler.fpcr == original.fpcr);
    CHECK(handler.fpsr == original.fpsr);
    REQUIRE(memory.CopyOut(frame.address, frame.bytes.data(), frame.bytes.size()));
    const std::uint64_t edited_pc = 0x400008;
    REQUIRE(memory.CopyOut(frame.address + 128 + 176 + 264, &edited_pc, 8));
    const std::uint8_t vector_byte = 0x5a;
    REQUIRE(memory.CopyOut(frame.address + 128 + 176 + 288 + 16 + 2 * 16, &vector_byte, 1));
    arm64::GuestState restored;
    SignalFrameContext context;
    REQUIRE(DecodeArm64SignalFrame(memory, handler, restored, context));
    CHECK(restored.pc == edited_pc);
    CHECK(restored.gprs[19] == original.gprs[19]);
    CHECK(restored.q[2][0] == vector_byte);
    CHECK(restored.fpcr == original.fpcr);
    CHECK(context.mask == saved.mask);
    const std::uint32_t invalid_size = 16;
    REQUIRE(memory.CopyOut(frame.address + 128 + 176 + 288 + 4, &invalid_size, 4));
    CHECK_FALSE(DecodeArm64SignalFrame(memory, handler, restored, context));
}
}  // namespace
