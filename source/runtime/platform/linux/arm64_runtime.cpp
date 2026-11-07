#include "arm64_runtime.h"
#include "runtime/arch/x86/state.h"
#include "runtime/backend/arm64/abi.h"
#include "runtime/backend/arm64/patch.h"
#include "runtime/execution/dispatch.h"
#include "runtime/execution/fault.h"
#include "runtime/execution/vm.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <sys/auxv.h>
#include <ucontext.h>
#if defined(__aarch64__)
#include <asm/hwcap.h>
#include <asm/sigcontext.h>
#endif

namespace swift::runtime::platform {
using execution::NativeRunContext;

namespace {
using execution::FaultContext;
using execution::NativeCode;

struct ServiceTimer {
    std::uint64_t& total;
    std::chrono::steady_clock::time_point begin{std::chrono::steady_clock::now()};

    ~ServiceTimer() {
        total += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                    std::chrono::steady_clock::now() - begin)
                                                    .count());
    }
};
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
                    ServiceTimer service{context.service_ns};
                    if (context.recover_code_write(context.code_write_opaque, address)) {
                        context.smc_write_faults.fetch_add(1, std::memory_order_relaxed);
                        return true;  // Retry the actual store; retirement waits for the cold
                                      // dispatcher.
                    }
                }
                if (signal == SIGBUS && signal_code == BUS_ADRALN &&
                    offset == site.unaligned_offset && has_fallback) {
                    if (context.patch_ordered && context.patch_ordered(*code,
                                                                       site.unaligned_offset,
                                                                       site.fallback_offset,
                                                                       context.page_size)) {
                        context.unaligned_patches.fetch_add(1, std::memory_order_relaxed);
                        context.unaligned_recoveries.fetch_add(1, std::memory_order_relaxed);
                        machine->uc_mcontext.pc = base + static_cast<std::uintptr_t>(fallback);
                        return true;
                    }
                }
                if (context.pinned_vector_offset != UINT32_MAX) {
                    const auto* vectors = FindFPSIMD(*machine);
                    if (!vectors)
                        break;  // Do not publish incomplete guest state; chain the original signal.
                    auto* slots = reinterpret_cast<std::uint8_t*>(context.state) +
                                  context.pinned_vector_offset;
                    std::memcpy(slots, &vectors->vregs[16], 16 * sizeof(vectors->vregs[0]));
                }
                if (context.pinned_uniform_offset != UINT32_MAX) {
                    auto* slots = reinterpret_cast<std::uint8_t*>(context.state) +
                                  context.pinned_uniform_offset;
                    for (unsigned i = 0; i < context.pinned_uniform_slots; ++i) {
                        // Low eight are callee-saved; pin16 maps high slots
                        // directly to x8..x15. Preserve the legacy pin12 map.
                        const auto reg = backend::arm64::abi::PinnedUniformRegister(
                                i, context.pinned_uniform_slots);
                        std::memcpy(slots + i * 8, &machine->uc_mcontext.regs[reg], 8);
                    }
                }
                if (context.flags_offset != UINT32_MAX) {
                    auto packed = machine->uc_mcontext.regs[1];
                    if (context.native_nzcv) {
                        std::memcpy(&packed,
                                    reinterpret_cast<const std::uint8_t*>(context.state) +
                                            context.flags_offset,
                                    8);
                        packed &= ~backend::arm64::abi::PackedDynamicFlags;
                        if (recipe.test_bits && (machine->uc_mcontext.regs[29] &
                                                 backend::arm64::abi::DeferredTestTag)) {
                            // The compiler records the actual still-valid
                            // entry snapshot, which can differ from the pin
                            // already overwritten by a successful load.
                            const auto raw = machine->uc_mcontext.regs[recipe.test_register];
                            const auto value =
                                    recipe.test_bits == 32
                                            ? std::uint64_t{static_cast<std::uint32_t>(raw)}
                                            : raw;
                            packed |= (value & UINT64_C(0xff)) | (std::uint64_t{value == 0} << 30) |
                                      (((value >> (recipe.test_bits - 1)) & 1) << 31);
                        } else {
                            const auto pf =
                                    machine->uc_mcontext
                                            .regs[recipe.parity_register ? recipe.parity_register
                                                                         : 1];
                            const auto carry_flip =
                                    recipe.carry_direct ? UINT64_C(0) : UINT64_C(0x20000000);
                            packed |= (pf & UINT64_C(0xff)) |
                                      ((machine->uc_mcontext.pstate ^ carry_flip) &
                                       UINT64_C(0xf0000000)) |
                                      ((((machine->uc_mcontext.regs[29] ^ pf) >> 4) & 1) << 26);
                        }
                    }
                    std::memcpy(
                            reinterpret_cast<std::uint8_t*>(context.state) + context.flags_offset,
                            &packed,
                            8);
                }
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
}  // namespace

