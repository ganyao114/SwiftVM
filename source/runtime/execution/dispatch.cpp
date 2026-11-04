#include "dispatch.h"

namespace swift::runtime::execution {

#if defined(__aarch64__)

#if defined(__APPLE__)
#define SVM_NATIVE_EXIT "_SvmNativeExit"
#define SVM_NATIVE_PENDING "_SvmNativePending"
#else
#define SVM_NATIVE_EXIT "SvmNativeExit"
#define SVM_NATIVE_PENDING "SvmNativePending"
#endif

// Only cold resolution and the root exit publish these private state slots.
// Normal ABI calls retain the low eight pins. Optional caller-saved pins use
// the same state range and are reloaded after cold calls.
#define SVM_PUBLISH_PINNED \
    "ldr x16, [x28, #72]\n" \
    "mov w17, #-1\n" \
    "cmp x16, x17\n" \
    "b.eq 1f\n" \
    "add x16, x19, x16\n" \
    "stp x20, x21, [x16]\n" \
    "stp x22, x23, [x16, #16]\n" \
    "stp x24, x25, [x16, #32]\n" \
    "stp x26, x27, [x16, #48]\n" \
    "ldr x17, [x28, #96]\n" \
    "cmp x17, #16\n" \
    "b.eq 12f\n" \
    "cmp x17, #12\n" \
    "b.ne 1f\n" \
    "stp x12, x13, [x16, #64]\n" \
    "stp x14, x15, [x16, #80]\n" \
    "b 1f\n" \
    "12:\n" \
    "stp x8, x9, [x16, #64]\n" \
    "stp x10, x11, [x16, #80]\n" \
    "stp x12, x13, [x16, #96]\n" \
    "stp x14, x15, [x16, #112]\n" \
    "1:\n"

// Keep the resolver's returned x0 intact, including failure. NativeExit will
// publish these real reloaded values rather than C++ caller-clobbered scratch.
#define SVM_RELOAD_CALLER_PINNED \
    "ldr x16, [x28, #72]\n" \
    "mov w17, #-1\n" \
    "cmp x16, x17\n" \
    "b.eq 8f\n" \
    "add x16, x19, x16\n" \
    "ldr x17, [x28, #96]\n" \
    "cmp x17, #16\n" \
    "b.eq 13f\n" \
    "cmp x17, #12\n" \
    "b.ne 8f\n" \
    "ldp x12, x13, [x16, #64]\n" \
    "ldp x14, x15, [x16, #80]\n" \
    "b 8f\n" \
    "13:\n" \
    "ldp x8, x9, [x16, #64]\n" \
    "ldp x10, x11, [x16, #80]\n" \
    "ldp x12, x13, [x16, #96]\n" \
    "ldp x14, x15, [x16, #112]\n" \
    "8:\n"

// XMM pins are all caller-saved. Only root/cold/exit touch their state bank;
// warm directory hits and direct links retain the physical registers.
#define SVM_PUBLISH_VECTORS \
    "ldr x16, [x28, #152]\n" \
    "mov w17, #-1\n" \
    "cmp x16, x17\n" \
    "b.eq 26f\n" \
    "add x16, x19, x16\n" \
    "stp q16, q17, [x16]\n" \
    "stp q18, q19, [x16, #32]\n" \
    "stp q20, q21, [x16, #64]\n" \
    "stp q22, q23, [x16, #96]\n" \
    "stp q24, q25, [x16, #128]\n" \
    "stp q26, q27, [x16, #160]\n" \
    "stp q28, q29, [x16, #192]\n" \
    "stp q30, q31, [x16, #224]\n" \
    "26:\n"
#define SVM_RELOAD_VECTORS \
    "ldr x16, [x28, #152]\n" \
    "mov w17, #-1\n" \
    "cmp x16, x17\n" \
    "b.eq 27f\n" \
    "add x16, x19, x16\n" \
    "ldp q16, q17, [x16]\n" \
    "ldp q18, q19, [x16, #32]\n" \
    "ldp q20, q21, [x16, #64]\n" \
    "ldp q22, q23, [x16, #96]\n" \
    "ldp q24, q25, [x16, #128]\n" \
    "ldp q26, q27, [x16, #160]\n" \
    "ldp q28, q29, [x16, #192]\n" \
    "ldp q30, q31, [x16, #224]\n" \
    "27:\n"

#define SVM_PUBLISH_FLAGS \
    "ldr x16, [x28, #80]\n" \
    "mov w17, #-1\n" \
    "cmp x16, x17\n" \
    "b.eq 7f\n" \
    "str x1, [x19, x16]\n" \
    "7:\n"

// The probe uses only non-S arithmetic. On a cold miss, merge the raw PF/AF
// carriers and saved NZCV into the original packed field's reserved bits.
#define SVM_SAVE_NATIVE_NZCV \
    "mrs x30, nzcv\n"
#define SVM_PACK_SAVED_NATIVE_NZCV \
    "ldr x16, [x28, #80]\n" \
    "mov w17, #-1\n" \
    "cmp x16, x17\n" \
    "b.eq 23f\n" \
    "ldr x17, [x19, x16]\n" \
    "b 24f\n" \
    "23:\n" \
    "mov x17, xzr\n" \
    "24:\n" \
    "eor w29, w29, w1\n" \
    "ubfx w29, w29, #4, #1\n" \
    "bfxil x17, x1, #0, #8\n" \
    "ubfx x1, x30, #28, #4\n" \
    "eor x1, x1, #2\n" \
    "bfi x17, x1, #28, #4\n" \
    "bfi x17, x29, #26, #1\n" \
    "mov x1, x17\n"
// Cold entry restores from the canonical packed state after the resolver.
#define SVM_RESTORE_NATIVE_NZCV \
    "eor x17, x1, #0x20000000\n" \
    "msr nzcv, x17\n" \
    "ubfx w29, w1, #22, #5\n" \
    "eor w29, w29, w1\n" \
    "ubfx x1, x1, #0, #8\n"

// Only root/cold/exit boundaries switch FP environments. A default context
// leaves ambient FP state untouched; hot directory hits execute none of this.
#define SVM_SAVE_HOST_FP \
    "ldr x16, [x28, #136]\n" \
    "cbz x16, 20f\n" \
    "mrs x16, fpcr\n" \
    "mrs x17, fpsr\n" \
    "stp x16, x17, [x28, #112]\n" \
    "20:\n"
#define SVM_RESTORE_HOST_FP \
    "ldr x16, [x28, #136]\n" \
    "cbz x16, 21f\n" \
    "ldp x16, x17, [x28, #112]\n" \
    "msr fpcr, x16\n" \
    "msr fpsr, x17\n" \
    "21:\n"
#define SVM_INSTALL_GUEST_FP \
    "ldr x16, [x28, #136]\n" \
    "cbz x16, 22f\n" \
    "ldr x16, [x28, #128]\n" \
    "msr fpcr, x16\n" \
    "msr fpsr, xzr\n" \
    "22:\n"

// One actual resolver call. All state used afterward lives in callee-saved
// registers or the Process-owned context. Release stores match the signal
// handler's acquire access to the lock-free active pointer.
#define SVM_RESOLVE_AND_ENTER \
    SVM_PUBLISH_PINNED \
    SVM_PUBLISH_VECTORS \
    SVM_PUBLISH_FLAGS \
    "ldr x16, [x28, #40]\n" \
    "stlr xzr, [x16]\n" \
    SVM_RESTORE_HOST_FP \
    "ldp x0, x16, [x28, #8]\n" \
    "mov x1, x28\n" \
    "blr x16\n" \
    SVM_RELOAD_CALLER_PINNED \
    SVM_RELOAD_VECTORS \
    "cbz x0, 3f\n" \
    SVM_INSTALL_GUEST_FP \
    "ldr x16, [x28, #40]\n" \
    "ldr x17, [x28, #48]\n" \
    "stlr x17, [x16]\n" \
    "ldr x16, [x28, #80]\n" \
    "mov w17, #-1\n" \
    "cmp x16, x17\n" \
    "b.eq 2f\n" \
    "ldr x1, [x19, x16]\n" \
    "b 10f\n" \
    "2:\n" \
    "mov x1, xzr\n" \
    "10:\n" \
    "ldr x17, [x28, #104]\n" \
    "cbz x17, 11f\n" \
    SVM_RESTORE_NATIVE_NZCV \
    "11:\n" \
    "br x0\n" \
    "3:\n" \
    "mov x0, #2\n" \
    "b " SVM_NATIVE_EXIT "\n"

extern "C" __attribute__((naked)) std::uint64_t SvmRunNative(GuestState*, NativeRunContext*) noexcept {
    asm volatile(
        "stp x19, x20, [sp, #-160]!\n"
        "stp x21, x22, [sp, #16]\n"
        "stp x23, x24, [sp, #32]\n"
        "stp x25, x26, [sp, #48]\n"
        "stp x27, x28, [sp, #64]\n"
        "stp x29, x30, [sp, #80]\n"
        "stp d8, d9, [sp, #96]\n"
        "stp d10, d11, [sp, #112]\n"
        "stp d12, d13, [sp, #128]\n"
        "stp d14, d15, [sp, #144]\n"
        "mov x19, x0\n"
        "mov x28, x1\n"
        "str x19, [x28]\n"
        SVM_SAVE_HOST_FP
        "ldr x16, [x28, #88]\n"
        "add x29, x19, x16\n"
        "ldr x16, [x28, #72]\n"
        "mov w17, #-1\n"
        "cmp x16, x17\n"
        "b.eq 1f\n"
        "add x16, x19, x16\n"
        "ldp x20, x21, [x16]\n"
        "ldp x22, x23, [x16, #16]\n"
        "ldp x24, x25, [x16, #32]\n"
        "ldp x26, x27, [x16, #48]\n"
        "ldr x17, [x28, #96]\n"
        "cmp x17, #16\n"
        "b.eq 12f\n"
        "cmp x17, #12\n"
        "b.ne 1f\n"
        "ldp x12, x13, [x16, #64]\n"
        "ldp x14, x15, [x16, #80]\n"
        "b 1f\n"
        "12:\n"
        "ldp x8, x9, [x16, #64]\n"
        "ldp x10, x11, [x16, #80]\n"
        "ldp x12, x13, [x16, #96]\n"
        "ldp x14, x15, [x16, #112]\n"
        "1:\n"
        SVM_RELOAD_VECTORS
        // Initialize the real shadow before the shared cold publisher; x1
        // originally held the context argument, not guest flags.
        "ldr x16, [x28, #80]\n"
        "mov w17, #-1\n"
        "cmp x16, x17\n"
        "b.eq 8f\n"
        "ldr x1, [x19, x16]\n"
        "b 9f\n"
        "8:\n"
        "mov x1, xzr\n"
        "9:\n"
        SVM_RESOLVE_AND_ENTER);
}

#define SVM_COUNT_DISPATCH \
    "ldr x4, [x28, #56]\n" \
    "add x4, x4, #1\n" \
    "str x4, [x28, #56]\n"
#define SVM_COUNT_PROBE \
    "ldr x4, [x28, #144]\n" \
    "add x4, x4, #1\n" \
    "str x4, [x28, #144]\n"
#define SVM_COUNT_HIT \
    "ldr x4, [x28, #64]\n" \
    "add x4, x4, #1\n" \
    "str x4, [x28, #64]\n"

// Matching tags can go straight to the ready entry. A matching null entry
// is either pending or an empty PC-zero slot; both take the same cold path.
// A callable cold payload does not prove occupancy after a tag mismatch.
// Only this cold path publishes x0 before the resolver reuses that register.
#define SVM_DISPATCH_BODY(count_entry, count_probe, count_hit, cold_pack) \
    count_entry \
    "ldp x2, x6, [x28, #24]\n" \
    "cbz x2, 5f\n" \
    "eor x5, x0, x0, lsr #%c[hash_shift]\n" \
    "and x5, x5, x6\n" \
    "4:\n" \
    count_probe \
    "add x3, x2, x5, lsl #5\n" \
    "ldp x4, x16, [x3]\n" \
    "sub x4, x4, x0\n" \
    "cbnz x4, 6f\n" \
    "cbz x16, 5f\n" \
    count_hit \
    "br x16\n" \
    "6:\n" \
    "ldrb w4, [x3, #24]\n" \
    "cbz w4, 5f\n" \
    "add x5, x5, #1\n" \
    "and x5, x5, x6\n" \
    "b 4b\n" \
    "5:\n" \
    "ldr x16, [x28, #88]\n" \
    "str x0, [x19, x16]\n" \
    cold_pack \
    SVM_RESOLVE_AND_ENTER

extern "C" __attribute__((naked)) void SvmNativeDispatch() noexcept {
    asm volatile(SVM_DISPATCH_BODY("", "", "", "")
                 : : [hash_shift] "i" (backend::arm64::abi::DispatchHashShift));
}
extern "C" __attribute__((naked)) void SvmNativeDispatchCounted() noexcept {
    asm volatile(SVM_DISPATCH_BODY(SVM_COUNT_DISPATCH, SVM_COUNT_PROBE, SVM_COUNT_HIT, "")
                 : : [hash_shift] "i" (backend::arm64::abi::DispatchHashShift));
}
extern "C" __attribute__((naked)) void SvmNativeDispatchNZCV() noexcept {
    asm volatile(SVM_DISPATCH_BODY("", "", "", SVM_SAVE_NATIVE_NZCV SVM_PACK_SAVED_NATIVE_NZCV)
                 : : [hash_shift] "i" (backend::arm64::abi::DispatchHashShift));
}
extern "C" __attribute__((naked)) void SvmNativeDispatchNZCVCounted() noexcept {
    asm volatile(SVM_DISPATCH_BODY(SVM_COUNT_DISPATCH, SVM_COUNT_PROBE, SVM_COUNT_HIT,
                                  SVM_SAVE_NATIVE_NZCV SVM_PACK_SAVED_NATIVE_NZCV)
                 : : [hash_shift] "i" (backend::arm64::abi::DispatchHashShift));
}

extern "C" __attribute__((naked)) void SvmNativePending() noexcept {
    asm volatile(
        "ldr x16, [x28, #88]\n"
        "str x0, [x19, x16]\n"
        "ldr x17, [x28, #104]\n"
        "cbz x17, 30f\n"
        SVM_SAVE_NATIVE_NZCV
        SVM_PACK_SAVED_NATIVE_NZCV
        "30:\n"
        SVM_RESOLVE_AND_ENTER);
}

extern "C" __attribute__((naked)) void SvmNativePendingCounted() noexcept {
    asm volatile(
        "ldr x4, [x28, #64]\n"
        "sub x4, x4, #1\n" // Non-S: guest NZCV must survive until the real cold pack.
        "str x4, [x28, #64]\n"
        "b " SVM_NATIVE_PENDING "\n");
}

extern "C" __attribute__((naked)) void SvmNativeExit() noexcept {
    asm volatile(
        SVM_PUBLISH_PINNED
        SVM_PUBLISH_VECTORS
        "ldr x16, [x28, #40]\n"
        "stlr xzr, [x16]\n"
        SVM_RESTORE_HOST_FP
        "ldp x19, x20, [sp]\n"
        "ldp x21, x22, [sp, #16]\n"
        "ldp x23, x24, [sp, #32]\n"
        "ldp x25, x26, [sp, #48]\n"
        "ldp x27, x28, [sp, #64]\n"
        "ldp x29, x30, [sp, #80]\n"
        "ldp d8, d9, [sp, #96]\n"
        "ldp d10, d11, [sp, #112]\n"
        "ldp d12, d13, [sp, #128]\n"
        "ldp d14, d15, [sp, #144]\n"
        "add sp, sp, #160\n"
        "ret\n");
}

#undef SVM_RESOLVE_AND_ENTER
#undef SVM_SAVE_NATIVE_NZCV
#undef SVM_PACK_SAVED_NATIVE_NZCV
#undef SVM_RESTORE_NATIVE_NZCV
#undef SVM_INSTALL_GUEST_FP
#undef SVM_RESTORE_HOST_FP
#undef SVM_SAVE_HOST_FP
#undef SVM_DISPATCH_BODY
#undef SVM_COUNT_DISPATCH
#undef SVM_COUNT_PROBE
#undef SVM_COUNT_HIT
#undef SVM_PUBLISH_FLAGS
#undef SVM_RELOAD_VECTORS
#undef SVM_PUBLISH_VECTORS
#undef SVM_RELOAD_CALLER_PINNED
#undef SVM_PUBLISH_PINNED
#undef SVM_NATIVE_EXIT
#undef SVM_NATIVE_PENDING

#else

extern "C" std::uint64_t SvmRunNative(GuestState*, NativeRunContext*) noexcept { return 2; }
extern "C" void SvmNativeDispatch() noexcept {}
extern "C" void SvmNativeDispatchCounted() noexcept {}
extern "C" void SvmNativeDispatchNZCV() noexcept {}
extern "C" void SvmNativeDispatchNZCVCounted() noexcept {}
extern "C" void SvmNativePending() noexcept {}
extern "C" void SvmNativePendingCounted() noexcept {}
extern "C" void SvmNativeExit() noexcept {}

#endif

} // namespace swift::runtime::execution
