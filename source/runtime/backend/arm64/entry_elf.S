// Static native ABI: publication, FP switches and cold lookup are explicit.
#include "runtime/backend/arm64/abi_layout.inc"
#include "runtime/exit.inc"
#if defined(__aarch64__)
.text

.macro SVM_PUBLISH_PINNED
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_PIN_OFFSET)]
mov w17, #-1
cmp x16, x17
b.eq 1f
add x16, x19, x16
stp x20, x21, [x16]
stp x22, x23, [x16, #16]
stp x24, x25, [x16, #32]
stp x26, x27, [x16, #48]
ldur x17, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_PIN_SLOTS)]
cmp x17, #16
b.eq 12f
cmp x17, #12
b.ne 1f
stp x12, x13, [x16, #64]
stp x14, x15, [x16, #80]
b 1f
12:
stp x8, x9, [x16, #64]
stp x10, x11, [x16, #80]
stp x12, x13, [x16, #96]
stp x14, x15, [x16, #112]
1:
.endm

.macro SVM_RELOAD_CALLER_PINNED
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_PIN_OFFSET)]
mov w17, #-1
cmp x16, x17
b.eq 8f
add x16, x19, x16
ldur x17, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_PIN_SLOTS)]
cmp x17, #16
b.eq 13f
cmp x17, #12
b.ne 8f
ldp x12, x13, [x16, #64]
ldp x14, x15, [x16, #80]
b 8f
13:
ldp x8, x9, [x16, #64]
ldp x10, x11, [x16, #80]
ldp x12, x13, [x16, #96]
ldp x14, x15, [x16, #112]
8:
.endm

.macro SVM_PUBLISH_VECTORS
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_VECTOR_OFFSET)]
mov w17, #-1
cmp x16, x17
b.eq 26f
add x16, x19, x16
stp q16, q17, [x16]
stp q18, q19, [x16, #32]
stp q20, q21, [x16, #64]
stp q22, q23, [x16, #96]
stp q24, q25, [x16, #128]
stp q26, q27, [x16, #160]
stp q28, q29, [x16, #192]
stp q30, q31, [x16, #224]
26:
.endm

.macro SVM_RELOAD_VECTORS
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_VECTOR_OFFSET)]
mov w17, #-1
cmp x16, x17
b.eq 27f
add x16, x19, x16
ldp q16, q17, [x16]
ldp q18, q19, [x16, #32]
ldp q20, q21, [x16, #64]
ldp q22, q23, [x16, #96]
ldp q24, q25, [x16, #128]
ldp q26, q27, [x16, #160]
ldp q28, q29, [x16, #192]
ldp q30, q31, [x16, #224]
27:
.endm

.macro SVM_PUBLISH_FLAGS
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_FLAGS_OFFSET)]
mov w17, #-1
cmp x16, x17
b.eq 7f
str x1, [x19, x16]
7:
.endm

.macro SVM_SAVE_NATIVE_NZCV
mrs x30, nzcv
.endm

.macro SVM_PACK_SAVED_NATIVE_NZCV
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_FLAGS_OFFSET)]
mov w17, #-1
cmp x16, x17
b.eq 23f
ldr x17, [x19, x16]
b 24f
23:
mov x17, xzr
24:
eor w29, w29, w1
ubfx w29, w29, #4, #1
bfxil x17, x1, #0, #8
ubfx x1, x30, #28, #4
eor x1, x1, #2
bfi x17, x1, #28, #4
bfi x17, x29, #26, #1
mov x1, x17
.endm

.macro SVM_RESTORE_NATIVE_NZCV
eor x17, x1, #0x20000000
msr nzcv, x17
ubfx w29, w1, #22, #5
eor w29, w29, w1
ubfx x1, x1, #0, #8
.endm

.macro SVM_SAVE_HOST_FP
SVM_INSTALL_GUEST_FP
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_FP_ENVIRONMENT)]
cbz x16, 20f
mrs x16, fpcr
mrs x17, fpsr
stp x16, x17, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_HOST_FPCR)]
20:
.endm

.macro SVM_RESTORE_HOST_FP
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_FP_ENVIRONMENT)]
cbz x16, 21f
mrs x17, fpsr
str x17, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_GUEST_FPSR)]
ldp x16, x17, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_HOST_FPCR)]
msr fpcr, x16
msr fpsr, x17
21:
.endm

.macro SVM_INSTALL_GUEST_FP
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_FP_ENVIRONMENT)]
cbz x16, 22f
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_GUEST_FPCR)]
msr fpcr, x16
ldr x17, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_GUEST_FPSR)]
msr fpsr, x17
22:
.endm

.macro SVM_RESOLVE_AND_ENTER
SVM_PUBLISH_PINNED

