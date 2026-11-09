// Linked RV64G ABI. Only the root owns host callee-save and FP state.
#include "runtime/backend/riscv64/abi_layout.inc"
#include "runtime/backend/dispatch_layout.inc"
#include "runtime/exit.inc"
#if defined(__riscv) && __riscv_xlen == 64
.text
.option push
.option norvc
.option norelax

.macro SVM_RV_PUBLISH_FLAGS
ld t0, SVM_RV_CTX_REL(SVM_RV_CTX_FLAGS_OFFSET)(s1)
li t1, 0xffffffff
beq t0, t1, 90f
add t0, s1, t0
sd s11, 0(t0)
90:
.endm

.macro SVM_RV_RELOAD_FLAGS
ld t0, SVM_RV_CTX_REL(SVM_RV_CTX_FLAGS_OFFSET)(s1)
li t1, 0xffffffff
beq t0, t1, 91f
add t0, s1, t0
ld s11, 0(t0)
j 92f
91:
li s11, 0
92:
.endm

.macro SVM_RV_CAPTURE_GUEST_FP
ld t0, SVM_RV_CTX_REL(SVM_RV_CTX_FP_ENVIRONMENT)(s1)
beqz t0, 93f
frflags t0
sd t0, SVM_RV_CTX_REL(SVM_RV_CTX_GUEST_FPSR)(s1)
93:
.endm
.macro SVM_RV_RESTORE_HOST_FP_ONLY
ld t0, SVM_RV_CTX_REL(SVM_RV_CTX_FP_ENVIRONMENT)(s1)
beqz t0, 96f
ld t0, SVM_RV_CTX_REL(SVM_RV_CTX_HOST_FCSR)(s1)
fscsr t0
96:
.endm
.macro SVM_RV_RESTORE_HOST_FP
SVM_RV_CAPTURE_GUEST_FP
SVM_RV_RESTORE_HOST_FP_ONLY
.endm
.macro SVM_RV_PHASE value
li t0, \value
sd t0, SVM_RV_CTX_REL(SVM_RV_CTX_PHASE)(s1)
.endm

.macro SVM_RV_INSTALL_GUEST_FP
ld t0, SVM_RV_CTX_REL(SVM_RV_CTX_FP_ENVIRONMENT)(s1)
beqz t0, 94f
ld t0, SVM_RV_CTX_REL(SVM_RV_CTX_GUEST_FCSR)(s1)
ld t1, SVM_RV_CTX_REL(SVM_RV_CTX_GUEST_FPSR)(s1)
or t0, t0, t1
fscsr t0
94:
.endm

.macro SVM_RV_CLEAR_ACTIVE
ld t0, SVM_RV_CTX_REL(SVM_RV_CTX_ACTIVE)(s1)
fence rw,w
sd zero, 0(t0)
.endm

.macro SVM_RV_RESOLVE_AND_ENTER
SVM_RV_PUBLISH_FLAGS
SVM_RV_CLEAR_ACTIVE
SVM_RV_RESTORE_HOST_FP
SVM_RV_PHASE SVM_NATIVE_PHASE_RUNTIME
ld a0, SVM_RV_CTX_REL(SVM_RV_CTX_OPAQUE)(s1)
addi a1, s1, -SVM_RV_CTX_STATE
ld t0, SVM_RV_CTX_REL(SVM_RV_CTX_RESOLVE)(s1)
jalr ra, t0, 0
SVM_RV_INSTALL_GUEST_FP
beqz a0, 95f
SVM_RV_RELOAD_FLAGS
ld t0, SVM_RV_CTX_REL(SVM_RV_CTX_ACTIVE)(s1)
ld t1, SVM_RV_CTX_REL(SVM_RV_CTX_ANCHOR)(s1)
fence rw,w
sd t1, 0(t0)
SVM_RV_PHASE SVM_NATIVE_PHASE_GUEST
jr a0
95:
li a0, SVM_NATIVE_EXIT_RESOLVE_FAILURE
j SvmNativeExit
.endm

.macro SVM_RV_COUNT field
ld t4, SVM_RV_CTX_REL(\field)(s1)
addi t4, t4, 1
sd t4, SVM_RV_CTX_REL(\field)(s1)
.endm

