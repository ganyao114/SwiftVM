#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include "arm64_host.h"
#include "base/error.h"
#include "runtime/arch/x86/state.h"
#include "runtime/backend/arm64/abi.h"
#include "runtime/backend/arm64/patch.h"
#include "runtime/execution/dispatch.h"
#include "runtime/execution/fault.h"
#include "signal_time.h"
#if defined(__aarch64__)
#include <asm/hwcap.h>
#include <asm/sigcontext.h>
#endif

namespace swift::runtime::platform {
using execution::NativeRunContext;

namespace {
using execution::FaultContext;
using execution::NativeCode;

#if defined(__aarch64__)
const fpsimd_context* FindFPSIMD(const ucontext_t& machine) noexcept {
    const auto* records = machine.uc_mcontext.__reserved;
    constexpr auto bytes = sizeof(machine.uc_mcontext.__reserved);
    for (std::size_t at = 0; at + sizeof(_aarch64_ctx) <= bytes;) {
        const auto* head = reinterpret_cast<const _aarch64_ctx*>(records + at);
        if (head->size < sizeof(_aarch64_ctx) || (head->size & 15) || head->size > bytes - at)
            break;
        if (head->magic == FPSIMD_MAGIC && head->size >= sizeof(fpsimd_context))
            return reinterpret_cast<const fpsimd_context*>(head);
        at += head->size;
    }
    return nullptr;
}

#endif
bool RecoverNative(FaultContext& context,
                   int signal,
                   int signal_code,
                   std::uintptr_t address,
                   void* raw) noexcept {
#if defined(__aarch64__)
    auto* machine = static_cast<ucontext_t*>(raw);
    const auto host_pc = static_cast<std::uintptr_t>(machine->uc_mcontext.pc);
    const auto* code = context.active.load();
    if (code) {
        if (context.lookup_native) code = context.lookup_native(context.lookup_opaque, host_pc);
    } else
        code = nullptr;
    if (code) {
        const auto base = reinterpret_cast<std::uintptr_t>(code->entry);
        if (host_pc >= base && host_pc - base < code->bytes && !((host_pc - base) & 3)) {
            const auto offset = host_pc - base;
            for (const auto& site : code->faults) {
                const auto recipe = backend::arm64::DecodeFaultRecovery(site);
                const auto fallback = std::uint64_t{site.fallback_offset};
                const bool has_fallback = fallback != UINT32_MAX && fallback + 12 <= code->bytes;
                const bool cold = has_fallback && offset >= fallback && offset - fallback < 12;
                if (!cold && (offset < site.first_offset || offset >= site.last_offset)) continue;
                if (signal == SIGSEGV && signal_code == SEGV_ACCERR && context.recover_code_write) {
                    platform::SignalTimer service{context.service_ns};
                    if (context.recover_code_write(context.code_write_opaque, address)) {
                        context.smc_write_faults.fetch_add(1, std::memory_order_relaxed);
                        return true;  // Retry the actual store; retirement waits for the cold
                                      // dispatcher.
                    }
                }
                if (signal == SIGBUS && signal_code == BUS_ADRALN &&
                    offset == site.unaligned_offset && has_fallback) {
                    errno = 0;
                    if (context.patch_ordered && context.patch_ordered(*code,
                                                                       site.unaligned_offset,
                                                                       site.fallback_offset,
                                                                       context.page_size)) {
                        context.unaligned_patches.fetch_add(1, std::memory_order_relaxed);
                        context.unaligned_recoveries.fetch_add(1, std::memory_order_relaxed);
                        machine->uc_mcontext.pc = base + static_cast<std::uintptr_t>(fallback);
                        return true;
                    }
                    if (errno && context.host_failure)
                        context.host_failure->Set(
                                ErrorKind::System, "patch native unaligned fallback", errno);
                }
                const bool needs_fp = context.pinned_vector_offset != UINT32_MAX ||
                                      context.guest_fp_status != nullptr;
                const auto* fp = needs_fp ? FindFPSIMD(*machine) : nullptr;
                if (needs_fp && !fp) break;
                const auto vectors = context.pinned_vector_offset != UINT32_MAX
                                             ? std::as_bytes(std::span{fp->vregs})
                                             : std::span<const std::byte>{};
                if (!backend::arm64::RestoreGuestState(
                            context,
                            site,
                            std::as_bytes(std::span{machine->uc_mcontext.regs}),
                            machine->uc_mcontext.pstate,
                            vectors))
                    break;
                if (context.guest_fp_status) *context.guest_fp_status = fp->fpsr;
                execution::RaiseGuestFault(context, site, signal, signal_code, host_pc, address);
            }
        }
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

bool PatchOrderedFallback(const NativeCode& code,
                          backend::NativeOffset fast,
                          backend::NativeOffset fallback,
                          std::uintptr_t page_size) noexcept {
    if (!page_size || fast > code.bytes || code.bytes - fast < 4) return false;
    auto* execute = reinterpret_cast<std::uint8_t*>(code.entry) + fast;
    auto* write = code.writable ? code.writable + fast : execute;
    const auto address = reinterpret_cast<std::uintptr_t>(execute);
    const auto page = address - address % page_size;
    if (!code.writable &&
        syscall(SYS_mprotect, reinterpret_cast<void*>(page), page_size, PROT_READ | PROT_WRITE))
        return false;
    const bool patched = backend::arm64::PatchOrderedFallback(code, fast, fallback, write);
    if (!code.writable &&
        syscall(SYS_mprotect, reinterpret_cast<void*>(page), page_size, PROT_READ | PROT_EXEC))
        return false;
    errno = 0;
    return patched;
}

execution::HostCapabilities ProbeFeatures() noexcept {
    execution::HostCapabilities capabilities;
#if defined(__aarch64__)
    const auto caps = getauxval(AT_HWCAP);
    capabilities.rcpc_loads = (caps & HWCAP_LRCPC) != 0;
    capabilities.rcpc_imm9 = (caps & HWCAP_ILRCPC) != 0;
    capabilities.native_nzcv = (caps & HWCAP_FLAGM) != 0;
    capabilities.lse_atomics = (caps & HWCAP_ATOMICS) != 0;
    capabilities.crc32c = (caps & HWCAP_CRC32) != 0;
    capabilities.aes = (caps & HWCAP_AES) != 0;
    capabilities.pmull = (caps & HWCAP_PMULL) != 0;
    capabilities.sha256 = (caps & HWCAP_SHA2) != 0;
    capabilities.afp = (getauxval(AT_HWCAP2) & HWCAP2_AFP) != 0;
#endif
    return capabilities;
}
}  // namespace

Arm64Host::Arm64Host(bool diagnostic) noexcept : runtime(diagnostic, ProbeFeatures()) {}

execution::HostBackendOps Arm64Host::Bind() noexcept {
    auto ops = runtime.Bind();
    ops.recover_native = RecoverNative;
    ops.patch_fault = PatchOrderedFallback;
    return ops;
}
}  // namespace swift::runtime::platform
