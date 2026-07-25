// Libc-free persistent-thread SB + MP litmus for the macOS-hosted launcher.
// It is a local fallback when the Orb x64 VM cannot build the pthread tests.
// The qualification sources remain tso_litmus.c and the native x64 results.

    .equ ITERATIONS, 1000000
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

mp_data:
    .long 0
mp_flag:
    .long 0
mp_epoch:
    .long 0
mp_done0:
    .long 0
mp_done1:
    .long 0
    .balign 8
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
    movl    $0, mp_data(%rip)
    movl    $0, mp_flag(%rip)
    movl    %r15d, mp_epoch(%rip)
mp_wait_done:
    cmp     %r15d, mp_done0(%rip)
    jne     mp_wait_done
    cmp     %r15d, mp_done1(%rip)
    jne     mp_wait_done
    inc     %r15d
    cmp     $(ITERATIONS + 1), %r15d
    jne     mp_parent_loop

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
    mov     %eax, mp_data(%rip)
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
    jne     mp_wait_flag
    cmp     mp_data(%rip), %eax
    je      mp_consumer_done
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
