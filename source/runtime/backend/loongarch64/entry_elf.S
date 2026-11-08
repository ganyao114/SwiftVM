// Linked LA64D ABI: r22=Uniform base, r31=packed flags. Only this root
// saves host callee-saved state; native units own only their spill frames.
#include "runtime/backend/loongarch64/abi_layout.inc"
#include "runtime/backend/dispatch_layout.inc"
#include "runtime/exit.inc"
#if defined(__loongarch64) || (defined(__loongarch__) && __loongarch_grlen == 64)
.text

.macro SVM_LA_PUBLISH_FLAGS
ld.d $t0, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_FLAGS_OFFSET)
li.d $t1, 0xffffffff
beq $t0, $t1, 90f
add.d $t0, $r22, $t0
st.d $r31, $t0, 0
90:
.endm

.macro SVM_LA_RELOAD_FLAGS
ld.d $t0, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_FLAGS_OFFSET)
li.d $t1, 0xffffffff
beq $t0, $t1, 91f
add.d $t0, $r22, $t0
ld.d $r31, $t0, 0
b 92f
91:
or $r31, $zero, $zero
92:
.endm

.macro SVM_LA_RESTORE_HOST_FP
ld.d $t0, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_FP_ENVIRONMENT)
beqz $t0, 93f
movfcsr2gr $t0, $fcsr0
li.d $t1, 0x1f0000
and $t0, $t0, $t1
st.d $t0, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_GUEST_FPSR)
ld.d $t0, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_HOST_FCSR)
movgr2fcsr $fcsr0, $t0
93:
.endm

.macro SVM_LA_INSTALL_GUEST_FP
ld.d $t0, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_FP_ENVIRONMENT)
beqz $t0, 94f
ld.d $t0, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_GUEST_FCSR)
ld.d $t1, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_GUEST_FPSR)
or $t0, $t0, $t1
movgr2fcsr $fcsr0, $t0
94:
.endm

.macro SVM_LA_CLEAR_ACTIVE
ld.d $t0, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_ACTIVE)
dbar 0
st.d $zero, $t0, 0
.endm

.macro SVM_LA_RESOLVE_AND_ENTER
SVM_LA_PUBLISH_FLAGS
SVM_LA_CLEAR_ACTIVE
SVM_LA_RESTORE_HOST_FP
ld.d $a0, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_OPAQUE)
addi.d $a1, $r22, -SVM_LA_CTX_STATE
ld.d $t0, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_RESOLVE)
jirl $ra, $t0, 0
SVM_LA_INSTALL_GUEST_FP
beqz $a0, 95f
SVM_LA_RELOAD_FLAGS
ld.d $t0, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_ACTIVE)
ld.d $t1, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_ANCHOR)
dbar 0
st.d $t1, $t0, 0
jirl $zero, $a0, 0
95:
li.d $a0, SVM_NATIVE_EXIT_RESOLVE_FAILURE
b SvmNativeExit
.endm

.macro SVM_LA_COUNT field
ld.d $t4, $r22, SVM_LA_CTX_REL(\field)
addi.d $t4, $t4, 1
st.d $t4, $r22, SVM_LA_CTX_REL(\field)
.endm

.macro SVM_LA_DISPATCH counted=0
.if \counted
SVM_LA_COUNT SVM_LA_CTX_DISPATCHES
.endif
ld.d $t0, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_SLOTS)
ld.d $t1, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_MASK)
beqz $t0, 3f
srli.d $t2, $a0, SVM_DISPATCH_HASH_SHIFT
xor $t2, $t2, $a0
and $t2, $t2, $t1
1:
.if \counted
SVM_LA_COUNT SVM_LA_CTX_PROBES
.endif
slli.d $t3, $t2, SVM_DISPATCH_SLOT_SHIFT
add.d $t3, $t3, $t0
ld.d $t5, $t3, 0
bne $t5, $a0, 2f
ld.d $t5, $t3, 8
dbar 0
beqz $t5, 3f
.if \counted
SVM_LA_COUNT SVM_LA_CTX_HITS
.endif
jirl $zero, $t5, 0
2:
ld.bu $t5, $t3, SVM_DISPATCH_OCCUPIED_OFFSET
beqz $t5, 3f
addi.d $t2, $t2, 1
and $t2, $t2, $t1
b 1b
3:
ld.d $t0, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_PC_OFFSET)
add.d $t0, $r22, $t0
st.d $a0, $t0, 0
SVM_LA_RESOLVE_AND_ENTER
.endm