Arm64Runtime::Arm64Runtime(bool diagnostic) noexcept {
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
    using x86::GuestState;
    options.flags_offset = offsetof(GuestState, packed_flags);
    options.pc_offset = offsetof(GuestState, pc);
    options.pinned_uniform_offset = offsetof(GuestState, gprs);
    options.pinned_uniform_slots = 16;
    options.rcpc_loads = capabilities.rcpc_loads;
    options.rcpc_imm9 = capabilities.rcpc_imm9;
    options.native_nzcv = capabilities.native_nzcv;
    options.lse_atomics = capabilities.lse_atomics;
    options.crc32c = capabilities.crc32c;
    options.aes = capabilities.aes;
    options.pmull = capabilities.pmull;
    options.sha256 = capabilities.sha256;
    options.native_fp_environment = true;
    options.dispatch_mode = diagnostic ? backend::arm64::DispatchMode::InlineCounted
                                       : backend::arm64::DispatchMode::Inline;
    options.local_dispatch_cache = options.callable_directory = true;
    const auto dispatch = capabilities.native_nzcv
                                  ? (diagnostic ? &execution::SvmNativeDispatchNZCVCounted
                                                : &execution::SvmNativeDispatchNZCV)
                                  : (diagnostic ? &execution::SvmNativeDispatchCounted
                                                : &execution::SvmNativeDispatch);
    options.dispatcher = reinterpret_cast<std::uintptr_t>(dispatch);
    options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);
    if (!capabilities.afp) options.mxcsr_flush_offset = offsetof(GuestState, mxcsr);
}

execution::HostBackendOps Arm64Runtime::Bind() noexcept {
    execution::HostBackendOps ops;
    ops.context = this;
    ops.target = backend::arm64::KnownTargetInfo();
    ops.capabilities = capabilities;
    ops.compile = [](void* raw,
                     const ir::Program& program,
                     execution::HostCompileRequest request) -> backend::CompileResult& {
        auto& self = *static_cast<Arm64Runtime*>(raw);
        auto policy = self.options;
        policy.entry_pc = request.entry_pc;
        policy.fault_locations = policy.recover_unaligned = request.fault_locations;
        policy.private_uniforms = request.private_state;
        policy.local_flags =
                request.private_state && request.fault_locations && self.capabilities.native_nzcv;
        policy.deferred_pc = request.private_state && request.fault_locations;
        policy.pinned_vector_offset =
                request.private_state ? offsetof(x86::GuestState, xmm) : UINT32_MAX;
        if (!self.capabilities.afp) policy.mxcsr_flush_mode = request.fp_mode;
        backend::arm64::Compile(program, policy, self.output, self.allocation);
        return self.output;
    };
    ops.trim = [](void* raw) noexcept {
        auto& self = *static_cast<Arm64Runtime*>(raw);
        auto& out = self.output;
        if (out.words.capacity() > 1024u * 1024) std::vector<std::uint32_t>{}.swap(out.words);
        if (out.links.capacity() > 4096) std::vector<backend::LinkSite>{}.swap(out.links);
        if (out.entries.capacity() > 4096) std::vector<backend::EntryPoint>{}.swap(out.entries);
        if (self.allocation.homes.capacity() > 8u * 1024 * 1024 / sizeof(backend::arm64::Home))
            std::vector<backend::arm64::Home>{}.swap(self.allocation.homes);
        if (self.allocation.block_order.capacity() > 4096)
            std::vector<std::uint32_t>{}.swap(self.allocation.block_order);
    };
    ops.release = [](void* raw) noexcept {
        auto& self = *static_cast<Arm64Runtime*>(raw);
        self.output = {};
        self.allocation = {};
    };
    ops.initialize_link = [](void*,
                             std::span<std::uint8_t> write,
                             std::uintptr_t execute,
                             bool dual,
                             const backend::LinkSite& site,
                             std::uintptr_t ready) noexcept {
        return backend::arm64::InitializeNativeLink(
                write,
                execute,
                site,
                ready,
                dual ? reinterpret_cast<std::uintptr_t>(write.data()) : 0);
    };
    ops.synchronize_code = backend::arm64::SynchronizeCode;
    ops.patch_link = backend::arm64::PatchNativeLink;
    ops.unlink = backend::arm64::UnlinkNativeLink;
    ops.reset_indirect = backend::arm64::ResetIndirectSite;
    ops.patch_fault = backend::arm64::PatchOrderedFallback;
    ops.recover_native = RecoverNative;
    ops.update_fp = [](void* raw, NativeRunContext& native, std::uint32_t mxcsr) noexcept {
        const auto& self = *static_cast<Arm64Runtime*>(raw);
        const auto daz = (mxcsr >> 6) & 1u, ftz = (mxcsr >> 15) & 1u;
        native.guest_fpcr = ((mxcsr >> 13) & 1u) << 23 | ((mxcsr >> 14) & 1u) << 22;
        if (self.capabilities.afp) native.guest_fpcr |= 6u | daz | (ftz << 24);
    };
    ops.restore_host_fp = [](const NativeRunContext& native) noexcept {
#if defined(__aarch64__)
        asm volatile("msr fpcr, %0\nmsr fpsr, %1"
                     :
                     : "r"(native.host_fpcr), "r"(native.host_fpsr)
                     : "memory");
#else
        (void)native;
#endif
    };
#if !defined(__aarch64__)
    ops.compile = nullptr;
    ops.target.mode = ir::TargetMode::Unknown;
#endif
    return ops;
}

