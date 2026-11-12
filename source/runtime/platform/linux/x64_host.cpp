#include "runtime/execution/dispatch.h"
#include "runtime/execution/fault.h"
#include "signal_time.h"
#include "x64_host.h"

#include <array>
#include <ucontext.h>

namespace swift::runtime::platform {
namespace {
#if defined(__x86_64__)
extern "C" void SvmNativeDispatchL1Probe() noexcept;
extern "C" void SvmNativeDispatchCountedL1Probe() noexcept;
extern "C" void SvmNativeDispatchNZCVL1Probe() noexcept;
extern "C" void SvmNativeDispatchNZCVCountedL1Probe() noexcept;

bool RestoreSignalState(execution::FaultContext& context,
                        const backend::FaultSite& site,
                        const ucontext_t& machine) noexcept {
    auto* native = context.native;
    if (native->fp_environment && !machine.uc_mcontext.fpregs) return false;
    constexpr unsigned indices[]{REG_RAX,
                                 REG_RCX,
                                 REG_RDX,
                                 REG_RBX,
                                 REG_RSP,
                                 REG_RBP,
                                 REG_RSI,
                                 REG_RDI,
                                 REG_R8,
                                 REG_R9,
                                 REG_R10,
                                 REG_R11,
                                 REG_R12,
                                 REG_R13,
                                 REG_R14,
                                 REG_R15};
    std::array<std::uint64_t, 16> registers;
    for (unsigned i = 0; i < registers.size(); ++i)
        registers[i] = static_cast<std::uint64_t>(machine.uc_mcontext.gregs[indices[i]]);
    if (!backend::x64::RestoreGuestState(context,
                                         site,
                                         std::as_bytes(std::span{registers}),
                                         machine.uc_mcontext.gregs[REG_EFL]))
        return false;
    if (native->fp_environment) native->guest_fpcr = machine.uc_mcontext.fpregs->mxcsr;
    return true;
}

void ReturnToHost(execution::NativeRunContext& native,
                  ucontext_t& machine,
                  std::uintptr_t exit) noexcept {
    native.phase.store(SVM_NATIVE_PHASE_LEAVING, std::memory_order_relaxed);
    machine.uc_mcontext.gregs[REG_RDI] = reinterpret_cast<std::uintptr_t>(&native);
    machine.uc_mcontext.gregs[REG_RSP] = native.root_sp;
    machine.uc_mcontext.gregs[REG_RIP] = exit;
}
#endif

bool RecoverNative(execution::FaultContext& context,
                   int signal,
                   int signal_code,
                   std::uintptr_t address,
                   void* raw) noexcept {
#if defined(__x86_64__)
    auto* native = context.native;
    if (!native || !native->root_sp || (signal != SIGSEGV && signal != SIGBUS)) return false;
    const auto phase = native->phase.load(std::memory_order_relaxed);
    if (phase != SVM_NATIVE_PHASE_GUEST && phase != SVM_NATIVE_PHASE_ENTERING) return false;
    auto& machine = *static_cast<ucontext_t*>(raw);
    const auto pc = static_cast<std::uintptr_t>(machine.uc_mcontext.gregs[REG_RIP]);
    const bool dispatch_probe =
            pc == reinterpret_cast<std::uintptr_t>(&SvmNativeDispatchL1Probe) ||
            pc == reinterpret_cast<std::uintptr_t>(&SvmNativeDispatchCountedL1Probe) ||
            pc == reinterpret_cast<std::uintptr_t>(&SvmNativeDispatchNZCVL1Probe) ||
            pc == reinterpret_cast<std::uintptr_t>(&SvmNativeDispatchNZCVCountedL1Probe);
    if (dispatch_probe && signal == SIGSEGV && signal_code == SEGV_ACCERR &&
        execution::IsDispatchInterrupt(context, address)) {
        // The dispatcher has committed the previous unit. RDI is its next
        // guest target, including the root probe before active becomes non-null.
        backend::FaultSite dispatch_state{};
        if (native->native_eflags) {
            dispatch_state.recovery[0] = 2 | 4;
            dispatch_state.recovery[1] = 15;
        }
        auto dispatch_machine = machine;
        if (native->native_eflags) dispatch_machine.uc_mcontext.gregs[REG_EFL] = native->eflags;
        if (!RestoreSignalState(context, dispatch_state, dispatch_machine)) return false;
        *context.guest_pc = static_cast<std::uint64_t>(machine.uc_mcontext.gregs[REG_RDI]);
        context.dispatch_interrupts.fetch_add(1, std::memory_order_relaxed);
        ReturnToHost(
                *native, machine, reinterpret_cast<std::uintptr_t>(&execution::SvmNativeEventExit));
        return true;
    }
    if (phase != SVM_NATIVE_PHASE_GUEST) return false;
    const auto* code = context.active.load();
    if (!code) return false;
    if (context.lookup_native) code = context.lookup_native(context.lookup_opaque, pc);
    if (!code) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(code->entry);
    if (pc < base || pc - base >= code->bytes) return false;
    for (const auto& site : code->faults) {
        if (pc - base < site.first_offset || pc - base >= site.last_offset) continue;
        if (site.Kind() != backend::FaultSiteKind::GuestMemory || !(site.recovery[4] & 7)) continue;
        if (signal == SIGSEGV && signal_code == SEGV_ACCERR && context.recover_code_write) {
            SignalTimer timer{context.service_ns};
            if (context.recover_code_write(context.code_write_opaque, address)) {
                context.smc_write_faults.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        if (!RestoreSignalState(context, site, machine)) return false;
        execution::RecordGuestFault(context, site, signal, signal_code, pc, address);
        context.exception.native_offset = static_cast<std::uint32_t>(pc - base);
        ReturnToHost(*native,
                     machine,
                     reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExceptionExit));
        return true;
    }
#else
    (void)context;
    (void)signal;
    (void)signal_code;
    (void)address;
    (void)raw;
#endif
    return false;
}
}  // namespace

X64Host::X64Host(bool diagnostic) noexcept : runtime(diagnostic) {}

execution::HostBackendOps X64Host::Bind() noexcept {
    auto ops = runtime.Bind();
    ops.recover_native = RecoverNative;
    return ops;
}
}  // namespace swift::runtime::platform