.macro SVM_RV_DISPATCH name, counted=0
.if \counted
SVM_RV_COUNT SVM_RV_CTX_DISPATCHES
.endif
// t6 retains this transfer's L1 slot through the one L2 probe loop.
// A legacy context's null L1 selects its existing L2-only path.
ld t6, SVM_RV_CTX_REL(SVM_RV_CTX_L1)(s1)
beqz t6, 6f
slli t2, a0, (64 - SVM_DISPATCH_L1_BITS)
srli t2, t2, (64 - SVM_DISPATCH_L1_BITS - SVM_DISPATCH_L1_ENTRY_SHIFT)
add t6, t6, t2
.global \name\()L1Probe
.hidden \name\()L1Probe
\name\()L1Probe:
ld t5, 0(t6)
bne t5, a0, 6f
ld t5, 8(t6)
.if \counted
SVM_RV_COUNT SVM_RV_CTX_HITS
.endif
jr t5
6:
ld t0, SVM_RV_CTX_REL(SVM_RV_CTX_SLOTS)(s1)
ld t1, SVM_RV_CTX_REL(SVM_RV_CTX_MASK)(s1)
beqz t0, 3f
srli t2, a0, SVM_DISPATCH_HASH_SHIFT
xor t2, t2, a0
and t2, t2, t1
1:
.if \counted
SVM_RV_COUNT SVM_RV_CTX_PROBES
.endif
slli t3, t2, SVM_DISPATCH_SLOT_SHIFT
add t3, t3, t0
ld t5, 0(t3)
bne t5, a0, 2f
ld t5, 8(t3)
beqz t5, 3f
.if \counted
SVM_RV_COUNT SVM_RV_CTX_HITS
.endif
// LW sign-extends UINT32_MAX; pending callable thunks never enter L1.
lw t4, SVM_DISPATCH_CODE_ID_OFFSET(t3)
addiw t4, t4, 1
beqz t4, 7f
beqz t6, 7f
sd t5, 8(t6)
sd a0, 0(t6)
7:
jr t5
2:
lbu t5, SVM_DISPATCH_OCCUPIED_OFFSET(t3)
beqz t5, 3f
addi t2, t2, 1
and t2, t2, t1
j 1b
3:
ld t0, SVM_RV_CTX_REL(SVM_RV_CTX_PC_OFFSET)(s1)
add t0, s1, t0
sd a0, 0(t0)
SVM_RV_RESOLVE_AND_ENTER
.endm

.p2align 2
.global SvmRunNative
.type SvmRunNative, @function
SvmRunNative:
addi sp, sp, -SVM_RV_ROOT_FRAME_BYTES
sd ra, 0(sp)
sd s0, 8(sp)
sd s1, 16(sp)
sd s2, 24(sp)
sd s3, 32(sp)
sd s4, 40(sp)
sd s5, 48(sp)
sd s6, 56(sp)
sd s7, 64(sp)
sd s8, 72(sp)
sd s9, 80(sp)
sd s10, 88(sp)
sd s11, 96(sp)
fsd fs0, 112(sp)
fsd fs1, 120(sp)
fsd fs2, 128(sp)
fsd fs3, 136(sp)
fsd fs4, 144(sp)
fsd fs5, 152(sp)
fsd fs6, 160(sp)
fsd fs7, 168(sp)
fsd fs8, 176(sp)
fsd fs9, 184(sp)
fsd fs10, 192(sp)
fsd fs11, 200(sp)
addi s1, a0, SVM_RV_CTX_STATE
SVM_RV_PHASE SVM_NATIVE_PHASE_ENTERING
sd sp, SVM_RV_CTX_REL(SVM_RV_CTX_ROOT_SP)(s1)
ld t0, SVM_RV_CTX_REL(SVM_RV_CTX_FP_ENVIRONMENT)(s1)
beqz t0, 4f
frcsr t0
sd t0, SVM_RV_CTX_REL(SVM_RV_CTX_HOST_FCSR)(s1)
4:
SVM_RV_INSTALL_GUEST_FP
SVM_RV_RELOAD_FLAGS
SVM_RV_RESOLVE_AND_ENTER
.size SvmRunNative, .-SvmRunNative

