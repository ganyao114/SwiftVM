// Libc-free four-thread stress for every accepted LOCK RMW family added after
// clone_futex_smoke.  Exit 0 means all exact counters, old-value sums, flags,
// and phased logical/bit-operation invariants matched.

    .set THREADS, 4
    .set SIMPLE_ITERS, 20000
    .set PHASE_ITERS, 2000
    .set TOTAL, THREADS*SIMPLE_ITERS
    .set OLD_SUM, TOTAL*(TOTAL-1)/2

    .section .bss
    .balign 16
child_stack:
    .skip 262144
    .balign 4
tid_words:
    .zero 16
barrier_count:
    .long 0
barrier_generation:
    .long 0
bts_lock:
    .long 0
btc_word:
    .long 0
    .balign 8
inc_counter:
    .quad 0
dec_counter:
    .quad 0
adc_counter:
    .quad 0
sbb_counter:
    .quad 0
neg_word:
    .quad 0
not_word:
    .quad 0
protected_counter:
    .quad 0
mapped_rbx_counter:
    .quad 0
mapped_rbp_counter:
    .quad 0
cmpxchg8b_counter:
    .quad 0
mapped_rbx_sums:
    .zero 32
mapped_rbp_sums:
    .zero 32
or_word:
    .quad 0
and_word:
    .quad 0
xor_word:
    .quad 0
btc_cf_count:
    .quad 0
failures:
    .quad 0

    .section .text
    .global _start
_start:
    movq    $TOTAL, dec_counter(%rip)
    movq    $TOTAL, sbb_counter(%rip)
    movabs  $0x0123456789abcdef, %rax
    mov     %rax, neg_word(%rip)
    mov     %rax, not_word(%rip)
    // Make the total NEG/NOT application count odd. This also catches an
    // optimizer incorrectly deleting a value-returning atomic with no user.
    lock negq neg_word(%rip)
    lock notq not_word(%rip)

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
    mov     $128, %esi              // FUTEX_WAIT_PRIVATE
    xor     %r10d, %r10d
    xor     %r8d, %r8d
    xor     %r9d, %r9d
    mov     $202, %eax
    syscall
    jmp     wait_child

child:
    xor     %r8d, %r8d              // sum of old [rbx] values
    xor     %r9d, %r9d              // sum of old [rbp] values
    lea     mapped_rbx_counter(%rip), %rbx
    lea     mapped_rbp_counter(%rip), %rbp
    mov     $SIMPLE_ITERS, %r14d
simple_loop:
    xor     %eax, %eax
    sub     $1, %eax                // set CF without relying on STC decoding
    lock incq inc_counter(%rip)
    jnc     record_failure          // INC must preserve the set CF
1:
    xor     %eax, %eax              // XOR clears CF
    lock decq dec_counter(%rip)
    jc      record_failure_2        // DEC must preserve the clear CF
2:
    xor     %eax, %eax
    sub     $1, %eax
    lock adcq $0, adc_counter(%rip)
    xor     %eax, %eax
    sub     $1, %eax
    lock sbbq $0, sbb_counter(%rip)
    lock negq neg_word(%rip)
    lock notq not_word(%rip)

acquire_bts:
    lock btsl $0, bts_lock(%rip)
    jc      acquire_bts
    incq    protected_counter(%rip)
    lock btrl $0, bts_lock(%rip)
    jnc     record_failure_3        // BTR must report the old set bit in CF
3:
    mov     $1, %rax
    lock xaddq %rax, (%rbx)
    add     %rax, %r8
    mov     $1, %rax
    lock xaddq %rax, (%rbp)
    add     %rax, %r9

    dec     %r14d
    jne     simple_loop

    lea     mapped_rbx_sums(%rip), %rax
    mov     %r8, (%rax,%r12,8)
    lea     mapped_rbp_sums(%rip), %rax
    mov     %r9, (%rax,%r12,8)

    lea     cmpxchg8b_counter(%rip), %r15
    mov     $SIMPLE_ITERS, %r14d
cmpxchg8b_loop:
    mov     (%r15), %rax
cmpxchg8b_retry:
    mov     %rax, %rdx
    shr     $32, %rdx
    lea     1(%rax), %rbx
    mov     %rbx, %rcx
    shr     $32, %rcx
    lock cmpxchg8b (%r15)
    jne     cmpxchg8b_retry
    dec     %r14d
    jne     cmpxchg8b_loop

    mov     $PHASE_ITERS, %r14d
phase_loop:
    test    %r12d, %r12d
    jne     4f
    movq    $0, or_word(%rip)
4:
    call    barrier
    mov     $1, %rax
    mov     %r12b, %cl
    shl     %cl, %rax
    lock orq %rax, or_word(%rip)
    call    barrier
    test    %r12d, %r12d
    jne     5f
    cmpq    $15, or_word(%rip)
    jne     record_phase_failure_1
5:
    call    barrier

    test    %r12d, %r12d
    jne     6f
    movq    $-1, and_word(%rip)
6:
    call    barrier
    mov     $1, %rax
    mov     %r12b, %cl
    shl     %cl, %rax
    not     %rax
    lock andq %rax, and_word(%rip)
    call    barrier
    test    %r12d, %r12d
    jne     7f
    cmpq    $-16, and_word(%rip)
    jne     record_phase_failure_2
7:
    call    barrier

    test    %r12d, %r12d
    jne     8f
    movq    $0, xor_word(%rip)
    movl    $0, btc_word(%rip)
8:
    call    barrier
    lock xorq $1, xor_word(%rip)
    lea     btc_word(%rip), %rbp
    lock btcl $0, (%rbp)
    lock adcq $0, btc_cf_count(%rip) // consume CF from the old BTC bit
    call    barrier
    test    %r12d, %r12d
    jne     9f
    cmpq    $0, xor_word(%rip)
    jne     record_phase_failure_3
    cmpl    $0, btc_word(%rip)
    jne     record_phase_failure_4
9:
    call    barrier

    dec     %r14d
    jne     phase_loop
    xor     %edi, %edi
    mov     $60, %eax
    syscall

record_failure:
    lock incq failures(%rip)
    jmp     1b
record_failure_2:
    lock incq failures(%rip)
    jmp     2b
record_failure_3:
    lock incq failures(%rip)
    jmp     3b
record_phase_failure_1:
    lock incq failures(%rip)
    jmp     5b
record_phase_failure_2:
    lock incq failures(%rip)
    jmp     7b
record_phase_failure_3:
    lock incq failures(%rip)
    jmp     9b
record_phase_failure_4:
    lock incq failures(%rip)
    jmp     9b

// Four-child reusable barrier. The last arrival resets count before publishing
// the new generation; waiters cannot enter the next generation early.
barrier:
    mov     barrier_generation(%rip), %edx
    mov     $1, %eax
    lock xaddl %eax, barrier_count(%rip)
    cmp     $THREADS-1, %eax
    jne     barrier_wait
    movl    $0, barrier_count(%rip)
    lock incl barrier_generation(%rip)
    ret
barrier_wait:
    pause
    cmp     barrier_generation(%rip), %edx
    je      barrier_wait
    ret

next_child:
    inc     %r12d
    cmp     $THREADS, %r12d
    jne     wait_child

    mov     $10, %edi
    cmpq    $TOTAL, inc_counter(%rip)
    jne     finish
    mov     $11, %edi
    cmpq    $0, dec_counter(%rip)
    jne     finish
    mov     $12, %edi
    cmpq    $TOTAL, adc_counter(%rip)
    jne     finish
    mov     $13, %edi
    cmpq    $0, sbb_counter(%rip)
    jne     finish
    movabs  $0xfedcba9876543211, %rax
    mov     $14, %edi
    cmp     %rax, neg_word(%rip)
    jne     finish
    movabs  $0xfedcba9876543210, %rax
    mov     $15, %edi
    cmp     %rax, not_word(%rip)
    jne     finish
    mov     $16, %edi
    cmpq    $TOTAL, protected_counter(%rip)
    jne     finish
    mov     $17, %edi
    cmpq    $TOTAL, mapped_rbx_counter(%rip)
    jne     finish
    mov     $18, %edi
    cmpq    $TOTAL, mapped_rbp_counter(%rip)
    jne     finish
    mov     $19, %edi
    cmpq    $TOTAL, cmpxchg8b_counter(%rip)
    jne     finish
    mov     $20, %edi
    cmpq    $PHASE_ITERS*2, btc_cf_count(%rip)
    jne     finish
    mov     $21, %edi
    cmpq    $0, failures(%rip)
    jne     finish

    mov     mapped_rbx_sums(%rip), %rcx
    add     mapped_rbx_sums+8(%rip), %rcx
    add     mapped_rbx_sums+16(%rip), %rcx
    add     mapped_rbx_sums+24(%rip), %rcx
    movabs  $OLD_SUM, %rax
    mov     $22, %edi
    cmp     %rax, %rcx
    jne     finish
    mov     mapped_rbp_sums(%rip), %rcx
    add     mapped_rbp_sums+8(%rip), %rcx
    add     mapped_rbp_sums+16(%rip), %rcx
    add     mapped_rbp_sums+24(%rip), %rcx
    movabs  $OLD_SUM, %rax
    mov     $23, %edi
    cmp     %rax, %rcx
    jne     finish

    xor     %edi, %edi
    jmp     finish
fail:
    mov     $1, %edi
finish:
    mov     $231, %eax              // exit_group
    syscall
