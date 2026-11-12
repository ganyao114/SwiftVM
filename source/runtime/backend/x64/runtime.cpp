#include <cstring>
#include "abi.h"
#include "abi_layout.inc"
#include "patch.h"
#include "relocate.h"
#include "runtime.h"
#include "runtime/arch/arm64/numeric.h"
#include "runtime/execution/dispatch.h"
#include "runtime/execution/fault.h"
#if defined(__x86_64__)
#include <cpuid.h>
#endif
namespace swift::runtime::backend::x64 {
Runtime::Runtime(bool diagnostic) noexcept : diagnostic(diagnostic) {
    options.dispatcher = reinterpret_cast<std::uintptr_t>(
            diagnostic ? &execution::SvmNativeDispatchCounted : &execution::SvmNativeDispatch);
    options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);
    options.native_fp_environment = true;
#if defined(__x86_64__)
    unsigned a, b, c, d;
    if (__get_cpuid(1, &a, &b, &c, &d)) {
        options.ssse3 = c & (1u << 9);
        options.sse41 = c & (1u << 19);
        options.sse42 = c & (1u << 20);
        options.aes = c & (1u << 25);
        options.pclmul = c & 2;
        options.cx16 = c & (1u << 13);
        if ((c & (1u << 12)) && (c & (1u << 27)) && (c & (1u << 28))) {
            unsigned lo, hi;
            asm volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
            options.fma = (lo & 6) == 6;
        }
    }
    if (__get_cpuid_max(0, nullptr) >= 7) {
        __cpuid_count(7, 0, a, b, c, d);
        options.sha = b & (1u << 29);
    }
#endif
    features = (options.ssse3 ? abi::Ssse3 : 0) | (options.sse41 ? abi::Sse41 : 0) |
               (options.sse42 ? abi::Sse42 : 0) | (options.fma ? abi::Fma : 0) |
               (options.aes ? abi::Aes : 0) | (options.pclmul ? abi::Pclmul : 0) |
               (options.sha ? abi::Sha : 0) | (options.cx16 ? abi::Cx16 : 0);
}

execution::HostBackendOps Runtime::Bind() noexcept {
    execution::HostBackendOps o;
    o.context = this;
    o.target = KnownTargetInfo();
    o.isa_features = features;
    o.pinned_uniform_slots = 2;
    o.pinned_uniform_default = 0;
    o.thread_dispatch_cache = true;
    o.capabilities.native_eflags = true;
    o.specializes_arm_fpcr = true;
    o.configure = [](void* p, const GuestLayout& l) noexcept {
        auto& v = *static_cast<Runtime*>(p);
        v.options.flags_offset = l.flags_offset;
        v.options.pc_offset = l.pc_offset;
        v.options.mxcsr_offset = l.numeric_control_offset;
        v.options.pinned_uniform_offset = l.pinned_uniform_slots ? l.gprs_offset : UINT32_MAX;
        v.options.pinned_uniform_slots = l.pinned_uniform_slots;
        v.options.local_flags = true;
        v.options.linked_flags = v.options.local_flags;
        v.options.arm_fpcr_offset =
                l.isa == GuestISA::Arm64 ? offsetof(runtime::arm64::GuestState, fpcr) : UINT32_MAX;
        v.options.dispatcher = reinterpret_cast<std::uintptr_t>(
                v.options.local_flags ? (v.diagnostic ? &execution::SvmNativeDispatchNZCVCounted
                                                      : &execution::SvmNativeDispatchNZCV)
                                      : (v.diagnostic ? &execution::SvmNativeDispatchCounted
                                                      : &execution::SvmNativeDispatch));
    };
    o.compile =
            [](void* p, const ir::Program& s, execution::HostCompileRequest r) -> CompileResult& {
        auto& v = *static_cast<Runtime*>(p);
        auto c = v.options;
        c.entry_pc = r.entry_pc;
        c.fault_locations = r.fault_locations;
        c.relocatable = r.relocatable;
        c.local_flags &= r.private_state && r.fault_locations;
        c.linked_flags &= c.local_flags;
        if (v.options.arm_fpcr_offset != UINT32_MAX)
            c.arm_fpcr = runtime::arm64::FPCRForDomain(r.fp_mode >> 2);
        Compile(s, c, v.output, v.allocation);
        return v.output;
    };
    o.relocate = [](void* p, std::span<std::uint8_t> w, std::span<const Relocation> r) noexcept {
        return Relocate(w, r, static_cast<Runtime*>(p)->options);
    };
    o.trim = [](void* p) noexcept {
        auto& v = *static_cast<Runtime*>(p);
        if (v.output.words.capacity() > 1024 * 1024)
            std::vector<std::uint32_t>{}.swap(v.output.words);
        if (v.allocation.homes.capacity() > 1024 * 1024)
            std::vector<Home>{}.swap(v.allocation.homes);
    };
    o.release = [](void* p) noexcept {
        auto& v = *static_cast<Runtime*>(p);
        v.output = {};
        v.allocation = {};
    };
    o.initialize_link = [](void*,
                           std::span<std::uint8_t> w,
                           std::uintptr_t rx,
                           bool dual,
                           const LinkSite& s,
                           std::uintptr_t ready) noexcept {
        return InitializeNativeLink(w, rx, s, ready, dual);
    };
    o.patch_link = PatchNativeLink;
    o.unlink = UnlinkNativeLink;
    o.synchronize_code = [](std::uint8_t* w, const std::uint8_t* e, std::size_t n) noexcept {
        SynchronizeCode(w, e, n);
        return 0;
    };
    o.update_fp = [](void* p, execution::NativeRunContext& n, std::uint32_t control) noexcept {
        n.guest_fpcr = (control & 0xe040) | 0x1f80;
        n.native_eflags = static_cast<Runtime*>(p)->options.linked_flags;
    };
    o.floating_status = [](const execution::NativeRunContext& n) noexcept -> std::uint32_t {
        return n.guest_fpcr & 63;
    };
    o.restore_host_fp = [](const execution::NativeRunContext& n) noexcept {
#if defined(__x86_64__)
        const unsigned control = n.host_fpcr;
        asm volatile("ldmxcsr %0" ::"m"(control) : "memory");
#else
        (void)n;
#endif
    };
#if !defined(__x86_64__)
    o.compile = nullptr;
    o.target.mode = ir::TargetMode::Unknown;
#endif
    return o;
}

static_assert(offsetof(execution::NativeRunContext, root_sp) == SVM_X64_CTX_ROOT_SP);
static_assert(offsetof(execution::NativeRunContext, phase) == SVM_X64_CTX_PHASE);
static_assert(offsetof(execution::NativeRunContext, pending_events) == SVM_X64_CTX_PENDING_EVENTS);
static_assert(offsetof(execution::NativeRunContext, faults) == SVM_X64_CTX_FAULTS);
static_assert(offsetof(execution::NativeRunContext, state) == SVM_X64_CTX_STATE);
static_assert(offsetof(execution::NativeRunContext, guest_fpsr) == SVM_X64_CTX_GUEST_FPSR);
static_assert(offsetof(execution::NativeRunContext, l1) == SVM_X64_CTX_L1);
static_assert(offsetof(execution::NativeRunContext, native_eflags) == SVM_X64_CTX_NATIVE_EFLAGS);
static_assert(offsetof(execution::NativeRunContext, eflags) == SVM_X64_CTX_EFLAGS);
}  // namespace swift::runtime::backend::x64
