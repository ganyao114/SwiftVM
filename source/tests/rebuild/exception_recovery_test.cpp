#include "runtime/arch/x86/state.h"
#include "runtime/execution/runtime.h"
#include "runtime/execution/vm.h"
#include "runtime/platform/linux/host_runtime.h"
#include "tests/support/check.h"
#include "tests/support/test_mode.h"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <span>
#include <thread>
#include <vector>
#include <elf.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using swift::tests::Check;
namespace execution = swift::runtime::execution;
namespace ir = swift::runtime::ir;

// Linux x86-64 UAPI, independent of the runtime's signal-frame encoder. The
// ucontext's 23 gregs start after flags, link and the 24-byte stack_t. These are
// the kernel ABI's REG_R12/REG_RAX/REG_RIP/REG_TRAPNO slots and fpregs pointer.
constexpr std::uint32_t context_r12 = 40 + 4 * 8, context_rax = 40 + 13 * 8,
                        context_rsp = 40 + 15 * 8, context_rip = 40 + 16 * 8,
                        context_trap = 40 + 20 * 8, context_fp = 40 + 23 * 8,
                        context_mask = 40 + 256;
constexpr std::uint32_t fxsave_mxcsr = 24, fxsave_xmm0 = 160;
constexpr std::uint64_t sa_siginfo = 4, sa_restorer = 0x04000000;
constexpr std::uint64_t image_base = 0x400000;
constexpr std::size_t image_headers = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
constexpr std::uint64_t load_value = 0x8877665544332211, old_r12 = 0x1122334455667788;
constexpr std::array<std::uint64_t, 2> restored_xmm{0x0123456789abcdef, 0xfedcba9876543210};

// Test-owned output layout. Guest stores below use these offsets, never the
// product's signal-frame structures.
enum Output : std::uint8_t {
    Installed = 0,
    HandlerCount = 8,
    Loaded = 16,
    ResumedRax = 24,
    Signal = 32,
    Code = 40,
    Address = 48,
    SavedRip = 56,
    SavedRax = 64,
    Trap = 72,
    SavedMxcsr = 80,
    SavedR12 = 88,
    ResumedMxcsr = 96,
    ResumedXmm = 112,
};

class GuestCode {
public:
    std::uint64_t PC() const { return image_base + image_headers + bytes.size(); }

    void Emit(std::initializer_list<std::uint8_t> instruction) {
        bytes.insert(bytes.end(), instruction);
    }

    void U32(std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes.push_back(value >> (i * 8));
    }

    void U64(std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) bytes.push_back(value >> (i * 8));
    }

    void MovAbs(unsigned reg, std::uint64_t value) {
        Emit({static_cast<std::uint8_t>(reg >= 8 ? 0x49 : 0x48),
              static_cast<std::uint8_t>(0xb8 + (reg & 7))});
        U64(value);
    }

    void StoreRax(Output output) { Emit({0x48, 0x89, 0x43, output}); }  // [RBX+disp8].

    void ReadContext(std::uint32_t offset, Output output) {
        Emit({0x48, 0x8b, 0x82});  // MOV RAX,[RDX+disp32].
        U32(offset);
        StoreRax(output);
    }

    void WriteContext(std::uint32_t offset, std::uint64_t value) {
        MovAbs(0, value);
        Emit({0x48, 0x89, 0x82});  // MOV [RDX+disp32],RAX.
        U32(offset);
    }

    void Exit(std::uint8_t result) { Emit({0xb8, 60, 0, 0, 0, 0xbf, result, 0, 0, 0, 0x0f, 0x05}); }

    void Once(Output counter) {
        Emit({0x48, 0xff, 0x43, counter});
        Emit({0x48, 0x83, 0x7b, counter, 1, 0x74, 12});
        Exit(99);
    }

    void Install(unsigned signal, std::uint64_t action, std::uint64_t output) {
        Emit({0xb8, 13, 0, 0, 0, 0xbf});  // rt_sigaction(signal,action,nullptr,8).
        U32(signal);
        MovAbs(6, action);  // RSI.
        Emit({0x31, 0xd2, 0x41, 0xba, 8, 0, 0, 0, 0x0f, 0x05});
        MovAbs(3, output);  // RBX survives syscall and is saved in the frame.
        StoreRax(Installed);
    }

    void HandlerPrefix(std::uint64_t output) {
        MovAbs(3, output);
        Emit({0x48, 0xff, 0x43, HandlerCount});  // INC qword [RBX+8].
        // Bound a broken retry to a failing exit instead of an infinite test.
        Emit({0x48, 0x83, 0x7b, HandlerCount, 1, 0x74, 12});
        Exit(99);
        Emit({0x48, 0x89, 0x7b, Signal});  // RDI = signal number.
        Emit({0x8b, 0x46, 8});             // EAX = siginfo.si_code.
        StoreRax(Code);
        Emit({0x48, 0x8b, 0x46, 16});  // RAX = siginfo.si_addr.
        StoreRax(Address);
        ReadContext(context_rip, SavedRip);
        ReadContext(context_rax, SavedRax);
        ReadContext(context_trap, Trap);
        ReadContext(context_r12, SavedR12);
    }

    std::uint64_t Restorer() {
        const auto pc = PC();
        Emit({0xb8, 15, 0, 0, 0, 0x0f, 0x05, 0x0f, 0x0b});  // rt_sigreturn; UD2.
        return pc;
    }

    std::vector<std::uint8_t> Image(unsigned machine = EM_X86_64) const {
        Elf64_Ehdr header{};
        std::memcpy(header.e_ident, ELFMAG, SELFMAG);
        header.e_ident[EI_CLASS] = ELFCLASS64;
        header.e_ident[EI_DATA] = ELFDATA2LSB;
        header.e_ident[EI_VERSION] = EV_CURRENT;
        header.e_type = ET_EXEC;
        header.e_machine = machine;
        header.e_version = EV_CURRENT;
        header.e_entry = image_base + image_headers;
        header.e_phoff = sizeof(Elf64_Ehdr);
        header.e_ehsize = sizeof(Elf64_Ehdr);
        header.e_phentsize = sizeof(Elf64_Phdr);
        header.e_phnum = 1;
        Elf64_Phdr segment{};
        segment.p_type = PT_LOAD;
        segment.p_flags = PF_R | PF_X;  // Immutable source permits actual Function formation.
        segment.p_vaddr = segment.p_paddr = image_base;
        segment.p_filesz = segment.p_memsz = image_headers + bytes.size();
        segment.p_align = 4096;
        std::vector<std::uint8_t> image(segment.p_filesz);
        std::memcpy(image.data(), &header, sizeof(header));
        std::memcpy(image.data() + sizeof(header), &segment, sizeof(segment));
        std::memcpy(image.data() + image_headers, bytes.data(), bytes.size());
        return image;
    }

private:
    std::vector<std::uint8_t> bytes;
};

// The ARM64 ucontext has a 128-byte sigset area followed by the 16-byte
// aligned mcontext. Linux v6.12 UAPI sigcontext starts with fault_address,
// regs[31], SP, PC and PSTATE. These constants are test-owned ABI facts.
constexpr std::uint32_t arm_context_x0 = 184, arm_context_pc = 440, arm_context_pstate = 448;

class ArmGuestCode : public GuestCode {
public:
    void Move(unsigned reg, std::uint64_t value) {
        U32(0xd2800000 | ((value & 0xffff) << 5) | reg);  // MOVZ Xd,#imm16.
        for (unsigned half = 1; half < 4; ++half)
            U32(0xf2800000 | (half << 21) | (((value >> (half * 16)) & 0xffff) << 5) | reg);
    }

    void Load(unsigned reg, unsigned base, unsigned offset) {
        U32(0xf9400000 | ((offset / 8) << 10) | (base << 5) | reg);
    }

    void Store(unsigned reg, unsigned base, unsigned offset) {
        U32(0xf9000000 | ((offset / 8) << 10) | (base << 5) | reg);
    }

    void Svc() { U32(0xd4000001); }

    void Exit(unsigned result) {
        Move(0, result);
        Move(8, 93);
        Svc();
    }

    void Install(std::uint64_t action, std::uint64_t output) {
        Move(8, 134);  // rt_sigaction(SIGSEGV,action,nullptr,8).
        Move(0, 11);
        Move(1, action);
        Move(2, 0);
        Move(3, 8);
        Svc();
        Move(19, output);
        Store(0, 19, Installed);
    }

    void ReadContext(unsigned offset, Output output) {
        Load(10, 2, offset);
        Store(10, 19, output);
    }

    void Handler(std::uint64_t output, std::uint64_t valid_load) {
        Move(19, output);
        Load(10, 19, HandlerCount);
        U32(0x9100054a);  // ADD X10,X10,#1.
        Store(10, 19, HandlerCount);
        U32(0xf100055f);  // CMP X10,#1.
        U32(0x54000140);  // B.EQ past the nine-instruction failure exit.
        Exit(99);
        Store(0, 19, Signal);
        U32(0xb940082a);  // LDR W10,[X1,#8]: siginfo.si_code.
        Store(10, 19, Code);
        Load(10, 1, 16);
        Store(10, 19, Address);
        ReadContext(arm_context_pc, SavedRip);
        ReadContext(arm_context_x0, SavedRax);
        ReadContext(arm_context_x0 + 14 * 8, SavedR12);
        ReadContext(arm_context_pstate, Trap);
        Move(10, valid_load);
        Store(10, 2, arm_context_x0);
        U32(0xd65f03c0);  // RET X30 to the guest restorer.
    }

    std::uint64_t Restorer() {
        const auto pc = PC();
        Move(8, 139);
        Svc();
        U32(0xd4400000);  // HLT: sigreturn must not return here.
        return pc;
    }
};

enum class Recovery { RetryLoad, SkipDivide };

void SignalReturn(ir::Scope scope, Recovery recovery) {
    INFO("scope=" << (scope == ir::Scope::Block ? "Block" : "Function"));
    swift::runtime::platform::HostRuntime host{false};
    execution::RuntimeOptions options;
    options.mode = execution::ExecutionMode::JitOnly;
    options.scope = scope;
    execution::RuntimeStats stats;
    execution::VM vm{options, stats, host.Bind()};
    auto& memory = vm.Memory();
    const auto data =
            memory.Map(0, memory.PageSize(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    Check(data != UINT64_MAX, "allocate independent guest action, output and valid load data");
    const auto output = data + 64, valid_load = data + 512;
    const bool retry = recovery == Recovery::RetryLoad;
    const unsigned signal = retry ? 11 : 8;  // Linux SIGSEGV / SIGFPE.
    GuestCode code;
    code.Install(signal, data, output);
    if (retry) {
        code.Emit({0xf3, 0x0f, 0x5e, 0xc1});  // DIVSS XMM0,XMM1 raises masked ZE.
        code.MovAbs(12, old_r12);
        code.Emit({0x31, 0xc0});  // XOR EAX,EAX: null is the fault address.
    } else {
        code.Emit({0xb8, 123, 0, 0, 0, 0x31, 0xd2, 0x31, 0xc9});  // EDX:EAX=123, ECX=0.
    }
    const auto fault_pc = code.PC();
    if (retry)
        code.Emit({0x4c, 0x8b, 0x20});  // MOV R12,[RAX].
    else
        code.Emit({0xf7, 0xf1});  // DIV ECX.
    const auto after_fault = code.PC();
    code.Emit({0x4c, 0x89, 0x63, Loaded});  // MOV [RBX+16],R12.
    code.StoreRax(ResumedRax);
    if (retry) {
        code.Emit({0x0f, 0xae, 0x5b, ResumedMxcsr});      // STMXCSR [RBX+96].
        code.Emit({0xf3, 0x0f, 0x7f, 0x43, ResumedXmm});  // MOVDQU [RBX+112],XMM0.
    }
    code.Exit(42);

    const auto handler = code.PC();
    code.HandlerPrefix(output);
    if (retry) {
        // Keep the saved RIP untouched: sigreturn must dispatch the same load
        // again with the corrected address, rather than resume host code.
        code.WriteContext(context_rax, valid_load);
        code.Emit({0x48, 0x8b, 0x82});  // MOV RAX,[RDX+ucontext.fpregs].
        code.U32(context_fp);
        code.Emit({0x8b, 0x48, fxsave_mxcsr});  // ECX = frame MXCSR.
        code.Emit({0x48, 0x89, 0x4b, SavedMxcsr});
        code.MovAbs(1, restored_xmm[0]);
        code.Emit({0x48, 0x89, 0x88});  // MOV [RAX+FXSAVE.XMM0],RCX.
        code.U32(fxsave_xmm0);
        code.MovAbs(1, restored_xmm[1]);
        code.Emit({0x48, 0x89, 0x88});
        code.U32(fxsave_xmm0 + 8);
        code.Emit({0xc7, 0x40, fxsave_mxcsr});  // Clear all sticky bits in the saved MXCSR.
        code.U32(0x1f80);
    } else {
        code.WriteContext(context_rip, after_fault);  // Skip the faulting DIV, preserving RAX.
    }
    code.Emit({0xc3});  // RET to the action's guest restorer.
    const auto restorer = code.Restorer();

    // Linux kernel sigaction: handler, flags, restorer, 64-bit signal mask.
    const std::array<std::uint64_t, 4> action{handler, sa_siginfo | sa_restorer, restorer, 0};
    Check(memory.CopyOut(data, action.data(), sizeof(action)) &&
                  memory.CopyOut(valid_load, &load_value, sizeof(load_value)),
          "initialize the guest-owned Linux ABI action and retry data");
    const auto image = code.Image();
    vm.PrepareGuest(image, "exception-recovery-test");
    const std::array<std::uint32_t, 4> left{0x3f800000, 0x12345678, 0x9abcdef0, 0xa5a55a5a},
            right{0, 0x87654321, 0x0fedcba9, 0x5a5aa5a5};
    if (retry) {
        std::memcpy(vm.State().xmm[0], left.data(), sizeof(left));
        std::memcpy(vm.State().xmm[1], right.data(), sizeof(right));
        vm.State().mxcsr = 0x1fa0;  // Existing PE, before DIVSS newly raises ZE.
    }
    Check(vm.RunPrepared() == 42 && vm.StopReason() == swift::runtime::GuestStopReason::GuestExit,
          "guest handler returns through rt_sigreturn and completes natively");
    std::array<std::uint64_t, 16> result{};
    Check(memory.CopyIn(result.data(), output, sizeof(result)), "read guest handler observations");
    const auto observed = [&](Output field) { return result[field / 8]; };
    Check(stats.interpreted_steps == 0 && stats.native_bytes && stats.code_units_published &&
                  stats.syscalls == 3,
          "installation, sigreturn and exit all run in JitOnly without interpreting");
    if (scope == ir::Scope::Function)
        Check(stats.function_attempts != 0, "Function scope reaches actual function compilation");
    Check(observed(Installed) == 0 && observed(HandlerCount) == 1 && observed(Signal) == signal &&
                  observed(Code) == 1,
          "rt_sigaction installs once and reports SEGV_MAPERR or FPE_INTDIV through siginfo");
    Check(observed(SavedRip) == fault_pc && observed(SavedRax) == (retry ? 0 : 123) &&
                  observed(Trap) == (retry ? 14 : 0),
          "independent Linux ucontext offsets expose the precise pre-fault RIP, RAX and cause");
    if (retry) {
        Check(observed(Address) == 0 && observed(SavedR12) == old_r12 &&
                      observed(Loaded) == load_value && observed(ResumedRax) == valid_load,
              "faulting load leaves its destination unchanged and retries with the frame's RAX");
        Check(observed(SavedMxcsr) == 0x1fa4 && observed(ResumedMxcsr) == 0x1f80 &&
                      vm.State().mxcsr == 0x1f80,
              "signal frame captures PE plus new ZE and clearing sticky bits survives sigreturn");
        std::array<std::uint64_t, 2> final_xmm{};
        std::array<std::uint32_t, 4> final_xmm1{};
        std::memcpy(final_xmm.data(), vm.State().xmm[0], sizeof(final_xmm));
        std::memcpy(final_xmm1.data(), vm.State().xmm[1], sizeof(final_xmm1));
        Check(observed(ResumedXmm) == restored_xmm[0] &&
                      result[ResumedXmm / 8 + 1] == restored_xmm[1] && final_xmm == restored_xmm &&
                      final_xmm1 == right,
              "edited XMM0 and untouched XMM1 restore from the guest frame through dispatch");
    } else {
        Check(observed(ResumedRax) == 123,
              "handler skips DIV through frame RIP without ordinary syscall RAX writeback");
    }
    Check(vm.Close().Complete(), "signal recovery fixture releases all VM mappings");
}

void ArmSignalReturn(ir::Scope scope) {
    INFO("scope=" << (scope == ir::Scope::Block ? "Block" : "Function"));
    swift::runtime::platform::HostRuntime host{false};
    execution::RuntimeOptions options;
    options.mode = execution::ExecutionMode::JitOnly;
    options.scope = scope;
    execution::RuntimeStats stats;
    execution::VM vm{options, stats, host.Bind()};
    auto& memory = vm.Memory();
    const auto data =
            memory.Map(0, memory.PageSize(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    Check(data != UINT64_MAX, "allocate ARM64 action, observations and load source");
    const auto output = data + 64, valid_load = data + 512;
    ArmGuestCode code;
    code.Install(data, output);
    code.Move(0, 0);
    code.Move(14, old_r12);
    code.U32(0x92800009);  // MOV X9,#-1.
    code.U32(0xb100053f);  // ADDS XZR,X9,#1: Z=C=1, N=V=0.
    const auto fault_pc = code.PC();
    code.U32(0xf940000e);  // LDR X14,[X0].
    code.Store(14, 19, Loaded);
    code.Store(0, 19, ResumedRax);
    code.U32(0xd53b420a);  // MRS X10,NZCV.
    code.Store(10, 19, ResumedMxcsr);
    code.Exit(42);
    const auto handler = code.PC();
    code.Handler(output, valid_load);
    const auto restorer = code.Restorer();
    const std::array<std::uint64_t, 4> action{handler, sa_siginfo | sa_restorer, restorer, 0};
    Check(memory.CopyOut(data, action.data(), sizeof(action)) &&
                  memory.CopyOut(valid_load, &load_value, sizeof(load_value)),
          "initialize independent ARM64 Linux ABI action and retry source");
    vm.PrepareGuest(code.Image(EM_AARCH64), "arm64-exception-recovery-test");
    Check(vm.RunPrepared() == 42 && vm.StopReason() == swift::runtime::GuestStopReason::GuestExit,
          "ARM64 guest retries the same LDR after handler and rt_sigreturn");
    std::array<std::uint64_t, 16> result{};
    Check(memory.CopyIn(result.data(), output, sizeof(result)), "read ARM64 signal observations");
    const auto observed = [&](Output field) { return result[field / 8]; };
    Check(observed(Installed) == 0 && observed(HandlerCount) == 1 && observed(Signal) == 11 &&
                  observed(Code) == 1 && observed(Address) == 0,
          "ARM64 action reports one SIGSEGV/SEGV_MAPERR for the null load");
    Check(observed(SavedRip) == fault_pc && observed(SavedRax) == 0 &&
                  observed(SavedR12) == old_r12 && observed(Trap) == 0x60000000,
          "ARM64 frame exposes fault PC, old destination and architectural NZCV");
    Check(observed(Loaded) == load_value && observed(ResumedRax) == valid_load &&
                  observed(ResumedMxcsr) == 0x60000000,
          "edited X0 and saved NZCV restore without ordinary syscall X0 writeback");
    Check(stats.interpreted_steps == 0 && stats.native_bytes && stats.code_units_published &&
                  stats.syscalls == 3 &&
                  (scope != ir::Scope::Function || stats.function_attempts != 0),
          "ARM64 Block/Function recovery stays in JitOnly and uses three guest syscalls");
    Check(vm.Close().Complete(), "ARM64 signal fixture releases all VM mappings");
}

void AsyncSignalReturn(ir::Scope scope, bool indirect = false) {
    swift::runtime::platform::HostRuntime host{false};
    execution::RuntimeOptions options;
    options.mode = execution::ExecutionMode::JitOnly;
    options.scope = scope;
    execution::RuntimeStats stats;
    execution::VM vm{options, stats, host.Bind()};
    auto& memory = vm.Memory();
    const auto data =
            memory.Map(0, memory.PageSize(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    Check(data != UINT64_MAX, "allocate asynchronous signal observations");
    const auto output = data + 64;
    const auto target_slot = data + 512;
    GuestCode code;
    code.Install(10, data, output);  // SIGUSR1.
    if (indirect) code.MovAbs(13, target_slot);
    const auto loop = code.PC();
    code.Emit({0x48, 0xff, 0x43, Loaded});  // INC [RBX+16].
    if (indirect)
        code.Emit({0x41, 0xff, 0x65, 0});  // JMP qword [R13]: real dynamic L1 dispatch.
    else
        code.Emit({0xeb, 0xfa});  // JMP loop.
    const auto continuation = code.PC();
    code.Exit(42);
    const auto handler = code.PC();
    code.HandlerPrefix(output);
    code.WriteContext(context_rip, continuation);
    code.Emit({0xc3});
    const auto restorer = code.Restorer();
    const std::array<std::uint64_t, 4> action{handler, sa_siginfo | sa_restorer, restorer, 0};
    Check(memory.CopyOut(data, action.data(), sizeof(action)) &&
                  (!indirect || memory.CopyOut(target_slot, &loop, sizeof(loop))),
          "initialize async Linux sigaction and indirect target");
    vm.PrepareGuest(code.Image(), "async-exception-recovery-test");
    const auto target_thread = syscall(SYS_gettid);
    std::atomic<bool> done{}, sent{};
    std::thread sender{[&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (!done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::uint64_t iterations{};
            iovec local{&iterations, sizeof(iterations)};
            iovec remote{reinterpret_cast<void*>(output + Loaded), sizeof(iterations)};
            // Copy through the kernel instead of racing an ordinary C++ load
            // against native guest writes. Signal only after the real loop ran.
            if (syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) ==
                        sizeof(iterations) &&
                iterations >= 128) {
                sent.store(syscall(SYS_tgkill, getpid(), target_thread, SIGUSR1) == 0,
                           std::memory_order_release);
                return;
            }
            usleep(1000);
        }
    }};
    int result{};
    try {
        result = vm.RunPrepared();
    } catch (...) {
        done.store(true, std::memory_order_release);
        sender.join();
        throw;
    }
    done.store(true, std::memory_order_release);
    sender.join();
    Check(result == 42 && sent.load(std::memory_order_acquire) &&
                  vm.StopReason() == swift::runtime::GuestStopReason::GuestExit,
          "host tgkill interrupts a live native guest loop and its handler completes");
    std::array<std::uint64_t, 16> observed{};
    Check(memory.CopyIn(observed.data(), output, sizeof(observed)), "read async signal results");
    Check(observed[Installed / 8] == 0 && observed[HandlerCount / 8] == 1 &&
                  observed[Signal / 8] == 10 &&
                  static_cast<std::int32_t>(observed[Code / 8]) == SI_TKILL &&
                  observed[Loaded / 8] >= 128,
          "asynchronous signal arrives after installation and committed loop progress");
    Check(observed[SavedRip / 8] == loop || observed[SavedRip / 8] == loop + 4,
          "async signal exposes a restartable loop boundary, not an arbitrary host PC");
    Check(stats.interpreted_steps == 0 && stats.syscalls == 3 && stats.native_bytes &&
                  (scope != ir::Scope::Function || stats.function_attempts != 0),
          "asynchronous Block/Function delivery uses native safepoints without interpretation");
    Check(vm.Close().Complete(), "async signal fixture releases all VM mappings");
#if defined(__aarch64__)
    if (indirect) {
        Check(observed[SavedRip / 8] == loop,
              "L1 interruption resumes after the indirect jump at its actual target");
        Check(stats.dispatch_interrupts > 0,
              "the asynchronous request exits through the protected L1 lookup");
    }
#endif
}

void BoundedAsyncSignalReturn(ir::Scope scope, bool indirect = false) {
    // An implementation that never polls must fail this case without hanging
    // the runner. The isolated child owns the VM, sender and signal actions.
    const auto child = fork();
    Check(child >= 0, "fork isolated asynchronous recovery case");
    if (!child) {
        try {
            AsyncSignalReturn(scope, indirect);
            _exit(0);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "async recovery child: %s\n", error.what());
        } catch (...) {
            std::fputs("async recovery child failed\n", stderr);
        }
        _exit(1);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{8};
    int status{};
    while (waitpid(child, &status, WNOHANG) == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            break;
        }
        usleep(1000);
    }
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "asynchronous native loop handles its signal within the bounded test lifetime");
}

void NestedSignalReturn() {
    swift::runtime::platform::HostRuntime host{false};
    execution::RuntimeOptions options;
    options.mode = execution::ExecutionMode::JitOnly;
    options.scope = ir::Scope::Function;
    execution::RuntimeStats stats;
    execution::VM vm{options, stats, host.Bind()};
    auto& memory = vm.Memory();
    const auto data =
            memory.Map(0, memory.PageSize(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    constexpr std::uint64_t alternate_bytes = 16384;
    const auto alternate =
            memory.Map(0, alternate_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    Check(data != UINT64_MAX && alternate != UINT64_MAX,
          "allocate nested signal actions, observations and alternate guest stack");
    const auto descriptor = data + 64, initial_mask_address = data + 88,
               final_mask_address = data + 104, output = data + 256;
    constexpr std::uint64_t initial_mask = std::uint64_t{1} << (14 - 1);  // SIGALRM stays blocked.
    constexpr auto first_install = static_cast<Output>(120);

    GuestCode code;
    code.Install(10, data, output);  // Outer SIGUSR1.
    code.StoreRax(first_install);
    code.Install(12, data + 32, output);  // Inner SIGUSR2.
    code.MovAbs(0, 131);                  // sigaltstack(&descriptor,nullptr).
    code.MovAbs(7, descriptor);
    code.Emit({0x31, 0xf6, 0x0f, 0x05});
    code.MovAbs(0, 14);  // rt_sigprocmask(SIG_BLOCK,&initial_mask,nullptr,8).
    code.Emit({0x31, 0xff});
    code.MovAbs(6, initial_mask_address);
    code.Emit({0x31, 0xd2, 0x41, 0xba, 8, 0, 0, 0, 0x0f, 0x05});
    code.Emit({0x48, 0x89, 0xe0});  // Record the original, ordinary guest RSP.
    code.StoreRax(SavedMxcsr);
    code.MovAbs(0, 39);  // getpid(), retained in R12 by both signal frames.
    code.Emit({0x0f, 0x05, 0x49, 0x89, 0xc4});
    code.Emit({0x44, 0x89, 0xe7, 0xbe, 10, 0, 0, 0, 0xb8, 62, 0, 0, 0, 0x0f, 0x05});
    // Both nested handlers and their rt_sigreturn calls must finish before
    // this continuation, restoring the ordinary stack and the original mask.
    code.Emit({0x48, 0x89, 0xe0});
    code.StoreRax(Trap);
    code.MovAbs(0, 14);  // Query current guest mask without changing it.
    code.Emit({0x31, 0xff, 0x31, 0xf6});
    code.MovAbs(2, final_mask_address);
    code.Emit({0x41, 0xba, 8, 0, 0, 0, 0x0f, 0x05});
    code.Exit(42);

    const auto outer = code.PC();
    code.MovAbs(3, output);
    code.Once(HandlerCount);
    code.MovAbs(0, 1);
    code.StoreRax(ResumedRax);  // Phase 1: outer handler entered.
    code.Emit({0x48, 0x89, 0xe0});
    code.StoreRax(Signal);  // Outer entry RSP.
    code.ReadContext(context_rsp, Address);
    code.ReadContext(context_mask, SavedR12);
    code.Emit({0x44, 0x89, 0xe7, 0xbe, 12, 0, 0, 0, 0xb8, 62, 0, 0, 0, 0x0f, 0x05});
    code.Emit({0x48, 0x89, 0xe0});
    code.StoreRax(SavedRax);  // Outer RSP after the inner frame was removed.
    code.Emit({0x48, 0x83, 0x7b, ResumedRax, 2, 0x74, 12});
    code.Exit(99);
    code.MovAbs(0, 3);
    code.StoreRax(ResumedRax);  // Phase 3: outer resumed after the inner return.
    code.Emit({0xc3});

    const auto inner = code.PC();
    code.MovAbs(3, output);
    code.Once(Loaded);
    code.Emit({0x48, 0x83, 0x7b, ResumedRax, 1, 0x74, 12});
    code.Exit(99);
    code.MovAbs(0, 2);
    code.StoreRax(ResumedRax);  // Phase 2 can only execute inside phase 1.
    code.Emit({0x48, 0x89, 0xe0});
    code.StoreRax(Code);
    code.ReadContext(context_rsp, SavedRip);
    code.ReadContext(context_mask, ResumedMxcsr);
    code.Emit({0xc3});
    const auto restorer = code.Restorer();

    constexpr std::uint64_t on_stack = 0x08000000;
    const auto action_flags = sa_siginfo | sa_restorer | on_stack;
    const std::array<std::uint64_t, 4> outer_action{outer, action_flags, restorer, 0},
            inner_action{inner, action_flags, restorer, 0};
    // Linux x86-64 stack_t: address, 32-bit flags/padding, byte count.
    const std::array<std::uint64_t, 3> alternate_descriptor{alternate, 0, alternate_bytes};
    Check(memory.CopyOut(data, outer_action.data(), sizeof(outer_action)) &&
                  memory.CopyOut(data + 32, inner_action.data(), sizeof(inner_action)) &&
                  memory.CopyOut(
                          descriptor, alternate_descriptor.data(), sizeof(alternate_descriptor)) &&
                  memory.CopyOut(initial_mask_address, &initial_mask, sizeof(initial_mask)),
          "initialize independent guest Linux actions, stack_t and signal mask");
    vm.PrepareGuest(code.Image(), "nested-altstack-signal-test");
    Check(vm.RunPrepared() == 42 && vm.StopReason() == swift::runtime::GuestStopReason::GuestExit,
          "both nested signal returns resume their respective guest continuations");
    std::array<std::uint64_t, 16> result{};
    std::uint64_t final_mask{};
    Check(memory.CopyIn(result.data(), output, sizeof(result)) &&
                  memory.CopyIn(&final_mask, final_mask_address, sizeof(final_mask)),
          "read nested alternate-stack observations and final guest mask");
    const auto observed = [&](Output field) { return result[field / 8]; };
    const auto outer_sp = observed(Signal), inner_sp = observed(Code);
    Check(observed(Installed) == 0 && observed(first_install) == 0 && observed(HandlerCount) == 1 &&
                  observed(Loaded) == 1 && observed(ResumedRax) == 3,
          "one outer and one nested handler execute in the required order");
    Check(inner_sp >= alternate && inner_sp < outer_sp && outer_sp < alternate + alternate_bytes,
          "SA_ONSTACK places both frames in the guest alternate stack, nested frame deeper");
    Check(observed(Address) == observed(SavedMxcsr) && observed(SavedRip) == outer_sp &&
                  observed(SavedRax) == outer_sp && observed(Trap) == observed(SavedMxcsr) &&
                  vm.State().gprs[swift::runtime::x86::RSP] == observed(SavedMxcsr),
          "nested sigreturn restores outer RSP, then outer sigreturn restores the original RSP");
    Check(observed(SavedR12) == initial_mask &&
                  observed(ResumedMxcsr) == (initial_mask | (std::uint64_t{1} << (10 - 1))) &&
                  final_mask == initial_mask,
          "each frame saves its prior guest mask and both returns remove only handler masking");
    Check(stats.interpreted_steps == 0 && stats.native_bytes && stats.function_attempts &&
                  stats.syscalls == 11,
          "nested alternate-stack delivery runs Function JIT through eleven real guest syscalls");
    Check(vm.Close().Complete(), "nested signal fixture releases both stack and code owners");
}
}  // namespace

TEST_CASE("Native guest sigreturn retries load with edited GPR and FP frame",
          "[native][execution][exception]") {
    if (!swift::tests::NativeMode()) return;
    for (const auto scope : {ir::Scope::Block, ir::Scope::Function})
        SignalReturn(scope, Recovery::RetryLoad);
}

TEST_CASE("Native guest divide signal skips instruction through sigreturn",
          "[native][execution][exception]") {
    if (!swift::tests::NativeMode()) return;
    for (const auto scope : {ir::Scope::Block, ir::Scope::Function})
        SignalReturn(scope, Recovery::SkipDivide);
}

TEST_CASE("Native ARM64 guest signal retries load with edited X0 and preserved NZCV",
          "[native][execution][exception]") {
    if (!swift::tests::NativeMode()) return;
    for (const auto scope : {ir::Scope::Block, ir::Scope::Function}) ArmSignalReturn(scope);
}

TEST_CASE("Native asynchronous signal leaves a live guest loop through edited context",
          "[native][execution][exception][async]") {
    if (!swift::tests::NativeMode()) return;
    for (const auto scope : {ir::Scope::Block, ir::Scope::Function})
        BoundedAsyncSignalReturn(scope);
}

TEST_CASE("Native nested guest signals restore alternate stacks and signal masks",
          "[native][execution][exception][nested]") {
    if (!swift::tests::NativeMode()) return;
    NestedSignalReturn();
}

TEST_CASE("Native asynchronous signal leaves an indirect loop through its selected L1 view",
          "[native][execution][dispatch-interrupt]") {
    if (!swift::tests::NativeMode()) return;
    for (const auto scope : {ir::Scope::Block, ir::Scope::Function})
        BoundedAsyncSignalReturn(scope, true);
}