SVM_PUBLISH_VECTORS

SVM_PUBLISH_FLAGS
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_ACTIVE)]
stlr xzr, [x16]

SVM_RESTORE_HOST_FP
ldp x0, x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_OPAQUE)]
sub x1, x19, #SVM_A64_CTX_STATE
blr x16

SVM_RELOAD_CALLER_PINNED

SVM_RELOAD_VECTORS
SVM_INSTALL_GUEST_FP
cbz x0, 3f
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_ACTIVE)]
ldur x17, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_ANCHOR)]
stlr x17, [x16]
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_FLAGS_OFFSET)]
mov w17, #-1
cmp x16, x17
b.eq 2f
ldr x1, [x19, x16]
b 10f
2:
mov x1, xzr
10:
ldur x17, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_NATIVE_NZCV)]
cbz x17, 11f

SVM_RESTORE_NATIVE_NZCV
11:
br x0
3:
mov x0, #SVM_NATIVE_EXIT_RESOLVE_FAILURE
b SvmNativeExit
.endm

.macro SVM_COUNT_DISPATCH
ldur x4, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_DISPATCHES)]
add x4, x4, #1
stur x4, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_DISPATCHES)]
.endm

.macro SVM_COUNT_PROBE
ldur x4, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_PROBES)]
add x4, x4, #1
stur x4, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_PROBES)]
.endm

.macro SVM_COUNT_HIT
ldur x4, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_HITS)]
add x4, x4, #1
stur x4, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_HITS)]
.endm

.macro SVM_L2_DISPATCH_BODY counted=0, native=0
ldp x2, x6, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_SLOTS)]
cbz x2, 5f
eor x5, x0, x0, lsr #SVM_A64_DISPATCH_HASH_SHIFT
and x5, x5, x6
4:

.if \counted
SVM_COUNT_PROBE
.endif
add x3, x2, x5, lsl #SVM_A64_SLOT_SHIFT
ldp x4, x16, [x3]
sub x4, x4, x0
cbnz x4, 6f
cbz x16, 5f

.if \counted
SVM_COUNT_HIT
.endif
// Pending payloads remain callable but must never enter the ready-only L1.
// A public callback context may omit L1; inline runtime exits always supply it.
cbz x7, 9f
ldr w4, [x3, #SVM_A64_CODE_ID_OFFSET]
add w4, w4, #1
cbz w4, 9f
stp x0, x16, [x7]
9:
br x16
6:
ldrb w4, [x3, #SVM_A64_OCCUPIED_OFFSET]
cbz w4, 5f
add x5, x5, #1
and x5, x5, x6
b 4b
5:
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_PC_OFFSET)]
str x0, [x19, x16]

.if \native
SVM_SAVE_NATIVE_NZCV
SVM_PACK_SAVED_NATIVE_NZCV
.endif

SVM_RESOLVE_AND_ENTER
.endm

.macro SVM_DISPATCH_BODY l2, counted=0
.if \counted
SVM_COUNT_DISPATCH
.endif
ldr x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_L1)]
cbz x16, 7f
ubfiz x7, x0, #SVM_A64_L1_ENTRY_SHIFT, #SVM_A64_L1_BITS
add x7, x16, x7
ldp x4, x16, [x7]
sub x4, x4, x0
cbnz x4, 8f
.if \counted
SVM_COUNT_HIT
.endif
br x16
7:
mov x7, xzr
8:
b \l2
.endm

