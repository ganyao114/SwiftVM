// Switches to guest GPR/XMM/YMM/RFLAGS state, jumps to one guest instruction,
// and returns through the absolute trampoline installed at terminal_pc.
#if defined(__APPLE__)
#define SYM(name) _##name
#else
#define SYM(name) name
#endif

.data
.align 8
SYM(host_rsp):      .quad 0
SYM(host_mxcsr):    .long 0
.align 8
SYM(ctx_ptr):       .quad 0
SYM(entry_ptr):     .quad 0
SYM(guest_rsp):     .quad 0
SYM(guest_rdi):     .quad 0
SYM(saved_r11):     .quad 0

.text
.globl SYM(svm_native_run)
SYM(svm_native_run):
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, SYM(host_rsp)(%rip)
    movq %rdi, SYM(ctx_ptr)(%rip)
    movq %rsi, SYM(entry_ptr)(%rip)
    stmxcsr SYM(host_mxcsr)(%rip)

    pushq 128(%rdi)
    popfq
    vldmxcsr 136(%rdi)
    vmovdqu 144(%rdi), %ymm0
    vmovdqu 176(%rdi), %ymm1
    vmovdqu 208(%rdi), %ymm2
    vmovdqu 240(%rdi), %ymm3
    vmovdqu 272(%rdi), %ymm4
    vmovdqu 304(%rdi), %ymm5
    vmovdqu 336(%rdi), %ymm6
    vmovdqu 368(%rdi), %ymm7
    vmovdqu 400(%rdi), %ymm8
    vmovdqu 432(%rdi), %ymm9
    vmovdqu 464(%rdi), %ymm10
    vmovdqu 496(%rdi), %ymm11
    vmovdqu 528(%rdi), %ymm12
    vmovdqu 560(%rdi), %ymm13
    vmovdqu 592(%rdi), %ymm14
    vmovdqu 624(%rdi), %ymm15

    movq 32(%rdi), %rax
    movq %rax, SYM(guest_rsp)(%rip)
    movq 56(%rdi), %rax
    movq %rax, SYM(guest_rdi)(%rip)
    movq 8(%rdi), %rcx
    movq 16(%rdi), %rdx
    movq 24(%rdi), %rbx
    movq 40(%rdi), %rbp
    movq 48(%rdi), %rsi
    movq 64(%rdi), %r8
    movq 72(%rdi), %r9
    movq 80(%rdi), %r10
    movq 88(%rdi), %r11
    movq 96(%rdi), %r12
    movq 104(%rdi), %r13
    movq 112(%rdi), %r14
    movq 120(%rdi), %r15
    movq 0(%rdi), %rax
    movq SYM(guest_rdi)(%rip), %rdi
    movq SYM(guest_rsp)(%rip), %rsp
    jmp *SYM(entry_ptr)(%rip)

.globl SYM(svm_native_resume)
SYM(svm_native_resume):
    movq %r11, SYM(saved_r11)(%rip)
    movq SYM(ctx_ptr)(%rip), %r11
    vmovdqu %ymm0, 144(%r11)
    vmovdqu %ymm1, 176(%r11)
    vmovdqu %ymm2, 208(%r11)
    vmovdqu %ymm3, 240(%r11)
    vmovdqu %ymm4, 272(%r11)
    vmovdqu %ymm5, 304(%r11)
    vmovdqu %ymm6, 336(%r11)
    vmovdqu %ymm7, 368(%r11)
    vmovdqu %ymm8, 400(%r11)
    vmovdqu %ymm9, 432(%r11)
    vmovdqu %ymm10, 464(%r11)
    vmovdqu %ymm11, 496(%r11)
    vmovdqu %ymm12, 528(%r11)
    vmovdqu %ymm13, 560(%r11)
    vmovdqu %ymm14, 592(%r11)
    vmovdqu %ymm15, 624(%r11)
    vstmxcsr 136(%r11)
    movq %rax, 0(%r11)
    movq %rcx, 8(%r11)
    movq %rdx, 16(%r11)
    movq %rbx, 24(%r11)
    movq %rsp, 32(%r11)
    movq %rbp, 40(%r11)
    movq %rsi, 48(%r11)
    movq %rdi, 56(%r11)
    movq %r8, 64(%r11)
    movq %r9, 72(%r11)
    movq %r10, 80(%r11)
    movq %r12, 96(%r11)
    movq %r13, 104(%r11)
    movq %r14, 112(%r11)
    movq %r15, 120(%r11)
    movq SYM(saved_r11)(%rip), %rax
    movq %rax, 88(%r11)
    movq SYM(host_rsp)(%rip), %rsp
    pushfq
    popq %rax
    movq %rax, 128(%r11)
    vldmxcsr SYM(host_mxcsr)(%rip)
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
