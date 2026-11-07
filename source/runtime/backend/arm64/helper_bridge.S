// Exact private helper bridges. The ISA save sets remain unchanged.
// ELF and Mach-O differ only in symbol spelling/metadata here.
#if defined(__aarch64__)
#if defined(__APPLE__)
#define SVM_SYMBOL(name) _##name
#else
#define SVM_SYMBOL(name) name
#endif
.text
.macro SVM_FUNCTION name
.p2align 2
.globl \name
#if !defined(__APPLE__)
.hidden \name
.type \name, %function
#endif
\name:
.endm
.macro SVM_END_FUNCTION name
#if !defined(__APPLE__)
.size \name, .-\name
#endif
.endm

.macro SVM_HELPER_SAVE_BANK frame=576
sub sp,sp,#\frame
stp x1,x2,[sp,#0]
stp x3,x4,[sp,#16]
stp x5,x6,[sp,#32]
stp x7,x8,[sp,#48]
stp x9,x10,[sp,#64]
stp x11,x12,[sp,#80]
stp x13,x14,[sp,#96]
stp x15,x30,[sp,#112]
stp q2,q3,[sp,#128]
stp q4,q5,[sp,#160]
stp q6,q7,[sp,#192]
stp q16,q17,[sp,#224]
stp q18,q19,[sp,#256]
stp q20,q21,[sp,#288]
stp q22,q23,[sp,#320]
stp q24,q25,[sp,#352]
stp q26,q27,[sp,#384]
stp q28,q29,[sp,#416]
stp q30,q31,[sp,#448]
stp q10,q11,[sp,#480]
stp q12,q13,[sp,#512]
stp q14,q15,[sp,#544]
.endm

.macro SVM_HELPER_RESTORE_BANK frame=576
ldp q2,q3,[sp,#128]
ldp q4,q5,[sp,#160]
ldp q6,q7,[sp,#192]
ldp q16,q17,[sp,#224]
ldp q18,q19,[sp,#256]
ldp q20,q21,[sp,#288]
ldp q22,q23,[sp,#320]
ldp q24,q25,[sp,#352]
ldp q26,q27,[sp,#384]
ldp q28,q29,[sp,#416]
ldp q30,q31,[sp,#448]
ldp q10,q11,[sp,#480]
ldp q12,q13,[sp,#512]
ldp q14,q15,[sp,#544]
ldp x1,x2,[sp,#0]
ldp x3,x4,[sp,#16]
ldp x5,x6,[sp,#32]
ldp x7,x8,[sp,#48]
ldp x9,x10,[sp,#64]
ldp x11,x12,[sp,#80]
ldp x13,x14,[sp,#96]
ldp x15,x30,[sp,#112]
add sp,sp,#\frame
ret
.endm

// x0=high,x16=low,x17=divisor -> x0=quotient,x16=remainder,x17=valid.
// The extra 16B is the real numerical status out-slot, not another save bank.
.macro SVM_DIV_STUB name, config
SVM_FUNCTION \name
SVM_HELPER_SAVE_BANK 592
mov x1, x16
mov x2, x17
mov x3, #\config
add x4, sp, #576
bl SVM_SYMBOL(SvmArm64WideDivide)
mov x16, x1
ldr x17, [sp, #576]
SVM_HELPER_RESTORE_BANK 592
SVM_END_FUNCTION \name
.endm
SVM_DIV_STUB SVM_SYMBOL(SvmArm64Div8U), 8
SVM_DIV_STUB SVM_SYMBOL(SvmArm64Div16U), 16
SVM_DIV_STUB SVM_SYMBOL(SvmArm64Div32U), 32
SVM_DIV_STUB SVM_SYMBOL(SvmArm64Div64U), 64
SVM_DIV_STUB SVM_SYMBOL(SvmArm64Div8S), 264
SVM_DIV_STUB SVM_SYMBOL(SvmArm64Div16S), 272
SVM_DIV_STUB SVM_SYMBOL(SvmArm64Div32S), 288
SVM_DIV_STUB SVM_SYMBOL(SvmArm64Div64S), 320

SVM_FUNCTION SVM_SYMBOL(SvmArm64QueryCpuid)
SVM_HELPER_SAVE_BANK
mov x1, x16
mov x16, x17
blr x16
mov x16, x1
SVM_HELPER_RESTORE_BANK
SVM_END_FUNCTION SVM_SYMBOL(SvmArm64QueryCpuid)

// Fixed integer helper ABI: x1=callback, x0/x16/x17=three arguments.
// The caller stages its original packed-flags register before borrowing x1.
SVM_FUNCTION SVM_SYMBOL(SvmArm64CallLambda)
SVM_HELPER_SAVE_BANK
mov x30, x1
mov x1, x16
mov x2, x17
blr x30
SVM_HELPER_RESTORE_BANK
SVM_END_FUNCTION SVM_SYMBOL(SvmArm64CallLambda)

SVM_FUNCTION SVM_SYMBOL(SvmArm64ExecuteX87)
#if SVM_X87_GPR_ONLY
sub sp, sp, #144
stp x1,x2,[sp,#0]
stp x3,x4,[sp,#16]
stp x5,x6,[sp,#32]
stp x7,x8,[sp,#48]
stp x9,x10,[sp,#64]
stp x11,x12,[sp,#80]
stp x13,x14,[sp,#96]
stp x15,x30,[sp,#112]
mrs x3,nzcv
str x3,[sp,#128]
#else
SVM_HELPER_SAVE_BANK
#endif
mov x1, x16
mov x2, x17
bl SVM_SYMBOL(SvmX87Execute)
#if SVM_X87_GPR_ONLY
ldr x3,[sp,#128]
msr nzcv,x3
ldp x1,x2,[sp,#0]
ldp x3,x4,[sp,#16]
ldp x5,x6,[sp,#32]
ldp x7,x8,[sp,#48]
ldp x9,x10,[sp,#64]
ldp x11,x12,[sp,#80]
ldp x13,x14,[sp,#96]
ldp x15,x30,[sp,#112]
add sp,sp,#144
ret
#else
SVM_HELPER_RESTORE_BANK
#endif
SVM_END_FUNCTION SVM_SYMBOL(SvmArm64ExecuteX87)

SVM_FUNCTION SVM_SYMBOL(SvmArm64VectorHelper)
sub sp,sp,#592
stp x1,x2,[sp,#0]
stp x3,x4,[sp,#16]
stp x5,x6,[sp,#32]
stp x7,x8,[sp,#48]
stp x9,x10,[sp,#64]
stp x11,x12,[sp,#80]
stp x13,x14,[sp,#96]
stp x15,x30,[sp,#112]
stp q2,q3,[sp,#128]
stp q4,q5,[sp,#160]
stp q6,q7,[sp,#192]
stp q16,q17,[sp,#224]
stp q18,q19,[sp,#256]
stp q20,q21,[sp,#288]
stp q22,q23,[sp,#320]
stp q24,q25,[sp,#352]
stp q26,q27,[sp,#384]
stp q28,q29,[sp,#416]
stp q30,q31,[sp,#448]
stp q10,q11,[sp,#480]
stp q12,q13,[sp,#512]
stp q14,q15,[sp,#544]
mrs x3,nzcv
str x3,[sp,#576]
mov x30,x0
mov x0,x16
mov x1,x17
blr x30
fmov d0,x0
ins v0.d[1],x1
ldr x3,[sp,#576]
msr nzcv,x3
ldp q2,q3,[sp,#128]
ldp q4,q5,[sp,#160]
ldp q6,q7,[sp,#192]
ldp q16,q17,[sp,#224]
ldp q18,q19,[sp,#256]
ldp q20,q21,[sp,#288]
ldp q22,q23,[sp,#320]
ldp q24,q25,[sp,#352]
ldp q26,q27,[sp,#384]
ldp q28,q29,[sp,#416]
ldp q30,q31,[sp,#448]
ldp q10,q11,[sp,#480]
ldp q12,q13,[sp,#512]
ldp q14,q15,[sp,#544]
ldp x1,x2,[sp,#0]
ldp x3,x4,[sp,#16]
ldp x5,x6,[sp,#32]
ldp x7,x8,[sp,#48]
ldp x9,x10,[sp,#64]
ldp x11,x12,[sp,#80]
ldp x13,x14,[sp,#96]
ldp x15,x30,[sp,#112]
add sp,sp,#592
ret
SVM_END_FUNCTION SVM_SYMBOL(SvmArm64VectorHelper)

#if !defined(__APPLE__)
.section .note.GNU-stack,"",%progbits
#endif
#endif