.p2align 2
.global SvmRunNative
.type SvmRunNative, @function
SvmRunNative:
addi.d $sp, $sp, -SVM_LA_ROOT_FRAME_BYTES
st.d $ra, $sp, 0
st.d $r22, $sp, 8
st.d $r23, $sp, 16
st.d $r24, $sp, 24
st.d $r25, $sp, 32
st.d $r26, $sp, 40
st.d $r27, $sp, 48
st.d $r28, $sp, 56
st.d $r29, $sp, 64
st.d $r30, $sp, 72
st.d $r31, $sp, 80
fst.d $f24, $sp, 96
fst.d $f25, $sp, 104
fst.d $f26, $sp, 112
fst.d $f27, $sp, 120
fst.d $f28, $sp, 128
fst.d $f29, $sp, 136
fst.d $f30, $sp, 144
fst.d $f31, $sp, 152
addi.d $r22, $a0, SVM_LA_CTX_STATE
ld.d $t0, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_FP_ENVIRONMENT)
beqz $t0, 4f
movfcsr2gr $t0, $fcsr0
st.d $t0, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_HOST_FCSR)
4:
SVM_LA_INSTALL_GUEST_FP
SVM_LA_RELOAD_FLAGS
SVM_LA_RESOLVE_AND_ENTER
.size SvmRunNative, .-SvmRunNative

.p2align 2
.global SvmNativeDispatch
.type SvmNativeDispatch, @function
SvmNativeDispatch:
SVM_LA_DISPATCH 0
.size SvmNativeDispatch, .-SvmNativeDispatch

.p2align 2
.global SvmNativeDispatchCounted
.type SvmNativeDispatchCounted, @function
SvmNativeDispatchCounted:
SVM_LA_DISPATCH 1
.size SvmNativeDispatchCounted, .-SvmNativeDispatchCounted

.p2align 2
.global SvmNativePendingCounted
.type SvmNativePendingCounted, @function
SvmNativePendingCounted:
ld.d $t0, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_HITS)
addi.d $t0, $t0, -1
st.d $t0, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_HITS)
b SvmNativePending
.size SvmNativePendingCounted, .-SvmNativePendingCounted

.p2align 2
.global SvmNativePending
.type SvmNativePending, @function
SvmNativePending:
ld.d $t0, $r22, SVM_LA_CTX_REL(SVM_LA_CTX_PC_OFFSET)
add.d $t0, $r22, $t0
st.d $a0, $t0, 0
SVM_LA_RESOLVE_AND_ENTER
.size SvmNativePending, .-SvmNativePending

.global SvmNativeDispatchNZCV
.set SvmNativeDispatchNZCV, SvmNativeDispatch
.global SvmNativeDispatchNZCVCounted
.set SvmNativeDispatchNZCVCounted, SvmNativeDispatchCounted

.p2align 2
.global SvmNativeExit
.type SvmNativeExit, @function
SvmNativeExit:
SVM_LA_PUBLISH_FLAGS
SVM_LA_CLEAR_ACTIVE
SVM_LA_RESTORE_HOST_FP
ld.d $ra, $sp, 0
ld.d $r22, $sp, 8
ld.d $r23, $sp, 16
ld.d $r24, $sp, 24
ld.d $r25, $sp, 32
ld.d $r26, $sp, 40
ld.d $r27, $sp, 48
ld.d $r28, $sp, 56
ld.d $r29, $sp, 64
ld.d $r30, $sp, 72
ld.d $r31, $sp, 80
fld.d $f24, $sp, 96
fld.d $f25, $sp, 104
fld.d $f26, $sp, 112
fld.d $f27, $sp, 120
fld.d $f28, $sp, 128
fld.d $f29, $sp, 136
fld.d $f30, $sp, 144
fld.d $f31, $sp, 152
addi.d $sp, $sp, SVM_LA_ROOT_FRAME_BYTES
jirl $zero, $ra, 0
.size SvmNativeExit, .-SvmNativeExit
.section .note.GNU-stack,"",@progbits
#endif