.p2align 2
.global SvmRunNative
.type SvmRunNative, %function
SvmRunNative:
stp x19, x20, [sp, #-SVM_A64_ROOT_FRAME_BYTES]!
stp x21, x22, [sp, #16]
stp x23, x24, [sp, #32]
stp x25, x26, [sp, #48]
stp x27, x28, [sp, #64]
stp x29, x30, [sp, #80]
stp d8, d9, [sp, #96]
stp d10, d11, [sp, #112]
stp d12, d13, [sp, #128]
stp d14, d15, [sp, #144]
add x19, x0, #SVM_A64_CTX_STATE

SVM_SAVE_HOST_FP
SVM_INSTALL_GUEST_FP
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_PC_OFFSET)]
add x29, x19, x16
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_PIN_OFFSET)]
mov w17, #-1
cmp x16, x17
b.eq 1f
add x16, x19, x16
ldp x20, x21, [x16]
ldp x22, x23, [x16, #16]
ldp x24, x25, [x16, #32]
ldp x26, x27, [x16, #48]
ldur x17, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_PIN_SLOTS)]
cmp x17, #16
b.eq 12f
cmp x17, #12
b.ne 1f
ldp x12, x13, [x16, #64]
ldp x14, x15, [x16, #80]
b 1f
12:
ldp x8, x9, [x16, #64]
ldp x10, x11, [x16, #80]
ldp x12, x13, [x16, #96]
ldp x14, x15, [x16, #112]
1:

SVM_RELOAD_VECTORS
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_FLAGS_OFFSET)]
mov w17, #-1
cmp x16, x17
b.eq 8f
ldr x1, [x19, x16]
b 9f
8:
mov x1, xzr
9:

SVM_RESOLVE_AND_ENTER
.size SvmRunNative, .-SvmRunNative

.p2align 2
.global SvmNativeDispatch
.type SvmNativeDispatch, %function
SvmNativeDispatch:
SVM_DISPATCH_BODY SvmNativeL2Dispatch, 0
.size SvmNativeDispatch, .-SvmNativeDispatch

.p2align 2
.global SvmNativeDispatchCounted
.type SvmNativeDispatchCounted, %function
SvmNativeDispatchCounted:
SVM_DISPATCH_BODY SvmNativeL2DispatchCounted, 1
.size SvmNativeDispatchCounted, .-SvmNativeDispatchCounted

.p2align 2
.global SvmNativeDispatchNZCV
.type SvmNativeDispatchNZCV, %function
SvmNativeDispatchNZCV:
SVM_DISPATCH_BODY SvmNativeL2DispatchNZCV, 0
.size SvmNativeDispatchNZCV, .-SvmNativeDispatchNZCV

.p2align 2
.global SvmNativeDispatchNZCVCounted
.type SvmNativeDispatchNZCVCounted, %function
SvmNativeDispatchNZCVCounted:
SVM_DISPATCH_BODY SvmNativeL2DispatchNZCVCounted, 1
.size SvmNativeDispatchNZCVCounted, .-SvmNativeDispatchNZCVCounted

.p2align 2
.global SvmNativeL2Dispatch
.type SvmNativeL2Dispatch, %function
SvmNativeL2Dispatch:
SVM_L2_DISPATCH_BODY 0, 0
.size SvmNativeL2Dispatch, .-SvmNativeL2Dispatch

.p2align 2
.global SvmNativeL2DispatchCounted
.type SvmNativeL2DispatchCounted, %function
SvmNativeL2DispatchCounted:
SVM_L2_DISPATCH_BODY 1, 0
.size SvmNativeL2DispatchCounted, .-SvmNativeL2DispatchCounted

.p2align 2
.global SvmNativeL2DispatchNZCV
.type SvmNativeL2DispatchNZCV, %function
SvmNativeL2DispatchNZCV:
SVM_L2_DISPATCH_BODY 0, 1
.size SvmNativeL2DispatchNZCV, .-SvmNativeL2DispatchNZCV

.p2align 2
.global SvmNativeL2DispatchNZCVCounted
.type SvmNativeL2DispatchNZCVCounted, %function
SvmNativeL2DispatchNZCVCounted:
SVM_L2_DISPATCH_BODY 1, 1
.size SvmNativeL2DispatchNZCVCounted, .-SvmNativeL2DispatchNZCVCounted

.p2align 2
.global SvmNativePending
.type SvmNativePending, %function
SvmNativePending:
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_PC_OFFSET)]
str x0, [x19, x16]
ldur x17, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_NATIVE_NZCV)]
cbz x17, 30f

SVM_SAVE_NATIVE_NZCV

SVM_PACK_SAVED_NATIVE_NZCV
30:

SVM_RESOLVE_AND_ENTER
.size SvmNativePending, .-SvmNativePending

.p2align 2
.global SvmNativePendingCounted
.type SvmNativePendingCounted, %function
SvmNativePendingCounted:
ldur x4, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_HITS)]
sub x4, x4, #1
stur x4, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_HITS)]
b SvmNativePending
.size SvmNativePendingCounted, .-SvmNativePendingCounted

.p2align 2
.global SvmNativeExit
.type SvmNativeExit, %function
SvmNativeExit:
SVM_PUBLISH_PINNED

SVM_PUBLISH_VECTORS
ldur x16, [x19, #SVM_A64_CTX_REL(SVM_A64_CTX_ACTIVE)]
stlr xzr, [x16]

SVM_RESTORE_HOST_FP
ldp x19, x20, [sp]
ldp x21, x22, [sp, #16]
ldp x23, x24, [sp, #32]
ldp x25, x26, [sp, #48]
ldp x27, x28, [sp, #64]
ldp x29, x30, [sp, #80]
ldp d8, d9, [sp, #96]
ldp d10, d11, [sp, #112]
ldp d12, d13, [sp, #128]
ldp d14, d15, [sp, #144]
add sp, sp, #SVM_A64_ROOT_FRAME_BYTES
ret
.size SvmNativeExit, .-SvmNativeExit

#endif
#if defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif
