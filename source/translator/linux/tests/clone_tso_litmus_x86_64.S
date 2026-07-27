// Libc-free persistent-thread SB + MP litmus for the macOS-hosted launcher.
// It is a local fallback when the Orb x64 VM cannot build the pthread tests.
// The qualification sources remain tso_litmus.c and the native x64 results.
//
// The checked-in ELF next to this file was rebuilt without a Linux box, via
//   clang -target x86_64-unknown-linux-gnu -c -ffreestanding -nostdlib -fno-pic
//   python3 mklinuxelf.py -o clone_tso_litmus_x86_64 <obj>
// which is what mklinuxelf.py exists for; build_real_tests.sh's gcc path
// produces an equivalent binary when a Linux machine is available. There is
// deliberately no .native.txt for this test: native x86 is TSO, so it always
// prints mp_bad=0 and exit 0, which is the acqrel expectation and says nothing
// about the relaxed side.
//
// TWO-SIDED TEST. The MP phase is what gates the exit code:
//   SVM_TSO_MODE=acqrel  -> mp_bad MUST be 0. This is the red line: SwiftVM
//                           claims x86-TSO, and TSO forbids the outcome.
//   SVM_TSO_MODE=relaxed -> mp_bad MUST be > 0. This is the DETECTOR CHECK.
//                           Relaxed mode deliberately emits plain ldr/str, so
//                           the violation is reachable; if this side ever
//                           reads 0 the acqrel side proves nothing, because a
//                           detector that cannot see a violation reports 0 for
//                           a broken implementation exactly as it does for a
//                           correct one.
//
// MP SHAPE, and why it looks like this.
//
// Producer stores mp_data0..3 then mp_flag; consumer spins until mp_flag has
// reached the current epoch, then requires every mp_dataN to have reached it
// too. Epoch values are strictly increasing and nobody ever resets them, so
// "stale" is exactly `epoch > dataN` -- an unsigned `ja`, not `jne`. That
// matters twice: the parent no longer has to zero the words each iteration
// (its own zeroing store would otherwise race the producer under relaxed and
// forge a violation), and a consumer that races one epoch AHEAD (`dataN >
// epoch`, reachable because ARM64 may reorder the consumer's mp_done1 store
// before its data loads) is correctly not counted.
//
// EVERY SHARED WORD SITS ON ITS OWN CACHE LINE (.balign 128 -- Apple Silicon's
// line size). This is the load-bearing part, and its absence is what silently
// broke this test. With mp_data and mp_flag packed into one 8-byte granule the
// two stores merge in the store buffer and the consumer's core cannot obtain
// the new flag without also obtaining the new data, so the window is not
// merely narrow, it is closed by the cache protocol: measured mp_bad was 0 in
// 12/12 relaxed runs. Separating the lines, with nothing else changed, moved
// relaxed mp_bad to 8..6940 (min over 6 runs) while acqrel stayed 0.
//
// Four data words rather than one, because the flag store may overtake any of
// them and checking all four catches whichever it was. Four beat 1, 2, 8 and
// 16 in measurement: past four, the extra stores lengthen the producer's
// critical section enough that the epoch rate -- and with it the count -- goes
// back down (N=16 measured 2..142 where N=4 measured 127..31775 on the same
// fixed-iteration build).
//
// MEASURED, this file, Release build, M-series host:
//   relaxed  40/40 runs reach mp_bad=64 (the target) and exit 1, in 0.4-1.2s
//   acqrel   40/40 runs end with mp_bad=0 and exit 0, in ~2.0-2.4s (the cap)
//   relaxed under SVM_FUNC_BASE=0 (block mode) behaves the same
//
// DO NOT "fix" a future relaxed-side 0 by relaxing the expectation to 0. That
// converts a dead detector into a green test. Widen the window instead: more
// data words, a larger MP_ITERATIONS cap, or a shape that does not depend on a
// window at all.

    .equ ITERATIONS, 1000000
// The MP phase is sample-budgeted, not iteration-budgeted: it stops as soon as
// it has seen MP_TARGET violations, and otherwise at MP_ITERATIONS. That is
// the part that makes the relaxed side stable rather than lucky. The violation
// RATE varies by four orders of magnitude between runs -- measured mp_bad over
// 40 fixed-1M-iteration runs was 1, 4, 5, 11, ... 139608 -- because it depends
// on which cores the three guest threads land on, and a fixed iteration count
// therefore inherits that whole spread and eventually rounds down to 0. A
// target instead spends iterations only on the runs that need them: a typical
// run reaches MP_TARGET long before the cap and exits immediately, while a
// badly-scheduled one is allowed 4M iterations before giving up.
//
// The cap is also the acqrel cost, since acqrel never scores and always runs
// to it. That is the intended asymmetry -- the red-line run is allowed to be
// the slow one.
    .equ MP_ITERATIONS, 4000000
    .equ MP_TARGET, 64
    .equ CLONE_FLAGS, 0x350f00
    .equ SYS_clone, 56
    .equ SYS_exit, 60
    .equ SYS_futex, 202
    .equ SYS_exit_group, 231
    .equ FUTEX_WAIT_PRIVATE, 128

    .section .bss
    .balign 16
