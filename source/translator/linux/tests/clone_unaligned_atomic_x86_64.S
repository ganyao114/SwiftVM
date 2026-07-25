// Four-thread split-lock smoke. Both qword counters are deliberately placed
// at address % 8 == 1. AArch64 exclusives would SIGBUS here; SwiftVM must take
// the process-wide serialized plain-RMW fallback and retain exact results.

    .set THREADS, 4
    .set ITERS, 20000
    .set TOTAL, THREADS*ITERS

    .section .bss
    .balign 16
child_stack:
    .skip 262144
    .balign 4
tid_words:
    .zero 16
    .balign 8
xadd_storage:
    .byte 0
unaligned_xadd:
    .quad 0
    .balign 8
cmpxchg_storage:
    .byte 0
unaligned_cmpxchg:
    .quad 0

    .section .text
    .global _start
_start:
    xor     %r12d, %r12d
spawn:
    mov     $0x350f00, %edi
    lea     child_stack+65536(%rip), %rsi
    mov     %r12, %rax
    shl     $16, %rax
    add     %rax, %rsi
    lea     tid_words(%rip), %rdx
    lea     (%rdx,%r12,4), %rdx
    mov     %rdx, %r10
    xor     %r8d, %r8d
    mov     $56, %eax
    syscall
    test    %eax, %eax
    jz      child
    js      fail
    inc     %r12d
    cmp     $THREADS, %r12d
    jne     spawn

    xor     %r12d, %r12d
wait_child:
    lea     tid_words(%rip), %rdi
    lea     (%rdi,%r12,4), %rdi
    mov     (%rdi), %edx
    test    %edx, %edx
    jz      next_child
    mov     $128, %esi
    xor     %r10d, %r10d
    xor     %r8d, %r8d
    xor     %r9d, %r9d
    mov     $202, %eax
    syscall
    jmp     wait_child

child:
    lea     unaligned_xadd(%rip), %rbx
    lea     unaligned_cmpxchg(%rip), %rbp
    mov     $ITERS, %r14d
child_loop:
    mov     $1, %rax
    lock xaddq %rax, (%rbx)

    mov     (%rbp), %rax
cmpxchg_retry:
    lea     1(%rax), %rdx
    lock cmpxchgq %rdx, (%rbp)
    jne     cmpxchg_retry

    dec     %r14d
    jne     child_loop
    xor     %edi, %edi
    mov     $60, %eax
    syscall

next_child:
    inc     %r12d
    cmp     $THREADS, %r12d
    jne     wait_child
    lea     unaligned_xadd(%rip), %rax
    test    $7, %rax
    jz      fail
    lea     unaligned_cmpxchg(%rip), %rax
    test    $7, %rax
    jz      fail
    cmpq    $TOTAL, unaligned_xadd(%rip)
    jne     fail
    cmpq    $TOTAL, unaligned_cmpxchg(%rip)
    jne     fail
    xor     %edi, %edi
    jmp     finish
fail:
    mov     $1, %edi
finish:
    mov     $231, %eax
    syscall
