#include "loongarch64_host.h"
#include "runtime/execution/dispatch.h"
#include "runtime/execution/fault.h"
#include "signal_time.h"

#include <ucontext.h>
#if defined(__loongarch64)
#include <asm/sigcontext.h>
#endif

namespace swift::runtime::platform {
namespace {
#if defined(__loongarch64)
const std::uint32_t* FindFCSR(const ucontext_t& machine) noexcept {
    const auto* records = reinterpret_cast<const std::byte*>(machine.uc_mcontext.__extcontext);
    // Linux places each payload immediately after sctx_info. Bound traversal;
    // unknown records retain their kernel size and are skipped, never copied.
    constexpr std::size_t limit = 16384;
    for (std::size_t at = 0; at + sizeof(sctx_info) <= limit;) {
        const auto* head = reinterpret_cast<const sctx_info*>(records + at);
        if (!head->magic || head->size < sizeof(sctx_info) || (head->size & 15) ||
            head->size > limit - at)
            return nullptr;
        const auto* payload = records + at + sizeof(sctx_info);
        if (head->magic == FPU_CTX_MAGIC && head->size >= sizeof(sctx_info) + sizeof(fpu_context))
            return &reinterpret_cast<const fpu_context*>(payload)->fcsr;
        if (head->magic == LSX_CTX_MAGIC && head->size >= sizeof(sctx_info) + sizeof(lsx_context))
            return &reinterpret_cast<const lsx_context*>(payload)->fcsr;
        if (head->magic == LASX_CTX_MAGIC && head->size >= sizeof(sctx_info) + sizeof(lasx_context))
            return &reinterpret_cast<const lasx_context*>(payload)->fcsr;
        at += head->size;
    }
    return nullptr;
}

extern "C" void SvmNativeDispatchL1Probe() noexcept;
extern "C" void SvmNativeDispatchCountedL1Probe() noexcept;

void ReturnToHost(execution::NativeRunContext& native,
                  ucontext_t& machine,
                  std::uintptr_t exit) noexcept {
    native.phase.store(SVM_NATIVE_PHASE_LEAVING, std::memory_order_relaxed);
    machine.uc_mcontext.__gregs[4] = reinterpret_cast<std::uintptr_t>(&native);
    machine.uc_mcontext.__gregs[3] = native.root_sp;
    machine.uc_mcontext.__pc = exit;
}
#endif

bool RecoverNative(execution::FaultContext& context,
                   int signal,
                   int signal_code,
                   std::uintptr_t address,
                   void* raw) noexcept {
#if defined(__loongarch64)
    auto* native = context.native;
    if (!native || !native->root_sp || (signal != SIGSEGV && signal != SIGBUS)) return false;
    const auto phase = native->phase.load(std::memory_order_relaxed);
    if (phase != SVM_NATIVE_PHASE_GUEST && phase != SVM_NATIVE_PHASE_ENTERING) return false;
    auto& machine = *static_cast<ucontext_t*>(raw);
    const auto pc = static_cast<std::uintptr_t>(machine.uc_mcontext.__pc);
    const bool dispatch_probe =
            pc == reinterpret_cast<std::uintptr_t>(&SvmNativeDispatchL1Probe) ||
            pc == reinterpret_cast<std::uintptr_t>(&SvmNativeDispatchCountedL1Probe);
    if (dispatch_probe && signal == SIGSEGV && signal_code == SEGV_ACCERR &&
        execution::IsDispatchInterrupt(context, address)) {
        const auto* fp = native->fp_environment ? FindFCSR(machine) : nullptr;
        if (native->fp_environment && !fp) return false;
        backend::loongarch64::RestoreGuestState(
                context, std::as_bytes(std::span{machine.uc_mcontext.__gregs}));
        if (fp) native->guest_fpsr = *fp & 0x1f0000;
        *context.guest_pc = static_cast<std::uint64_t>(machine.uc_mcontext.__gregs[4]);
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
        const auto* fp = native->fp_environment ? FindFCSR(machine) : nullptr;
        if (native->fp_environment && !fp) return false;
        backend::loongarch64::RestoreGuestState(
                context, std::as_bytes(std::span{machine.uc_mcontext.__gregs}));
        if (fp) native->guest_fpsr = *fp & 0x1f0000;
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

int SynchronizeCode(std::uint8_t* writable,
                    const std::uint8_t* executable,
                    std::size_t size) noexcept {
    __builtin___clear_cache(reinterpret_cast<char*>(writable),
                            reinterpret_cast<char*>(writable + size));
    if (writable != executable)
        __builtin___clear_cache(
                reinterpret_cast<char*>(const_cast<std::uint8_t*>(executable)),
                reinterpret_cast<char*>(const_cast<std::uint8_t*>(executable + size)));
    return 0;
}
}  // namespace

Loongarch64Host::Loongarch64Host(bool diagnostic) noexcept : runtime(diagnostic) {}

execution::HostBackendOps Loongarch64Host::Bind() noexcept {
    auto ops = runtime.Bind();
    ops.recover_native = RecoverNative;
    ops.synchronize_code = SynchronizeCode;
    return ops;
}
}  // namespace swift::runtime::platform