worker_stacks:
    .skip 131072
    .balign 4
tid_words:
    .zero 8

sb_x:
    .long 0
sb_y:
    .long 0
sb_epoch:
    .long 0
sb_done0:
    .long 0
sb_done1:
    .long 0
sb_r1:
    .long 0
sb_r2:
    .long 0
    .balign 8
sb_zero_zero:
    .quad 0

    .balign 128
mp_data0:
    .long 0
    .balign 128
mp_data1:
    .long 0
    .balign 128
mp_data2:
    .long 0
    .balign 128
mp_data3:
    .long 0
    .balign 128
mp_flag:
    .long 0
    .balign 128
mp_epoch:
    .long 0
    .balign 128
mp_done0:
    .long 0
    .balign 128
mp_done1:
    .long 0
    .balign 128
mp_bad:
    .quad 0
number_buffer:
    .skip 32

    .section .rodata
sb_label:
    .ascii "sb00="
mp_label:
    .ascii " mp_bad="
newline:
    .ascii "\n"

    .section .text
    .global _start
_start:
    xor     %r14d, %r14d             // phase 0: SB
    call    spawn_pair

    mov     $1, %r15d
sb_parent_loop:
    movl    $0, sb_x(%rip)
    movl    $0, sb_y(%rip)
    movl    %r15d, sb_epoch(%rip)
sb_wait_done:
    cmp     %r15d, sb_done0(%rip)
    jne     sb_wait_done
    cmp     %r15d, sb_done1(%rip)
    jne     sb_wait_done
    mov     sb_r1(%rip), %eax
    or      sb_r2(%rip), %eax
    jne     sb_parent_next
    incq    sb_zero_zero(%rip)
sb_parent_next:
    inc     %r15d
    cmp     $(ITERATIONS + 1), %r15d
    jne     sb_parent_loop

    movl    $-1, sb_epoch(%rip)
    call    join_pair

    mov     $1, %r14d                 // phase 1: MP
    call    spawn_pair

    mov     $1, %r15d
mp_parent_loop:
    movl    %r15d, mp_epoch(%rip)
mp_wait_done:
    cmp     %r15d, mp_done0(%rip)
    jne     mp_wait_done
    cmp     %r15d, mp_done1(%rip)
    jne     mp_wait_done
    cmpq    $MP_TARGET, mp_bad(%rip)  // enough evidence: stop paying for more
    jae     mp_phase_done
    inc     %r15d
    cmp     $(MP_ITERATIONS + 1), %r15d
    jne     mp_parent_loop

mp_phase_done:
    movl    $-1, mp_epoch(%rip)
    call    join_pair

    mov     $1, %edi
    lea     sb_label(%rip), %rsi
    mov     $5, %edx
    mov     $1, %eax
    syscall
    mov     sb_zero_zero(%rip), %rdi
    call    print_u64
    mov     $1, %edi
    lea     mp_label(%rip), %rsi
    mov     $8, %edx
    mov     $1, %eax
    syscall
    mov     mp_bad(%rip), %rdi
    call    print_u64
    mov     $1, %edi
    lea     newline(%rip), %rsi
    mov     $1, %edx
    mov     $1, %eax
    syscall

    xor     %edi, %edi
    cmpq    $0, mp_bad(%rip)
    je      finish
    mov     $1, %edi
finish:
    mov     $SYS_exit_group, %eax
    syscall

// Spawn two host-backed guest threads. r14 selects the phase; each child
// inherits r12=0/1 and dispatches without returning through the new stack.
spawn_pair:
    xor     %r12d, %r12d
spawn_one:
    mov     $CLONE_FLAGS, %edi
    lea     worker_stacks+65536(%rip), %rsi
    mov     %r12, %rax
    shl     $16, %rax
    add     %rax, %rsi
    lea     tid_words(%rip), %rdx
    lea     (%rdx,%r12,4), %rdx
    mov     %rdx, %r10
    xor     %r8d, %r8d
    mov     $SYS_clone, %eax
    syscall
    test    %eax, %eax
    jz      child_dispatch
    js      spawn_fail
    inc     %r12d
    cmp     $2, %r12d
    jne     spawn_one
    ret
spawn_fail:
    mov     $2, %edi
    mov     $SYS_exit_group, %eax
    syscall

child_dispatch:
    test    %r14d, %r14d
    jnz     mp_child_dispatch
    test    %r12d, %r12d
    jnz     sb_worker1

    xor     %r15d, %r15d
sb_worker0_wait:
    mov     sb_epoch(%rip), %eax
    cmp     %r15d, %eax
    je      sb_worker0_wait
    cmp     $-1, %eax
    je      child_exit
    mov     %eax, %r15d
    movl    $1, sb_x(%rip)
    mov     sb_y(%rip), %edx
    mov     %edx, sb_r1(%rip)
    mov     %r15d, sb_done0(%rip)
    jmp     sb_worker0_wait

sb_worker1:
    xor     %r15d, %r15d
sb_worker1_wait:
    mov     sb_epoch(%rip), %eax
    cmp     %r15d, %eax
    je      sb_worker1_wait
    cmp     $-1, %eax
    je      child_exit
    mov     %eax, %r15d
    movl    $1, sb_y(%rip)
    mov     sb_x(%rip), %edx
    mov     %edx, sb_r2(%rip)
    mov     %r15d, sb_done1(%rip)
    jmp     sb_worker1_wait

mp_child_dispatch:
    test    %r12d, %r12d
    jnz     mp_consumer

    xor     %r15d, %r15d
mp_producer_wait:
    mov     mp_epoch(%rip), %eax
    cmp     %r15d, %eax
    je      mp_producer_wait
    cmp     $-1, %eax
    je      child_exit
    mov     %eax, %r15d
    mov     %eax, mp_data0(%rip)
    mov     %eax, mp_data1(%rip)
    mov     %eax, mp_data2(%rip)
    mov     %eax, mp_data3(%rip)
    mov     %eax, mp_flag(%rip)
    mov     %eax, mp_done0(%rip)
    jmp     mp_producer_wait

mp_consumer:
    xor     %r15d, %r15d
mp_consumer_wait:
    mov     mp_epoch(%rip), %eax
    cmp     %r15d, %eax
    je      mp_consumer_wait
    cmp     $-1, %eax
    je      child_exit
    mov     %eax, %r15d
mp_wait_flag:
    cmp     mp_flag(%rip), %eax
    ja      mp_wait_flag
    cmp     mp_data0(%rip), %eax
    ja      mp_consumer_stale
    cmp     mp_data1(%rip), %eax
    ja      mp_consumer_stale
    cmp     mp_data2(%rip), %eax
    ja      mp_consumer_stale
    cmp     mp_data3(%rip), %eax
    ja      mp_consumer_stale
    jmp     mp_consumer_done
mp_consumer_stale:
    incq    mp_bad(%rip)              // one consumer: no atomic RMW needed
mp_consumer_done:
    mov     %r15d, mp_done1(%rip)
    jmp     mp_consumer_wait

child_exit:
    xor     %edi, %edi
    mov     $SYS_exit, %eax
    syscall

join_pair:
    xor     %r12d, %r12d
join_one:
    lea     tid_words(%rip), %rdi
    lea     (%rdi,%r12,4), %rdi
join_wait:
    mov     (%rdi), %edx
    test    %edx, %edx
    jz      join_next
    mov     $FUTEX_WAIT_PRIVATE, %esi
    xor     %r10d, %r10d
    xor     %r8d, %r8d
    xor     %r9d, %r9d
    mov     $SYS_futex, %eax
    syscall
    jmp     join_wait
join_next:
    inc     %r12d
    cmp     $2, %r12d
    jne     join_one
    ret

// Print unsigned rdi to stdout without libc.
print_u64:
    lea     number_buffer+32(%rip), %rsi
    xor     %ecx, %ecx
    mov     %rdi, %rax
    test    %rax, %rax
    jne     print_digits
    dec     %rsi
    movb    $'0', (%rsi)
    mov     $1, %ecx
    jmp     print_write
print_digits:
    mov     $10, %r8
print_digit_loop:
    xor     %edx, %edx
    div     %r8
    add     $'0', %dl
    dec     %rsi
    mov     %dl, (%rsi)
    inc     %ecx
    test    %rax, %rax
    jne     print_digit_loop
print_write:
    mov     $1, %edi
    mov     %ecx, %edx
    mov     $1, %eax
    syscall
    ret
