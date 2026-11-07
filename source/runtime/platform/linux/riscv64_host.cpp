#include "riscv64_host.h"
#include "runtime/arch/x86/state.h"
#include "runtime/backend/riscv64/abi_layout.inc"
#include "runtime/backend/riscv64/patch.h"
#include "runtime/execution/dispatch.h"
#include "runtime/execution/fault.h"

#include "signal_time.h"
#include <cerrno>
#include <cstring>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

namespace swift::runtime::platform {
namespace {
using execution::FaultContext;
using execution::NativeRunContext;

bool RecoverNative(FaultContext& context,
                   int signal,
                   int signal_code,
                   std::uintptr_t address,
                   void* raw) noexcept {
#if defined(__riscv) && __riscv_xlen == 64
    const auto& machine = *static_cast<const ucontext_t*>(raw);
    // Linux's first general-context word is the faulting PC; x1..x31
    // retain their architectural indices, including the packed flags home.
    const auto host_pc = static_cast<std::uintptr_t>(machine.uc_mcontext.__gregs[0]);
    const auto* code = context.active.load();
    if (!code) return false;
    if (context.lookup_native) code = context.lookup_native(context.lookup_opaque, host_pc);
    if (!code) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(code->entry);
    if (host_pc < base || host_pc - base >= code->bytes) return false;
    const auto offset = host_pc - base;
    for (const auto& site : code->faults) {
        if (offset < site.first_offset || offset >= site.last_offset) continue;
        if (signal == SIGSEGV && signal_code == SEGV_ACCERR && context.recover_code_write) {
            SignalTimer elapsed{context.service_ns};
            const bool recovered = context.recover_code_write(context.code_write_opaque, address);
            if (recovered) {
                context.smc_write_faults.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        backend::riscv64::RestoreGuestState(context,
                                            std::as_bytes(std::span{machine.uc_mcontext.__gregs}));
        execution::RaiseGuestFault(context, site, signal, signal_code, host_pc, address);
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

int FlushCode(const std::uint8_t* address, std::size_t bytes) noexcept {
#if defined(__linux__) && defined(__riscv) && __riscv_xlen == 64
    // Flags=0 requests the Linux process-wide synchronization contract,
    // including migration. A failed synchronization cannot publish callable code.
    if (syscall(SYS_riscv_flush_icache, address, address + bytes, 0) != 0) return errno;
#else
    __builtin___clear_cache(reinterpret_cast<char*>(const_cast<std::uint8_t*>(address)),
                            reinterpret_cast<char*>(const_cast<std::uint8_t*>(address + bytes)));
#endif
    return 0;
}

int SynchronizeCode(std::uint8_t* writable,
                    const std::uint8_t* executable,
                    std::size_t bytes) noexcept {
    if (const int error = FlushCode(writable, bytes)) return error;
    return executable != writable ? FlushCode(executable, bytes) : 0;
}
}  // namespace

Riscv64Host::Riscv64Host(bool diagnostic) noexcept : runtime(diagnostic) {}

execution::HostBackendOps Riscv64Host::Bind() noexcept {
    auto ops = runtime.Bind();
    ops.recover_native = RecoverNative;
    ops.synchronize_code = SynchronizeCode;
    return ops;
}
}  // namespace swift::runtime::platform
