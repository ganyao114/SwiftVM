.text
.global _start
_start:
    xor %ebx,%ebx
    lea multiply_expected(%rip),%r13
restart:
    lea modes(%rip),%r12
    mov $5,%ecx
next_mode:
    ldmxcsr (%r12)
    call fp_site
    movq %xmm0,%rax
    cmp (%r13),%rax
    jne failure
    movhlps %xmm0,%xmm1
    movq %xmm1,%rax
    cmp 8(%r13),%rax
    jne failure
    add $4,%r12
    add $16,%r13
    dec %ecx
    jne next_mode
    test %ebx,%ebx
    jne success
    inc %ebx
    # All four actual mode directories must retire this old instruction.
    movb $0x58,fp_op+2(%rip)
    lea addition_expected(%rip),%r13
    jmp restart
success:
    xor %edi,%edi
    mov $231,%eax
    syscall
failure:
    mov $1,%edi
    mov $231,%eax
    syscall
fp_site:
    movapd inputs(%rip),%xmm0
fp_op:
    mulpd factors(%rip),%xmm0
    ret
.balign 16
inputs:
    .quad 0x0000000000000001,0x0010000000000000
factors:
    .quad 0x7fd0000000000000,0x3fe0000000000000
modes:
    .long 0x1f80,0x1fc0,0x9f80,0x9fc0,0x1f80
.balign 16
multiply_expected:
    .quad 0x3cb0000000000000,0x0008000000000000
    .quad 0,0x0008000000000000
    .quad 0x3cb0000000000000,0
    .quad 0,0
    .quad 0x3cb0000000000000,0x0008000000000000
addition_expected:
    .rept 5
    .quad 0x7fd0000000000000,0x3fe0000000000000
    .endr
