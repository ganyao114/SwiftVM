#include "abi_layout.inc"
#include "patch.h"
#include "relocate.h"
#include "runtime.h"
#include "runtime/arch/guest.h"
#include "runtime/execution/dispatch.h"
#include "runtime/execution/fault.h"

#include <cstring>

namespace swift::runtime::backend::loongarch64 {
using execution::NativeRunContext;

Runtime::Runtime(bool diagnostic) noexcept {
    options.native_fp_environment = true;
    options.dispatcher = reinterpret_cast<std::uintptr_t>(
            diagnostic ? &execution::SvmNativeDispatchCounted : &execution::SvmNativeDispatch);
    options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);
}

execution::HostBackendOps Runtime::Bind() noexcept {
    execution::HostBackendOps ops;
    ops.context = this;
    ops.target = backend::loongarch64::KnownTargetInfo();
    ops.configure = [](void* raw, const GuestLayout& layout) noexcept {
        auto& options = static_cast<Runtime*>(raw)->options;
        options.flags_offset = layout.flags_offset;
        options.pc_offset = layout.pc_offset;
        options.mxcsr_offset = layout.numeric_control_offset;
    };
    ops.pinned_uniform_slots = 0;
    ops.pinned_vectors = false;
    ops.compile = [](void* raw,
                     const ir::Program& program,
                     execution::HostCompileRequest request) -> backend::CompileResult& {
        auto& self = *static_cast<Runtime*>(raw);
        auto policy = self.options;
        policy.entry_pc = request.entry_pc;
        policy.relocatable = request.relocatable;
        policy.fault_locations = request.fault_locations;
        backend::loongarch64::Compile(program, policy, self.output, self.allocation);
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
        if (self.allocation.homes.capacity() >
            8u * 1024 * 1024 / sizeof(backend::loongarch64::Home))
            std::vector<backend::loongarch64::Home>{}.swap(self.allocation.homes);
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
        return backend::loongarch64::InitializeNativeLink(write, execute, site, ready, dual);
    };
    ops.patch_link = backend::loongarch64::PatchNativeLink;
    ops.unlink = backend::loongarch64::UnlinkNativeLink;
    ops.update_fp = [](void*, NativeRunContext& native, std::uint32_t mxcsr) noexcept {
        constexpr unsigned rounding[]{0, 3, 2, 1};  // Canonical RC -> LA RNE/RDN/RUP/RTZ.
        native.guest_fpcr = rounding[(mxcsr >> 13) & 3] << 8;
    };
    ops.floating_status = [](const execution::NativeRunContext& n) noexcept -> std::uint32_t {
        const auto f = n.guest_fpsr >> 16;
        return ((f >> 4) & 1) | ((f & 8) >> 1) | ((f & 4) << 1) | ((f & 2) << 3) | ((f & 1) << 5);
    };
    ops.restore_host_fp = [](const NativeRunContext& native) noexcept {
#if defined(__loongarch64) || (defined(__loongarch__) && __loongarch_grlen == 64)
        asm volatile("movgr2fcsr $fcsr0, %0" ::"r"(native.host_fpcr) : "memory");
#else
        (void)native;
#endif
    };
#if !defined(__loongarch64) && !(defined(__loongarch__) && __loongarch_grlen == 64)
    ops.compile = nullptr;
    ops.target.mode = ir::TargetMode::Unknown;
#endif
    return ops;
}

#define SVM_CHECK_CONTEXT(field, macro)                                                            \
    static_assert(offsetof(execution::NativeRunContext, field) == macro)
SVM_CHECK_CONTEXT(state, SVM_LA_CTX_STATE);
SVM_CHECK_CONTEXT(opaque, SVM_LA_CTX_OPAQUE);
SVM_CHECK_CONTEXT(resolve, SVM_LA_CTX_RESOLVE);
SVM_CHECK_CONTEXT(slots, SVM_LA_CTX_SLOTS);
SVM_CHECK_CONTEXT(mask, SVM_LA_CTX_MASK);
SVM_CHECK_CONTEXT(active, SVM_LA_CTX_ACTIVE);
SVM_CHECK_CONTEXT(anchor, SVM_LA_CTX_ANCHOR);
SVM_CHECK_CONTEXT(dispatcher_entries, SVM_LA_CTX_DISPATCHES);
SVM_CHECK_CONTEXT(cache_hits, SVM_LA_CTX_HITS);
SVM_CHECK_CONTEXT(flags_offset, SVM_LA_CTX_FLAGS_OFFSET);
SVM_CHECK_CONTEXT(pc_offset, SVM_LA_CTX_PC_OFFSET);
SVM_CHECK_CONTEXT(host_fpcr, SVM_LA_CTX_HOST_FCSR);
SVM_CHECK_CONTEXT(guest_fpcr, SVM_LA_CTX_GUEST_FCSR);
SVM_CHECK_CONTEXT(fp_environment, SVM_LA_CTX_FP_ENVIRONMENT);
SVM_CHECK_CONTEXT(probe_steps, SVM_LA_CTX_PROBES);
SVM_CHECK_CONTEXT(atomic_service, SVM_LA_CTX_ATOMIC_SERVICE);
#undef SVM_CHECK_CONTEXT

void RestoreGuestState(execution::FaultContext& context, std::span<const std::byte> gprs) noexcept {
    if (context.flags_offset == UINT32_MAX) return;
    std::memcpy(reinterpret_cast<std::uint8_t*>(context.state) + context.flags_offset,
                gprs.data() + SVM_LA_FLAGS_REG * 8,
                8);
}

}  // namespace swift::runtime::backend::loongarch64
