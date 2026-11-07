#include "runtime.h"
#include "runtime/arch/x86/state.h"
#include "abi_layout.inc"
#include "patch.h"
#include "relocate.h"
#include "runtime/execution/dispatch.h"
#include "runtime/execution/fault.h"

#include <cstring>

namespace swift::runtime::backend::riscv64 {
using execution::NativeRunContext;

Runtime::Runtime(bool diagnostic) noexcept {
    options.flags_offset = offsetof(x86::GuestState, packed_flags);
    options.pc_offset = offsetof(x86::GuestState, pc);
    options.mxcsr_offset = offsetof(x86::GuestState, mxcsr);
    options.native_fp_environment = true;
    options.dispatcher = reinterpret_cast<std::uintptr_t>(
            diagnostic ? &execution::SvmNativeDispatchCounted : &execution::SvmNativeDispatch);
    options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);
}

execution::HostBackendOps Runtime::Bind() noexcept {
    execution::HostBackendOps ops;
    ops.context = this;
    ops.target = backend::riscv64::KnownTargetInfo();
    ops.pinned_uniform_slots = 0;
    ops.pinned_vectors = false;
    ops.configure = [](void* raw, const GuestLayout& layout) noexcept {
        auto& options = static_cast<Runtime*>(raw)->options;
        options.flags_offset = layout.flags_offset;
        options.pc_offset = layout.pc_offset;
        options.mxcsr_offset = layout.numeric_control_offset;
    };
    ops.compile = [](void* raw,
                     const ir::Program& program,
                     execution::HostCompileRequest request) -> backend::CompileResult& {
        auto& self = *static_cast<Runtime*>(raw);
        auto policy = self.options;
        policy.entry_pc = request.entry_pc;
        policy.relocatable = request.relocatable;
        policy.fault_locations = request.fault_locations;
        backend::riscv64::Compile(program, policy, self.output, self.allocation);
        return self.output;
    };
    ops.relocate = [](void* raw,
                      std::span<std::uint8_t> code,
                      std::span<const backend::Relocation> relocations) noexcept {
        return Relocate(code, relocations, static_cast<Runtime*>(raw)->options);
    };
    ops.trim = [](void* raw) noexcept {
        auto& self = *static_cast<Runtime*>(raw);
        if (self.output.words.capacity() > 1024u * 1024)
            std::vector<std::uint32_t>{}.swap(self.output.words);
        if (self.output.relocations.capacity() > 4096)
            std::vector<backend::Relocation>{}.swap(self.output.relocations);
        if (self.output.entries.capacity() > 4096)
            std::vector<backend::EntryPoint>{}.swap(self.output.entries);
        if (self.allocation.homes.capacity() > 8u * 1024 * 1024 / sizeof(backend::riscv64::Home))
            std::vector<backend::riscv64::Home>{}.swap(self.allocation.homes);
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
        return backend::riscv64::InitializeNativeLink(write, execute, site, ready, dual);
    };
    ops.patch_link = backend::riscv64::PatchNativeLink;
    ops.unlink = backend::riscv64::UnlinkNativeLink;
    ops.update_fp = [](void*, NativeRunContext& native, std::uint32_t mxcsr) noexcept {
        constexpr unsigned rounding[]{0, 2, 3, 1};  // RNE, RDN, RUP, RTZ.
        native.guest_fpcr = rounding[(mxcsr >> 13) & 3] << 5;
    };
    ops.restore_host_fp = [](const NativeRunContext& native) noexcept {
#if defined(__riscv) && __riscv_xlen == 64
        asm volatile("fscsr %0" ::"r"(native.host_fpcr) : "memory");
#else
        (void)native;
#endif
    };
#if !defined(__riscv) || __riscv_xlen != 64
    ops.compile = nullptr;
    ops.target.mode = ir::TargetMode::Unknown;
#endif
    return ops;
}

#define SVM_CHECK_CONTEXT(field, macro)                                                            \
    static_assert(offsetof(execution::NativeRunContext, field) == macro)
SVM_CHECK_CONTEXT(state, SVM_RV_CTX_STATE);
SVM_CHECK_CONTEXT(opaque, SVM_RV_CTX_OPAQUE);
SVM_CHECK_CONTEXT(resolve, SVM_RV_CTX_RESOLVE);
SVM_CHECK_CONTEXT(slots, SVM_RV_CTX_SLOTS);
SVM_CHECK_CONTEXT(mask, SVM_RV_CTX_MASK);
SVM_CHECK_CONTEXT(active, SVM_RV_CTX_ACTIVE);
SVM_CHECK_CONTEXT(anchor, SVM_RV_CTX_ANCHOR);
SVM_CHECK_CONTEXT(dispatcher_entries, SVM_RV_CTX_DISPATCHES);
SVM_CHECK_CONTEXT(cache_hits, SVM_RV_CTX_HITS);
SVM_CHECK_CONTEXT(flags_offset, SVM_RV_CTX_FLAGS_OFFSET);
SVM_CHECK_CONTEXT(pc_offset, SVM_RV_CTX_PC_OFFSET);
SVM_CHECK_CONTEXT(host_fpcr, SVM_RV_CTX_HOST_FCSR);
SVM_CHECK_CONTEXT(guest_fpcr, SVM_RV_CTX_GUEST_FCSR);
SVM_CHECK_CONTEXT(fp_environment, SVM_RV_CTX_FP_ENVIRONMENT);
SVM_CHECK_CONTEXT(probe_steps, SVM_RV_CTX_PROBES);
SVM_CHECK_CONTEXT(prepare_atomic, SVM_RV_CTX_PREPARE_ATOMIC);
#undef SVM_CHECK_CONTEXT

void RestoreGuestState(execution::FaultContext& context, std::span<const std::byte> gprs) noexcept {
    if (context.flags_offset == UINT32_MAX) return;
    std::memcpy(reinterpret_cast<std::uint8_t*>(context.state) + context.flags_offset,
                gprs.data() + SVM_RV_FLAGS_REG * 8,
                8);
}

}  // namespace swift::runtime::backend::riscv64
