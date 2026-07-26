.global _start
.text
_start:
    sub $128, %rsp
    mov $1, %r12d
    movabs $0x0123456789abcdef, %r8

    movbe %r8, (%rsp)
    movbe (%rsp), %r9
    cmp %r8, %r9
    jne 1f

    mov $0x1122, %r8d
    movbe %r8w, 16(%rsp)
    cmpw $0x2211, 16(%rsp)
    jne 1f
    xor %r9d, %r9d
    movbe 16(%rsp), %r9w
    cmp $0x1122, %r9d
    jne 1f

    mov $0x11223344, %r8d
    movbe %r8d, 20(%rsp)
    cmpl $0x44332211, 20(%rsp)
    jne 1f
    movbe 20(%rsp), %r9d
    cmp $0x11223344, %r9d
    jne 1f

    movnti %r8, 8(%rsp)
    cmp %r8, 8(%rsp)
    jne 1f

    mov %rsp, %rbx
    movb $0x7b, 21(%rbx)
    mov $21, %al
    xlat
    cmp $0x7b, %al
    jne 1f

    mov $2, %r12d
    mov $1, %rax
    mov $2, %rdx
    shld $1, %rdx, %rax
    cmp $2, %rax
    jne 1f
    mov $3, %r12d
    mov $2, %rax
    shrd $1, %rdx, %rax
    cmp $1, %rax
    jne 1f

    mov $4, %r12d
    clc
    mov $1, %rax
    rcl $1, %rax
    cmp $2, %rax
    jne 1f
    mov $5, %r12d
    stc
    mov $3, %rax
    rcr $1, %rax
    jnc 1f

    mov $6, %r12d
    movb $0x5a, 24(%rsp)
    lea 24(%rsp), %rsi
    lea 28(%rsp), %rdi
    std
    mov $7, %r12d
    movsb
    cmpb $0x5a, 28(%rsp)
    jne 1f
    mov $8, %r12d
    lea 23(%rsp), %r10
    cmp %r10, %rsi
    jne 1f
    mov $9, %r12d
    lea 27(%rsp), %r10
    cmp %r10, %rdi
    jne 1f
    mov $10, %r12d
    mov %rsp, %r11
    pushfq
    pop %r10
    cmp %r11, %rsp
    jne 1f
    mov $11, %r12d
    test $0x400, %r10
    jz 1f
    push $0x202
    popfq

    mov $12, %r12d
    movabs $0x3ff0000000000000, %rax
    movabs $0x4000000000000000, %rdx
    movq %rax, %xmm0
    movq %rdx, %xmm1
    addsd %xmm1, %xmm0
    movq %xmm0, %r10
    movabs $0x4008000000000000, %r11
    cmp %r11, %r10
    jne 1f

    movq %rax, %xmm0
    cmpltsd %xmm1, %xmm0
    movq %xmm0, %r10
    cmp $-1, %r10
    jne 1f

    mov $13, %r12d
    movabs $0xffffffffffffffff, %rax
    movq %rax, %xmm0
    punpcklqdq %xmm0, %xmm0
    movabs $0x0002000200020002, %rax
    movq %rax, %xmm1
    punpcklqdq %xmm1, %xmm1
    pmulhuw %xmm1, %xmm0
    movq %xmm0, %r10
    movabs $0x0001000100010001, %r11
    cmp %r11, %r10
    jne 1f

    mov $14, %r12d
    movabs $0x8000800080008000, %rax
    movq %rax, %xmm0
    punpcklqdq %xmm0, %xmm0
    pmulhw %xmm1, %xmm0
    movq %xmm0, %r10
    cmp $-1, %r10
    jne 1f

    mov $15, %r12d
    movabs $0xaaaaaaaaaaaaaaaa, %rax
    movq %rax, %xmm0
    punpcklqdq %xmm0, %xmm0
    pxor %xmm1, %xmm1
    mov $-4096, %rdi
    maskmovdqu %xmm1, %xmm0
    pcmpeqb %xmm1, %xmm1
    lea 64(%rsp), %rdi
    maskmovdqu %xmm1, %xmm0
    cmp %rax, 64(%rsp)
    jne 1f
    cmp %rax, 72(%rsp)
    jne 1f

    mov $16, %r12d
    movabs $0xfffffffe00000001, %rax
    movq %rax, %xmm0
    movabs $0xfffffffc00000003, %rdx
    movq %rdx, %xmm1
    punpcklqdq %xmm1, %xmm0
    cvtdq2ps %xmm0, %xmm0
    movdqu %xmm0, 80(%rsp)
    cmpl $0x3f800000, 80(%rsp)
    jne 1f
    mov $21, %r12d
    cmpl $0xc0000000, 84(%rsp)
    jne 1f
    mov $22, %r12d
    cmpl $0x40400000, 88(%rsp)
    jne 1f
    mov $23, %r12d
    cmpl $0xc0800000, 92(%rsp)
    jne 1f

    mov $17, %r12d
    movabs $0x3ff8000000000000, %rax
    movq %rax, %xmm0
    movabs $0xc000000000000000, %rax
    movq %rax, %xmm1
    punpcklqdq %xmm1, %xmm0
    cvtpd2ps %xmm0, %xmm0
    movq %xmm0, %r10
    movabs $0xc00000003fc00000, %r11
    cmp %r11, %r10
    jne 1f

    mov $18, %r12d
    movabs $0xc00000003fc00000, %rax
    movq %rax, %xmm0
    cvtps2pd %xmm0, %xmm0
    movdqu %xmm0, 96(%rsp)
    movabs $0x3ff8000000000000, %r10
    cmp %r10, 96(%rsp)
    jne 1f
    movabs $0xc000000000000000, %r10
    cmp %r10, 104(%rsp)
    jne 1f

    mov $19, %r12d
    cvtpd2dq %xmm0, %xmm0
    movq %xmm0, %r10
    movabs $0xfffffffe00000002, %r11
    cmp %r11, %r10
    jne 1f

    mov $20, %r12d
    mov $0x3fcccccd, %eax
    movd %eax, %xmm0
    cvtss2si %xmm0, %r10d
    cmp $2, %r10d
    jne 1f
    movabs $0xbff999999999999a, %rax
    movq %rax, %xmm0
    cvtsd2si %xmm0, %r10
    cmp $-2, %r10
    jne 1f

    mov $24, %r12d
    mov $1, %eax
    xor %ecx, %ecx
    cpuid
    mov %edx, %r10d
    and $0x07008110, %r10d
    cmp $0x07008110, %r10d
    jne 1f
    test $0x00800000, %edx
    jnz 1f
    mov %ecx, %r10d
    and $0x40400000, %r10d
    cmp $0x40400000, %r10d
    jne 1f
    test $0x0c002000, %ecx
    jnz 1f
    mov $7, %eax
    xor %ecx, %ecx
    cpuid
    test $0x00040000, %ebx
    jz 1f
    mov $0x80000001, %eax
    xor %ecx, %ecx
    cpuid
    test $0x08000000, %edx
    jz 1f
    mov $0x15, %eax
    xor %ecx, %ecx
    cpuid
    cmp $1, %eax
    jne 1f
    cmp $1, %ebx
    jne 1f
    cmp $1000000000, %ecx
    jne 1f

    mov $25, %r12d
    movabs $0x4f0000007fc00000, %rax
    movq %rax, %xmm0
    punpcklqdq %xmm0, %xmm0
    cvttps2dq %xmm0, %xmm0
    movq %xmm0, %r10
    movabs $0x8000000080000000, %r11
    cmp %r11, %r10
    jne 1f

    mov $26, %r12d
    movabs $0x4110000040800000, %rax
    movq %rax, %xmm0
    punpcklqdq %xmm0, %xmm0
    sqrtps %xmm0, %xmm0
    movq %xmm0, %r10
    movabs $0x4040000040000000, %r11
    cmp %r11, %r10
    jne 1f

    xor %edi, %edi
    mov $60, %eax
    syscall
1:
    mov %r12d, %edi
    mov $60, %eax
    syscall
