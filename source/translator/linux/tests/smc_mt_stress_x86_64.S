// MT self-modifying-code STRESS (amplified clone_smc_mt).
//
// Unlike clone_smc_mt, patchers and runners are deliberately UNSYNCHRONIZED:
// a patch may land while another thread is decoding/publishing the very unit
// being invalidated, which is the interleaving the lock-stepped test cannot
// reach. Four guest threads:
//   runner_a: calls workerA/workerA2 and workerC/workerC2 in a loop
//   runner_b: calls workerB/workerB2 and workerD/workerD2 in a loop
//   patcher_a: rewrites workerA/workerC immediates
//   patcher_b: rewrites workerB/workerD immediates
// Each worker page holds two functions with a small CFG so function-mode
// compilation publishes several blocks per unit.
//
// The guest never validates the observed immediates (they race by design);
// the point is that the HOST must survive.
//   exit 0    all four worker threads reported in
//   exit 1    clone failed
//   exit 10x  watchdog: only x of 4 workers reported (a guest thread was
//             halted by the runtime -- see "Guest thread N halted" on stdout)
// A host abort / SIGILL is the failure this test exists for: before the
// SmcTracker node-ownership fix that happened on ~100% of runs.

    .set ITERS, 4000
    .set THROTTLE, 4000

    .section .smc,"awx",@progbits
    .global _start
_start:
    mov     $0x50f00, %edi          // CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM
    lea     stack0_end(%rip), %rsi
    xor     %edx, %edx
    xor     %r10d, %r10d
    xor     %r8d, %r8d
    mov     $56, %eax
    syscall
    test    %eax, %eax
    js      clone_fail
    jz      runner_a

    mov     $0x50f00, %edi
    lea     stack1_end(%rip), %rsi
    xor     %edx, %edx
    xor     %r10d, %r10d
    xor     %r8d, %r8d
    mov     $56, %eax
    syscall
    test    %eax, %eax
    js      clone_fail
    jz      runner_b

    mov     $0x50f00, %edi
    lea     stack2_end(%rip), %rsi
    xor     %edx, %edx
    xor     %r10d, %r10d
    xor     %r8d, %r8d
    mov     $56, %eax
    syscall
    test    %eax, %eax
    js      clone_fail
    jz      patcher_a

    mov     $0x50f00, %edi
    lea     stack3_end(%rip), %rsi
    xor     %edx, %edx
    xor     %r10d, %r10d
    xor     %r8d, %r8d
    mov     $56, %eax
    syscall
    test    %eax, %eax
    js      clone_fail
    jz      patcher_b

    mov     $2000000, %r14d
main_wait:
    mov     done(%rip), %eax
    cmp     $4, %eax
    je      main_done
    mov     $39, %eax               // __NR_getpid: host boundary + yield
    syscall
    dec     %r14d
    jnz     main_wait
    // watchdog: report how many workers reported in
    mov     done(%rip), %edi
    add     $100, %edi
    jmp     finish
main_done:
    xor     %edi, %edi
    jmp     finish
clone_fail:
    mov     $1, %edi
finish:
    mov     $231, %eax              // __NR_exit_group
    syscall
    hlt

runner_a:
    mov     $ITERS, %r12d
ra_loop:
    call    workerA
    call    workerA2
    call    workerC
    call    workerC2
    mov     $39, %eax
    syscall
    dec     %r12d
    jnz     ra_loop
    jmp     thread_done

runner_b:
    mov     $ITERS, %r12d
rb_loop:
    call    workerB
    call    workerB2
    call    workerD
    call    workerD2
    mov     $39, %eax
    syscall
    dec     %r12d
    jnz     rb_loop
    jmp     thread_done

patcher_a:
    mov     $ITERS, %r12d
pa_loop:
    mov     %r12b, %al
    and     $1, %al
    add     $1, %al                 // 1 or 2
    lea     workerA(%rip), %r15
    movb    %al, 1(%r15)
    lea     workerA2(%rip), %r15
    movb    %al, 1(%r15)
    lea     workerC(%rip), %r15
    movb    %al, 1(%r15)
    lea     workerC2(%rip), %r15
    movb    %al, 1(%r15)
    mov     $THROTTLE, %r13d
pa_spin:
    pause
    dec     %r13d
    jnz     pa_spin
    mov     $39, %eax
    syscall
    dec     %r12d
    jnz     pa_loop
    jmp     thread_done

patcher_b:
    mov     $ITERS, %r12d
pb_loop:
    mov     %r12b, %al
    and     $1, %al
    add     $1, %al
    lea     workerB(%rip), %r15
    movb    %al, 1(%r15)
    lea     workerB2(%rip), %r15
    movb    %al, 1(%r15)
    lea     workerD(%rip), %r15
    movb    %al, 1(%r15)
    lea     workerD2(%rip), %r15
    movb    %al, 1(%r15)
    mov     $THROTTLE, %r13d
pb_spin:
    pause
    dec     %r13d
    jnz     pb_spin
    mov     $39, %eax
    syscall
    dec     %r12d
    jnz     pb_loop
    jmp     thread_done

thread_done:
    lock incl done(%rip)
    xor     %edi, %edi
    mov     $60, %eax               // __NR_exit (this thread)
    syscall
    hlt

// ---- four worker pages, two functions each -------------------------------

    .p2align 14
workerA:
    mov     $1, %eax                // B8 imm32; byte +1 is patched
    test    $1, %al
    jz      1f
    add     $3, %eax
    jmp     2f
1:  add     $5, %eax
2:  ret
workerA2:
    mov     $1, %eax
    test    $2, %al
    jz      3f
    sub     $1, %eax
    jmp     4f
3:  add     $7, %eax
4:  ret

    .p2align 14
workerB:
    mov     $1, %eax
    test    $1, %al
    jz      5f
    add     $3, %eax
    jmp     6f
5:  add     $5, %eax
6:  ret
workerB2:
    mov     $1, %eax
    test    $2, %al
    jz      7f
    sub     $1, %eax
    jmp     8f
7:  add     $7, %eax
8:  ret

    .p2align 14
workerC:
    mov     $1, %eax
    test    $1, %al
    jz      9f
    add     $3, %eax
    jmp     10f
9:  add     $5, %eax
10: ret
workerC2:
    mov     $1, %eax
    test    $2, %al
    jz      11f
    sub     $1, %eax
    jmp     12f
11: add     $7, %eax
12: ret

    .p2align 14
workerD:
    mov     $1, %eax
    test    $1, %al
    jz      13f
    add     $3, %eax
    jmp     14f
13: add     $5, %eax
14: ret
workerD2:
    mov     $1, %eax
    test    $2, %al
    jz      15f
    sub     $1, %eax
    jmp     16f
15: add     $7, %eax
16: ret

// ---- data / stacks on their own pages -------------------------------------

    .p2align 14
    .balign 8
done:
    .long 0
    .balign 16
stack0:
    .zero 65536
stack0_end:
stack1:
    .zero 65536
stack1_end:
stack2:
    .zero 65536
stack2_end:
stack3:
    .zero 65536
stack3_end:
