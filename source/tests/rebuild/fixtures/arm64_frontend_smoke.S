.arch armv8.2-a+dotprod+fp16+crypto
.text
.global _start
.type _start,%function
_start:
    mov x19, #6
    mov x20, #7
    mul x19, x19, x20
    bl check_flags
    bl check_division
    bl check_vectors
    bl check_fp
    mov x7, #4
loop:
    subs x7, x7, #1
    b.ne loop
    mov x0, #1
    adr x1, message
    mov x2, #(message_end-message)
    mov x8, #64
    svc #0
    mov x0, x19
    mov x8, #93
    svc #0
failure:
    mov x0, #125
    mov x8, #93
    svc #0
.size _start,.-_start

.type check_flags,%function
check_flags:
    mov x3, #7
    subs x3, x3, #7
    b.cc failure
    adc x0, xzr, xzr
    cmp x0, #1
    b.ne failure
    mov x0, #2
    cmp x0, #2
    ccmp x0, #2, #0, eq
    b.ne failure
    b.cc failure
    cmp x0, #3
    ccmp x0, #2, #0xa, eq
    mrs x3, nzcv
    mov x4, #0xa0000000
    cmp x3, x4
    b.ne failure
    ret
.size check_flags,.-check_flags

.type check_division,%function
check_division:
    mov x4, #-7
    udiv x3, x4, xzr
    cbnz x3, failure
    sdiv x3, x4, xzr
    cbnz x3, failure
    mov x5, #3
    sdiv x3, x4, x5
    cmn x3, #2
    b.ne failure
    mov x4, #-1
    mov x5, #0x8000000000000000
    sdiv x3, x5, x4
    cmp x3, x5
    b.ne failure
    mov w5, #0x80000000
    sdiv w3, w5, w4
    cmp x3, x5
    b.ne failure
    mov x5, #2
    udiv x3, x4, x5
    mov x5, #0x7fffffffffffffff
    cmp x3, x5
    b.ne failure
    ret
.size check_division,.-check_division

.type check_vectors,%function
check_vectors:
    movi v0.16b, #1
    movi v1.16b, #2
    add v2.16b, v0.16b, v1.16b
    umov w3, v2.b[0]
    cmp w3, #3
    b.ne failure
    movi v3.4s, #0
    sdot v3.4s, v0.16b, v1.16b
    umov w3, v3.s[3]
    cmp w3, #8
    b.ne failure
    msr fpsr, xzr
    movi v4.8h, #0x80, lsl #8
    sqrdmulh v5.8h, v4.8h, v4.8h
    umov w3, v5.h[7]
    mov w4, #0x7fff
    cmp w3, w4
    b.ne failure
    mrs x3, fpsr
    tbz x3, #27, failure
    msr fpsr, xzr
    ret
.size check_vectors,.-check_vectors

.type check_fp,%function
check_fp:
    mrs x21, fpcr
    msr fpcr, xzr
    fmov d0, #1.5
    fmov d1, #2.25
    fadd d2, d0, d1
    fcvtzs x20, d2
    cmp x20, #3
    b.ne failure
    // 1 + 2^-24 is the exact halfway case for binary32.
    mov w3, #0x3f800000
    mov w4, #0x33800000
    fmov s1, w3
    fmov s2, w4
    msr fpsr, xzr
    mov x3, #0x400000
    msr fpcr, x3
    mrs x4, fpcr
    cmp x3, x4
    b.ne failure
    fadd s0, s1, s2
    fmov w3, s0
    movz w4, #1
    movk w4, #0x3f80, lsl #16
    cmp w3, w4
    b.ne failure
    mrs x3, fpsr
    tbz x3, #4, failure
    // A signaling NaN preserves its payload and reports IOC with DN disabled.
    msr fpcr, xzr
    msr fpsr, xzr
    movz w3, #0x42
    movk w3, #0x7f80, lsl #16
    fmov s1, w3
    fadd s0, s1, s2
    fmov w3, s0
    movz w4, #0x42
    movk w4, #0x7fc0, lsl #16
    cmp w3, w4
    b.ne failure
    mrs x3, fpsr
    tbz x3, #0, failure
    fmov h0, #1.0
    fmov h1, #0.5
    fadd h2, h0, h1
    fmov w3, h2
    mov w4, #0x3e00
    cmp w3, w4
    b.ne failure
    msr fpcr, x21
    msr fpsr, xzr
    ret
.size check_fp,.-check_fp

.section .rodata
message:
    .ascii "arm64 frontend ok\n"
message_end:
.section .note.GNU-stack,"",%progbits
