#include "runtime.h"
#include "runtime/arch/x86/state.h"
#include "abi.h"
#include "patch.h"
#include "relocate.h"
#include "runtime/execution/dispatch.h"
#include "runtime/execution/fault.h"

#include <cstring>

namespace swift::runtime::backend::arm64 {
using execution::NativeRunContext;

Runtime::Runtime(bool diagnostic, execution::HostCapabilities host) noexcept : capabilities(host) {
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

execution::HostBackendOps Runtime::Bind() noexcept {
    execution::HostBackendOps ops;
    ops.context = this;
    ops.target = backend::arm64::KnownTargetInfo();
    ops.capabilities = capabilities;
    ops.pinned_uniform_slots = 16;
    ops.pinned_vectors = true;
    ops.specializes_fp_flush = !capabilities.afp;
    ops.compile = [](void* raw,
                     const ir::Program& program,
                     execution::HostCompileRequest request) -> backend::CompileResult& {
        auto& self = *static_cast<Runtime*>(raw);
        auto policy = self.options;
        policy.entry_pc = request.entry_pc;
        policy.relocatable = request.relocatable;
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
    ops.relocate = [](void* raw,
                      std::span<std::uint8_t> code,
                      std::span<const backend::Relocation> relocations) noexcept {
        return Relocate(code, relocations, static_cast<Runtime*>(raw)->options);
    };
    ops.trim = [](void* raw) noexcept {
        auto& self = *static_cast<Runtime*>(raw);
        auto& out = self.output;
        if (out.words.capacity() > 1024u * 1024) std::vector<std::uint32_t>{}.swap(out.words);
        if (out.relocations.capacity() > 4096)
            std::vector<backend::Relocation>{}.swap(out.relocations);
        if (out.links.capacity() > 4096) std::vector<backend::LinkSite>{}.swap(out.links);
        if (out.entries.capacity() > 4096) std::vector<backend::EntryPoint>{}.swap(out.entries);
        if (self.allocation.homes.capacity() > 8u * 1024 * 1024 / sizeof(backend::arm64::Home))
            std::vector<backend::arm64::Home>{}.swap(self.allocation.homes);
        if (self.allocation.block_order.capacity() > 4096)
            std::vector<std::uint32_t>{}.swap(self.allocation.block_order);
    };
    ops.release = [](void* raw) noexcept {
        auto& self = *static_cast<Runtime*>(raw);
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
    ops.synchronize_code =
            [](std::uint8_t* write, const std::uint8_t* execute, std::size_t bytes) noexcept {
                backend::arm64::SynchronizeCode(write, execute, bytes);
                return 0;
            };
    ops.patch_link = backend::arm64::PatchNativeLink;
    ops.unlink = backend::arm64::UnlinkNativeLink;
    ops.reset_indirect = backend::arm64::ResetIndirectSite;
    ops.update_fp = [](void* raw, NativeRunContext& native, std::uint32_t mxcsr) noexcept {
        const auto& self = *static_cast<Runtime*>(raw);
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

namespace {
struct Registers {
    std::span<const std::byte> bytes;

    std::uint64_t operator[](unsigned index) const noexcept {
        std::uint64_t bits;
        std::memcpy(&bits, bytes.data() + index * 8, 8);
        return bits;
    }
};
}  // namespace

bool RestoreGuestState(execution::FaultContext& context,
                       const backend::FaultSite& site,
                       std::span<const std::byte> gprs,
                       std::uint64_t native_flags,
                       std::span<const std::byte> vectors) noexcept {
    const Registers registers{gprs};
    const auto recipe = DecodeFaultRecovery(site);
    if (context.pinned_vector_offset != UINT32_MAX) {
        if (vectors.empty()) return false;
        auto* slots = reinterpret_cast<std::uint8_t*>(context.state) + context.pinned_vector_offset;
        std::memcpy(slots, vectors.data() + 16 * 16, 16 * 16);
    }
    if (context.pinned_uniform_offset != UINT32_MAX) {
        auto* slots =
                reinterpret_cast<std::uint8_t*>(context.state) + context.pinned_uniform_offset;
        for (unsigned i = 0; i < context.pinned_uniform_slots; ++i) {
            // Low eight are callee-saved; pin16 maps high slots
            // directly to x8..x15. Preserve the legacy pin12 map.
            const auto reg =
                    backend::arm64::abi::PinnedUniformRegister(i, context.pinned_uniform_slots);
            const auto bits = registers[reg];
            std::memcpy(slots + i * 8, &bits, 8);
        }
    }
    if (context.flags_offset != UINT32_MAX) {
        auto packed = registers[1];
        if (context.native_nzcv) {
            std::memcpy(&packed,
                        reinterpret_cast<const std::uint8_t*>(context.state) + context.flags_offset,
                        8);
            packed &= ~backend::arm64::abi::PackedDynamicFlags;
            if (recipe.test_bits && (registers[29] & backend::arm64::abi::DeferredTestTag)) {
                // The compiler records the actual still-valid
                // entry snapshot, which can differ from the pin
                // already overwritten by a successful load.
                const auto raw = registers[recipe.test_register];
                const auto value = recipe.test_bits == 32
                                           ? std::uint64_t{static_cast<std::uint32_t>(raw)}
                                           : raw;
                packed |= (value & UINT64_C(0xff)) | (std::uint64_t{value == 0} << 30) |
                          (((value >> (recipe.test_bits - 1)) & 1) << 31);
            } else {
                const auto pf = registers[recipe.parity_register ? recipe.parity_register : 1];
                const auto carry_flip = recipe.carry_direct ? UINT64_C(0) : UINT64_C(0x20000000);
                packed |= (pf & UINT64_C(0xff)) |
                          ((native_flags ^ carry_flip) & UINT64_C(0xf0000000)) |
                          ((((registers[29] ^ pf) >> 4) & 1) << 26);
            }
        }
        std::memcpy(
                reinterpret_cast<std::uint8_t*>(context.state) + context.flags_offset, &packed, 8);
    }
    return true;
}

}  // namespace swift::runtime::backend::arm64
