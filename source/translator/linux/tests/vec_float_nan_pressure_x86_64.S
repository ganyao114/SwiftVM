.global _start
.text
_start:
    sub $128, %rsp

    # Keep seven guest GPR values and ten guest SIMD values live across the
    # entire straight-line floating-point sequence. This leaves little room
    # for backend temporaries when the sequence is compiled as one function.
    mov $0x01020304, %ebx
    mov $0x11121314, %ebp
    mov $0x21222324, %r8d
    mov $0x31323334, %r9d
    mov $0x41424344, %r10d
    mov $0x51525354, %r11d
    mov $0x61626364, %r12d

    movdqa xmm_sentinel_6(%rip), %xmm6
    movdqa xmm_sentinel_7(%rip), %xmm7
    movdqa xmm_sentinel_8(%rip), %xmm8
    movdqa xmm_sentinel_9(%rip), %xmm9
    movdqa xmm_sentinel_10(%rip), %xmm10
    movdqa xmm_sentinel_11(%rip), %xmm11
    movdqa xmm_sentinel_12(%rip), %xmm12
    movdqa xmm_sentinel_13(%rip), %xmm13
    movdqa xmm_sentinel_14(%rip), %xmm14
    movdqa xmm_sentinel_15(%rip), %xmm15

    # Each repetition expands to a real straight-line sequence. Inputs cover
    # signaling/quiet NaN payloads, infinities, denormals, and signed zero.
    .rept 32
        movdqa sqrt_input(%rip), %xmm0
        sqrtps %xmm0, %xmm0

        movdqa add_left(%rip), %xmm1
        addps add_right(%rip), %xmm1

        movdqa mul_left(%rip), %xmm2
        mulps mul_right(%rip), %xmm2

        movdqa div_left(%rip), %xmm3
        divps div_right(%rip), %xmm3

        movdqa minmax_left(%rip), %xmm4
        minps minmax_right(%rip), %xmm4

        movdqa minmax_left(%rip), %xmm5
        maxps minmax_right(%rip), %xmm5
    .endr

    movdqu %xmm0, 0(%rsp)
    movdqu %xmm1, 16(%rsp)
    movdqu %xmm2, 32(%rsp)
    movdqu %xmm3, 48(%rsp)
    movdqu %xmm4, 64(%rsp)
    movdqu %xmm5, 80(%rsp)

    # sqrtps: quiet the source NaN, preserve negative zero, retain infinity,
    # and round sqrt(the smallest positive denormal) exactly.
    mov $1, %r13d
    cmpl $0x7fc12345, 0(%rsp)
    jne fail
    cmpl $0x80000000, 4(%rsp)
    jne fail
    cmpl $0x7f800000, 8(%rsp)
    jne fail
    cmpl $0x1a3504f3, 12(%rsp)
    jne fail

    # addps: operand 1 NaN priority, operand 2 NaN quieting, invalid
    # infinity cancellation, and denormal plus negative zero.
    mov $2, %r13d
    cmpl $0x7fc11111, 16(%rsp)
    jne fail
    cmpl $0x7fc33333, 20(%rsp)
    jne fail
    cmpl $0xffc00000, 24(%rsp)
    jne fail
    cmpl $0x00000001, 28(%rsp)
    jne fail

    # mulps: operand 1 NaN priority, operand 2 NaN propagation, infinity
    # times zero invalid, and an exact denormal result.
    mov $3, %r13d
    cmpl $0x7fc11111, 32(%rsp)
    jne fail
    cmpl $0x7fc33333, 36(%rsp)
    jne fail
    cmpl $0xffc00000, 40(%rsp)
    jne fail
    cmpl $0x00000002, 44(%rsp)
    jne fail

    # divps: both NaN priority cases plus zero/zero and infinity/infinity.
    mov $4, %r13d
    cmpl $0x7fc11111, 48(%rsp)
    jne fail
    cmpl $0x7fc33333, 52(%rsp)
    jne fail
    cmpl $0xffc00000, 56(%rsp)
    jne fail
    cmpl $0xffc00000, 60(%rsp)
    jne fail

    # SSE min/max choose operand 2 for unordered and equal values. The other
    # lanes distinguish numeric min/max and retain infinity/denormal bits.
    mov $5, %r13d
    cmpl $0x40000000, 64(%rsp)
    jne fail
    cmpl $0x7f833333, 68(%rsp)
    jne fail
    cmpl $0x80000000, 72(%rsp)
    jne fail
    cmpl $0xff800000, 76(%rsp)
    jne fail

    mov $6, %r13d
    cmpl $0x40400000, 80(%rsp)
    jne fail
    cmpl $0x7f833333, 84(%rsp)
    jne fail
    cmpl $0x80000000, 88(%rsp)
    jne fail
    cmpl $0x00000001, 92(%rsp)
    jne fail

    # Consume every pressure sentinel only after all NaN fixups have emitted.
    mov $7, %r13d
    cmpl $0x01020304, %ebx
    jne fail
    cmpl $0x11121314, %ebp
    jne fail
    cmpl $0x21222324, %r8d
    jne fail
    cmpl $0x31323334, %r9d
    jne fail
    cmpl $0x41424344, %r10d
    jne fail
    cmpl $0x51525354, %r11d
    jne fail
    cmpl $0x61626364, %r12d
    jne fail

    mov $8, %r13d
    movd %xmm6, %eax
    cmpl $0x66666666, %eax
    jne fail
    movd %xmm7, %eax
    cmpl $0x77777777, %eax
    jne fail
    movd %xmm8, %eax
    cmpl $0x88888888, %eax
    jne fail
    movd %xmm9, %eax
    cmpl $0x99999999, %eax
    jne fail
    movd %xmm10, %eax
    cmpl $0xaaaaaaaa, %eax
    jne fail
    movd %xmm11, %eax
    cmpl $0xbbbbbbbb, %eax
    jne fail
    movd %xmm12, %eax
    cmpl $0xcccccccc, %eax
    jne fail
    movd %xmm13, %eax
    cmpl $0xdddddddd, %eax
    jne fail
    movd %xmm14, %eax
    cmpl $0xeeeeeeee, %eax
    jne fail
    movd %xmm15, %eax
    cmpl $0xffffffff, %eax
    jne fail

    xor %edi, %edi
    mov $60, %eax
    syscall

fail:
    mov %r13d, %edi
    mov $60, %eax
    syscall

.p2align 4
sqrt_input:
    .long 0x7f812345, 0x80000000, 0x7f800000, 0x00000001
add_left:
    .long 0x7fc11111, 0x3f800000, 0x7f800000, 0x00000001
add_right:
    .long 0x7f822222, 0x7f833333, 0xff800000, 0x80000000
mul_left:
    .long 0x7f811111, 0x40000000, 0x7f800000, 0x00000001
mul_right:
    .long 0x7fc22222, 0x7fc33333, 0x00000000, 0x40000000
div_left:
    .long 0x7fc11111, 0x40000000, 0x00000000, 0x7f800000
div_right:
    .long 0x7f822222, 0x7f833333, 0x00000000, 0x7f800000
minmax_left:
    .long 0x40400000, 0x7fc11111, 0x00000000, 0xff800000
minmax_right:
    .long 0x40000000, 0x7f833333, 0x80000000, 0x00000001

.p2align 4
xmm_sentinel_6:
    .long 0x66666666, 0x66666666, 0x66666666, 0x66666666
xmm_sentinel_7:
    .long 0x77777777, 0x77777777, 0x77777777, 0x77777777
xmm_sentinel_8:
    .long 0x88888888, 0x88888888, 0x88888888, 0x88888888
xmm_sentinel_9:
    .long 0x99999999, 0x99999999, 0x99999999, 0x99999999
xmm_sentinel_10:
    .long 0xaaaaaaaa, 0xaaaaaaaa, 0xaaaaaaaa, 0xaaaaaaaa
xmm_sentinel_11:
    .long 0xbbbbbbbb, 0xbbbbbbbb, 0xbbbbbbbb, 0xbbbbbbbb
xmm_sentinel_12:
    .long 0xcccccccc, 0xcccccccc, 0xcccccccc, 0xcccccccc
xmm_sentinel_13:
    .long 0xdddddddd, 0xdddddddd, 0xdddddddd, 0xdddddddd
xmm_sentinel_14:
    .long 0xeeeeeeee, 0xeeeeeeee, 0xeeeeeeee, 0xeeeeeeee
xmm_sentinel_15:
    .long 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff
