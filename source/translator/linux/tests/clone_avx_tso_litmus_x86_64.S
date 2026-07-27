// Libc-free persistent-thread AVX message-passing litmus.
//
// This is the vector sibling of clone_tso_litmus_x86_64.S and uses the same
// clone/futex harness and monotonically increasing epochs. Each phase stores
// four cache-line-separated data values, publishes a scalar flag, then checks
// the data after observing the flag. x86 TSO forbids stale data in every phase.
//
// Phases:
//   avx128  - VMOVUPS XMM stores and loads
//   avx256  - VMOVUPS YMM stores and loads (two V128 IR halves)
//   masked  - VMASKMOVPS YMM stores with all lanes enabled, VMOVUPS loads
//   gather  - scalar producer stores, VPGATHERDD consumer loads
//
// SVM_TSO_MODE=acqrel must print four zero counts and exit 0. Relaxed mode is
// the detector check: it should observe at least one violation and exit 1.

    .equ VARIANTS, 4
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

    .balign 128
avx_data0:
    .skip 32
    .balign 128
avx_data1:
    .skip 32
    .balign 128
avx_data2:
    .skip 32
    .balign 128
avx_data3:
    .skip 32
    .balign 128
avx_flag:
    .long 0
    .balign 128
avx_epoch:
    .long 0
    .balign 128
avx_done0:
    .long 0
    .balign 128
avx_done1:
    .long 0
    .balign 128
avx_bad:
    .zero 32
number_buffer:
    .skip 32

    .section .rodata
    .balign 32
all_lane_mask:
    .long -1, -1, -1, -1, -1, -1, -1, -1
gather_lane_mask:
    .long 0x80000000, 0x80000000, 0x80000000, 0x80000000
    .long 0, 0, 0, 0
gather_indices:
    .long 0
    .long avx_data1-avx_data0
    .long avx_data2-avx_data0
    .long avx_data3-avx_data0
    .long 0, 0, 0, 0

avx128_label:
    .ascii "avx128_bad="
avx256_label:
    .ascii " avx256_bad="
masked_label:
    .ascii " masked_bad="
gather_label:
    .ascii " gather_bad="
newline:
    .ascii "\n"

    .section .text
    .global _start
_start:
    xor     %r13d, %r13d

variant_start:
    movl    $0, avx_data0(%rip)
    movl    $0, avx_data1(%rip)
    movl    $0, avx_data2(%rip)
    movl    $0, avx_data3(%rip)
    movl    $0, avx_flag(%rip)
    movl    $0, avx_epoch(%rip)
    movl    $0, avx_done0(%rip)
    movl    $0, avx_done1(%rip)
    movq    $0, avx_bad(,%r13,8)

    call    spawn_pair
    mov     $1, %r15d

parent_loop:
    movl    %r15d, avx_epoch(%rip)
parent_wait_done:
    cmp     %r15d, avx_done0(%rip)
    jne     parent_wait_done
    cmp     %r15d, avx_done1(%rip)
    jne     parent_wait_done
    cmpq    $MP_TARGET, avx_bad(,%r13,8)
    jae     variant_done
    inc     %r15d
    cmp     $(MP_ITERATIONS + 1), %r15d
    jne     parent_loop

variant_done:
    movl    $-1, avx_epoch(%rip)
    call    join_pair
    inc     %r13d
    cmp     $VARIANTS, %r13d
    jne     variant_start

    mov     $1, %edi
    lea     avx128_label(%rip), %rsi
    mov     $11, %edx
    mov     $1, %eax
    syscall
    mov     avx_bad(%rip), %rdi
    call    print_u64

    mov     $1, %edi
    lea     avx256_label(%rip), %rsi
    mov     $12, %edx
    mov     $1, %eax
    syscall
    mov     avx_bad+8(%rip), %rdi
    call    print_u64

    mov     $1, %edi
    lea     masked_label(%rip), %rsi
    mov     $12, %edx
    mov     $1, %eax
    syscall
    mov     avx_bad+16(%rip), %rdi
    call    print_u64

    mov     $1, %edi
    lea     gather_label(%rip), %rsi
    mov     $12, %edx
    mov     $1, %eax
    syscall
    mov     avx_bad+24(%rip), %rdi
    call    print_u64

    mov     $1, %edi
    lea     newline(%rip), %rsi
    mov     $1, %edx
    mov     $1, %eax
    syscall

    mov     avx_bad(%rip), %rax
    or      avx_bad+8(%rip), %rax
    or      avx_bad+16(%rip), %rax
    or      avx_bad+24(%rip), %rax
    xor     %edi, %edi
    test    %rax, %rax
    je      finish
    mov     $1, %edi
finish:
    mov     $SYS_exit_group, %eax
    syscall

// Spawn two host-backed guest threads. r13 selects the AVX phase; each child
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
    test    %r12d, %r12d
    jnz     consumer

producer:
    xor     %r15d, %r15d
producer_wait:
    mov     avx_epoch(%rip), %eax
    cmp     %r15d, %eax
    je      producer_wait
    cmp     $-1, %eax
    je      child_exit
    mov     %eax, %r15d

    test    %r13d, %r13d
    je      producer_128
    cmp     $1, %r13d
    je      producer_256
    cmp     $2, %r13d
    je      producer_masked
    jmp     producer_gather

producer_128:
    vmovd       %eax, %xmm0
    vpbroadcastd %xmm0, %xmm0
    vmovups     %xmm0, avx_data0(%rip)
    vmovups     %xmm0, avx_data1(%rip)
    vmovups     %xmm0, avx_data2(%rip)
    vmovups     %xmm0, avx_data3(%rip)
    jmp         producer_publish

producer_256:
    vmovd       %eax, %xmm0
    vpbroadcastd %xmm0, %ymm0
    vmovups     %ymm0, avx_data0(%rip)
    vmovups     %ymm0, avx_data1(%rip)
    vmovups     %ymm0, avx_data2(%rip)
    vmovups     %ymm0, avx_data3(%rip)
    jmp         producer_publish

producer_masked:
    vmovd       %eax, %xmm0
    vpbroadcastd %xmm0, %ymm0
    vmovups     all_lane_mask(%rip), %ymm1
    vmaskmovps  %ymm0, %ymm1, avx_data0(%rip)
    vmaskmovps  %ymm0, %ymm1, avx_data1(%rip)
    vmaskmovps  %ymm0, %ymm1, avx_data2(%rip)
    vmaskmovps  %ymm0, %ymm1, avx_data3(%rip)
    jmp         producer_publish

producer_gather:
    mov     %eax, avx_data0(%rip)
    mov     %eax, avx_data1(%rip)
    mov     %eax, avx_data2(%rip)
    mov     %eax, avx_data3(%rip)

producer_publish:
    mov     %eax, avx_flag(%rip)
    mov     %eax, avx_done0(%rip)
    jmp     producer_wait

consumer:
    xor     %r15d, %r15d
consumer_wait:
    mov     avx_epoch(%rip), %eax
    cmp     %r15d, %eax
    je      consumer_wait
    cmp     $-1, %eax
    je      child_exit
    mov     %eax, %r15d
consumer_wait_flag:
    cmp     avx_flag(%rip), %eax
    ja      consumer_wait_flag

    test    %r13d, %r13d
    je      consumer_128
    cmp     $1, %r13d
    je      consumer_256
    cmp     $2, %r13d
    je      consumer_256
    jmp     consumer_gather

consumer_128:
    vmovd       %eax, %xmm2
    vpbroadcastd %xmm2, %xmm2

    vmovups     avx_data0(%rip), %xmm0
    vpcmpeqd    %xmm2, %xmm0, %xmm0
    vpmovmskb   %xmm0, %edx
    cmp         $0xffff, %edx
    jne         consumer_stale

    vmovups     avx_data1(%rip), %xmm0
    vpcmpeqd    %xmm2, %xmm0, %xmm0
    vpmovmskb   %xmm0, %edx
    cmp         $0xffff, %edx
    jne         consumer_stale

    vmovups     avx_data2(%rip), %xmm0
    vpcmpeqd    %xmm2, %xmm0, %xmm0
    vpmovmskb   %xmm0, %edx
    cmp         $0xffff, %edx
    jne         consumer_stale

    vmovups     avx_data3(%rip), %xmm0
    vpcmpeqd    %xmm2, %xmm0, %xmm0
    vpmovmskb   %xmm0, %edx
    cmp         $0xffff, %edx
    jne         consumer_stale
    jmp         consumer_done

consumer_256:
    vmovd       %eax, %xmm2
    vpbroadcastd %xmm2, %ymm2

    vmovups     avx_data0(%rip), %ymm0
    vpcmpeqd    %ymm2, %ymm0, %ymm0
    vpmovmskb   %ymm0, %edx
    cmp         $-1, %edx
    jne         consumer_stale

    vmovups     avx_data1(%rip), %ymm0
    vpcmpeqd    %ymm2, %ymm0, %ymm0
    vpmovmskb   %ymm0, %edx
    cmp         $-1, %edx
    jne         consumer_stale

    vmovups     avx_data2(%rip), %ymm0
    vpcmpeqd    %ymm2, %ymm0, %ymm0
    vpmovmskb   %ymm0, %edx
    cmp         $-1, %edx
    jne         consumer_stale

    vmovups     avx_data3(%rip), %ymm0
    vpcmpeqd    %ymm2, %ymm0, %ymm0
    vpmovmskb   %ymm0, %edx
    cmp         $-1, %edx
    jne         consumer_stale
    jmp         consumer_done

consumer_gather:
    vmovd       %eax, %xmm3
    vpbroadcastd %xmm3, %xmm3
    vmovups     gather_indices(%rip), %ymm1
    vmovups     gather_lane_mask(%rip), %ymm2
    lea         avx_data0(%rip), %rdi
    vpgatherdd  %ymm2, (%rdi,%ymm1,1), %ymm0
    vpcmpeqd    %xmm3, %xmm0, %xmm0
    vpmovmskb   %xmm0, %edx
    cmp         $0xffff, %edx
    jne         consumer_stale
    jmp         consumer_done

consumer_stale:
    incq    avx_bad(,%r13,8)
consumer_done:
    mov     %r15d, avx_done1(%rip)
    jmp     consumer_wait

child_exit:
    vzeroupper
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