// The C++ bridge and assembler consume the same ABI offsets.
#define SVM_CHECK_CONTEXT(field, macro) static_assert(offsetof(NativeRunContext, field) == macro)
SVM_CHECK_CONTEXT(state, SVM_A64_CTX_STATE);
SVM_CHECK_CONTEXT(opaque, SVM_A64_CTX_OPAQUE);
SVM_CHECK_CONTEXT(resolve, SVM_A64_CTX_RESOLVE);
SVM_CHECK_CONTEXT(slots, SVM_A64_CTX_SLOTS);
SVM_CHECK_CONTEXT(mask, SVM_A64_CTX_MASK);
SVM_CHECK_CONTEXT(active, SVM_A64_CTX_ACTIVE);
SVM_CHECK_CONTEXT(anchor, SVM_A64_CTX_ANCHOR);
SVM_CHECK_CONTEXT(dispatcher_entries, SVM_A64_CTX_DISPATCHES);
SVM_CHECK_CONTEXT(cache_hits, SVM_A64_CTX_HITS);
SVM_CHECK_CONTEXT(pinned_uniform_offset, SVM_A64_CTX_PIN_OFFSET);
SVM_CHECK_CONTEXT(flags_offset, SVM_A64_CTX_FLAGS_OFFSET);
SVM_CHECK_CONTEXT(pc_offset, SVM_A64_CTX_PC_OFFSET);
SVM_CHECK_CONTEXT(pinned_uniform_slots, SVM_A64_CTX_PIN_SLOTS);
SVM_CHECK_CONTEXT(native_nzcv, SVM_A64_CTX_NATIVE_NZCV);
SVM_CHECK_CONTEXT(host_fpcr, SVM_A64_CTX_HOST_FPCR);
SVM_CHECK_CONTEXT(host_fpsr, SVM_A64_CTX_HOST_FPSR);
SVM_CHECK_CONTEXT(guest_fpcr, SVM_A64_CTX_GUEST_FPCR);
SVM_CHECK_CONTEXT(fp_environment, SVM_A64_CTX_FP_ENVIRONMENT);
SVM_CHECK_CONTEXT(probe_steps, SVM_A64_CTX_PROBES);
SVM_CHECK_CONTEXT(pinned_vector_offset, SVM_A64_CTX_VECTOR_OFFSET);
#undef SVM_CHECK_CONTEXT
}  // namespace swift::runtime::platform

namespace swift::runtime::execution {
RunResult RunGuest(const std::string& executable,
                   std::span<const std::string> argv,
                   std::span<const std::string> environment,
                   RuntimeOptions options) {
    using Clock = std::chrono::steady_clock;
    RuntimeStats stats;
    int code = 125;
    std::string error;
    const auto begin = Clock::now();
    try {
        const auto binding_begin = Clock::now();
        platform::Arm64Runtime host{options.decode_timing};
        const auto backend = host.Bind();
        const auto binding_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - binding_begin)
                        .count());
        stats.compile_request_ns += binding_ns;
        stats.fault_service_ns += binding_ns;
        VM vm{options, stats, backend};
        try {
            code = vm.Run(executable, argv, environment);
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        const auto released = vm.Close();
        if (!released.Complete()) {
            if (!error.empty()) error += "; ";
            error += "native mapping release failed: remaining=" +
                     std::to_string(released.remaining_views) +
                     " unmap_errno=" + std::to_string(released.unmap_error) +
                     " close_errno=" + std::to_string(released.close_error);
        }
    } catch (const std::exception& failure) {
        error = failure.what();
    }
    stats.run_lifetime_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
    if (options.decode_timing)
        stats.compile_without_byte_decode_ns =
                stats.compile_request_ns > stats.compile_decode.nanoseconds
                        ? stats.compile_request_ns - stats.compile_decode.nanoseconds
                        : 0;
    return {code, stats, std::move(error)};
}
#if !defined(__aarch64__)
extern "C" std::uint64_t SvmRunNative(NativeRunContext*) noexcept { return 2; }

extern "C" void SvmNativeDispatch() noexcept {}

extern "C" void SvmNativeDispatchCounted() noexcept {}

extern "C" void SvmNativeDispatchNZCV() noexcept {}

extern "C" void SvmNativeDispatchNZCVCounted() noexcept {}

extern "C" void SvmNativePending() noexcept {}

extern "C" void SvmNativePendingCounted() noexcept {}

extern "C" void SvmNativeExit() noexcept {}
#endif
}  // namespace swift::runtime::execution