.p2align 2
.global SvmNativeDispatch
.type SvmNativeDispatch, @function
SvmNativeDispatch:
SVM_RV_DISPATCH SvmNativeDispatch, 0
.size SvmNativeDispatch, .-SvmNativeDispatch

.p2align 2
.global SvmNativeDispatchCounted
.type SvmNativeDispatchCounted, @function
SvmNativeDispatchCounted:
SVM_RV_DISPATCH SvmNativeDispatchCounted, 1
.size SvmNativeDispatchCounted, .-SvmNativeDispatchCounted

.p2align 2
.global SvmNativePendingCounted
.type SvmNativePendingCounted, @function
SvmNativePendingCounted:
ld t0, SVM_RV_CTX_REL(SVM_RV_CTX_HITS)(s1)
addi t0, t0, -1
sd t0, SVM_RV_CTX_REL(SVM_RV_CTX_HITS)(s1)
j SvmNativePending
.size SvmNativePendingCounted, .-SvmNativePendingCounted

.p2align 2
.global SvmNativePending
.type SvmNativePending, @function
SvmNativePending:
ld t0, SVM_RV_CTX_REL(SVM_RV_CTX_PC_OFFSET)(s1)
add t0, s1, t0
sd a0, 0(t0)
SVM_RV_RESOLVE_AND_ENTER
.size SvmNativePending, .-SvmNativePending

// These public names remain available to generic clients; RV has no NZCV.
.global SvmNativeDispatchNZCV
.set SvmNativeDispatchNZCV, SvmNativeDispatch
.global SvmNativeDispatchNZCVCounted
.set SvmNativeDispatchNZCVCounted, SvmNativeDispatchCounted

.p2align 2
.global SvmNativeExit
.type SvmNativeExit, @function
SvmNativeExit:
SVM_RV_PHASE SVM_NATIVE_PHASE_LEAVING
SVM_RV_PUBLISH_FLAGS
SVM_RV_CLEAR_ACTIVE
SVM_RV_CAPTURE_GUEST_FP
j .Lsvm_restore_host_root
.size SvmNativeExit, .-SvmNativeExit
.p2align 2
.global SvmNativeEventExit
.type SvmNativeEventExit, @function
SvmNativeEventExit:
addi s1, a0, SVM_RV_CTX_STATE
SVM_RV_PHASE SVM_NATIVE_PHASE_LEAVING
li a0, SVM_NATIVE_EXIT_GUEST_EVENTS
j .Lsvm_restore_host_root
.size SvmNativeEventExit, .-SvmNativeEventExit
.p2align 2
.global SvmNativeExceptionExit
.type SvmNativeExceptionExit, @function
SvmNativeExceptionExit:
addi s1, a0, SVM_RV_CTX_STATE
SVM_RV_PHASE SVM_NATIVE_PHASE_LEAVING
li a0, SVM_NATIVE_EXIT_GUEST_EXCEPTION
.Lsvm_restore_host_root:
ld sp, SVM_RV_CTX_REL(SVM_RV_CTX_ROOT_SP)(s1)
SVM_RV_CLEAR_ACTIVE
SVM_RV_RESTORE_HOST_FP_ONLY
sd zero, SVM_RV_CTX_REL(SVM_RV_CTX_ROOT_SP)(s1)
sd zero, SVM_RV_CTX_REL(SVM_RV_CTX_PHASE)(s1)
ld ra, 0(sp)
ld s0, 8(sp)
ld s1, 16(sp)
ld s2, 24(sp)
ld s3, 32(sp)
ld s4, 40(sp)
ld s5, 48(sp)
ld s6, 56(sp)
ld s7, 64(sp)
ld s8, 72(sp)
ld s9, 80(sp)
ld s10, 88(sp)
ld s11, 96(sp)
fld fs0, 112(sp)
fld fs1, 120(sp)
fld fs2, 128(sp)
fld fs3, 136(sp)
fld fs4, 144(sp)
fld fs5, 152(sp)
fld fs6, 160(sp)
fld fs7, 168(sp)
fld fs8, 176(sp)
fld fs9, 184(sp)
fld fs10, 192(sp)
fld fs11, 200(sp)
addi sp, sp, SVM_RV_ROOT_FRAME_BYTES
ret
.size SvmNativeExceptionExit, .-SvmNativeExceptionExit

.option pop
.section .note.GNU-stack,"",@progbits
#endif
