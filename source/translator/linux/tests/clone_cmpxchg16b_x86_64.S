// Libc-free four-thread CMPXCHG16B stress. Each successful CAS increments the
// low counter and writes its bitwise complement to the high tag, so a torn
// update or lost atomic increment is observable. Also covers CPUID.CX16 and
// the legal unaligned no-LOCK form.

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
    .balign 16
counter_tag:
    .zero 16
    .balign 16
unaligned_storage:
    .zero 32

    .section .text
    .global _start
_start:
    movq    $-1, counter_tag+8(%rip)

    // SwiftVM may advertise CX16 only when the instruction is implemented.
    mov     $1, %eax
    cpuid
    bt      $13, %ecx
    jnc     cpuid_fail

    // The no-LOCK spelling is legal even when the m128 operand is unaligned.
    // Check both success and mismatch result plumbing before starting threads.
    lea     unaligned_storage+1(%rip), %rdi
    xor     %eax, %eax
    xor     %edx, %edx
    movabs  $0x1122334455667788, %rbx
    movabs  $0x8877665544332211, %rcx
    cmpxchg16b (%rdi)
    jnz     nolock_success_fail

    xor     %eax, %eax
    xor     %edx, %edx
    movabs  $0x0102030405060708, %rbx
    movabs  $0x8070605040302010, %rcx
    cmpxchg16b (%rdi)
    jz      nolock_mismatch_fail
    movabs  $0x1122334455667788, %r8
    cmp     %r8, %rax
    jne     nolock_result_fail
    movabs  $0x8877665544332211, %r8
    cmp     %r8, %rdx
    jne     nolock_result_fail

    xor     %r12d, %r12d
spawn:
    // CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM|
    // PARENT_SETTID|CHILD_CLEARTID
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
    js      clone_fail
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
    mov     $128, %esi              // FUTEX_WAIT_PRIVATE
    xor     %r10d, %r10d
    xor     %r8d, %r8d
    xor     %r9d, %r9d
    mov     $202, %eax
    syscall
    jmp     wait_child

child:
    // The initial expected pair is the statically initialized {0,~0}. On a
    // failed CAS, CMPXCHG16B supplies a coherent retry value in RDX:RAX.
    xor     %eax, %eax
    mov     $-1, %rdx
    mov     $ITERS, %r14d
cas_retry:
    lea     1(%rax), %rbx
    mov     %rbx, %rcx
    not     %rcx
    lock cmpxchg16b counter_tag(%rip)
    jnz     cas_retry
    // Success leaves RDX:RAX unchanged, so carry the desired pair forward as
    // this thread's optimistic expected value for its next iteration.
    mov     %rbx, %rax
    mov     %rcx, %rdx
    dec     %r14d
    jne     cas_retry
    xor     %edi, %edi
    mov     $60, %eax               // exit current thread
    syscall

next_child:
    inc     %r12d
    cmp     $THREADS, %r12d
    jne     wait_child

    mov     $10, %edi
    cmpq    $TOTAL, counter_tag(%rip)
    jne     finish
    mov     counter_tag(%rip), %rax
    not     %rax
    mov     $11, %edi
    cmp     %rax, counter_tag+8(%rip)
    jne     finish
    xor     %edi, %edi
    jmp     finish

cpuid_fail:
    mov     $2, %edi
    jmp     finish
nolock_success_fail:
    mov     $3, %edi
    jmp     finish
nolock_mismatch_fail:
    mov     $4, %edi
    jmp     finish
nolock_result_fail:
    mov     $5, %edi
    jmp     finish
clone_fail:
    mov     $1, %edi
finish:
    mov     $231, %eax              // exit_group
    syscall
