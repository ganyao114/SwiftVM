#include "loongarch64_host.h"
#include "runtime/execution/fault.h"
#include "signal_time.h"

#include <ucontext.h>

namespace swift::runtime::platform {
namespace {
bool RecoverNative(execution::FaultContext& context,
                   int signal,
                   int signal_code,
                   std::uintptr_t address,
                   void* raw) noexcept {
#if defined(__loongarch64)
    const auto& machine = *static_cast<const ucontext_t*>(raw);
    const auto pc = static_cast<std::uintptr_t>(machine.uc_mcontext.__pc);
    const auto* code = context.active.load();
    if (!code) return false;
    if (context.lookup_native) code = context.lookup_native(context.lookup_opaque, pc);
    if (!code) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(code->entry);
    if (pc < base || pc - base >= code->bytes) return false;
    for (const auto& site : code->faults) {
        if (pc - base < site.first_offset || pc - base >= site.last_offset) continue;
        if (signal == SIGSEGV && signal_code == SEGV_ACCERR && context.recover_code_write) {
            SignalTimer timer{context.service_ns};
            if (context.recover_code_write(context.code_write_opaque, address)) {
                context.smc_write_faults.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        backend::loongarch64::RestoreGuestState(
                context, std::as_bytes(std::span{machine.uc_mcontext.__gregs}));
        execution::RaiseGuestFault(context, site, signal, signal_code, pc, address);
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
