// Raw Linux x86_64 qualification for the opt-in x87 mid-tier JIT.
// Returns zero only when loads/conversions, memory arithmetic, comparisons,
// and special FSQRT results match their architectural bit patterns.
    .global _start
    .text
_start:
    fninit

    // FILD m64 above the exact-f64 boundary must retain its ext80 low bit.
    fildll  int_2p53_plus_1(%rip)
    fstpt   out80(%rip)
    movabs  $0x8000000000000400, %rax
    cmpq    %rax, out80(%rip)
    jne     fail_1
    cmpw    $0x4034, out80+8(%rip)
    jne     fail_1

    // FILD m32 is exactly representable and seeds canonical provenance.
    fildl   int_min32(%rip)
    fstpt   out80(%rip)
    movabs  $0x8000000000000000, %rax
    cmpq    %rax, out80(%rip)
    jne     fail_2
    cmpw    $0xc01e, out80+8(%rip)
    jne     fail_2

    // Runtime FCW.RC selection: +1.5 rounds up/down to 2/1.
    fldcw   cw_up(%rip)
    fldl    one_half(%rip)
    fistpl  out32(%rip)
    cmpl    $2, out32(%rip)
    jne     fail_3
    fldcw   cw_down(%rip)
    fldl    one_half(%rip)
    fistpl  out32(%rip)
    cmpl    $1, out32(%rip)
    jne     fail_3

    // +2^31 is invalid for m32 and stores integer indefinite with IE.
    fninit
    fldl    plus_2p31(%rip)
    fistpl  out32(%rip)
    cmpl    $0x80000000, out32(%rip)
    jne     fail_4
    fnstsw  %ax
    testw   $1, %ax
    jz      fail_4

    // Exact memory-form arithmetic in both source widths.
    fninit
    fld1
    fadds   two_f32(%rip)
    fstpt   out80(%rip)
    movabs  $0xc000000000000000, %rax
    cmpq    %rax, out80(%rip)
    jne     fail_5
    cmpw    $0x4000, out80+8(%rip)
    jne     fail_5
    fld1
    faddl   half_f64(%rip)
    fstpt   out80(%rip)
    cmpq    %rax, out80(%rip)
    jne     fail_5
    cmpw    $0x3fff, out80+8(%rip)
    jne     fail_5

    // FCOMI must define CF=1, PF=ZF=0 for 0 < 1 and clear OF/SF/AF.
    fninit
    fld1
    fldz
    fcomi   %st(1), %st
    jnc     fail_6
    jz      fail_6
    jp      fail_6
    jo      fail_6
    js      fail_6
    lahf
    testb   $0x10, %ah
    jnz     fail_6
    fstp    %st(0)
    fstp    %st(0)

    // Negative finite FSQRT uses the x87 negative indefinite and raises IE.
    fninit
    fldl    minus_four(%rip)
    fsqrt
    fstpt   out80(%rip)
    movabs  $0xc000000000000000, %rax
    cmpq    %rax, out80(%rip)
    jne     fail_7
    cmpw    $0xffff, out80+8(%rip)
    jne     fail_7
    fnstsw  %ax
    testw   $1, %ax
    jz      fail_7

    // sqrt(-0) preserves the sign and does not raise IE.
    fninit
    fldl    minus_zero(%rip)
    fsqrt
    fstpt   out80(%rip)
    cmpq    $0, out80(%rip)
    jne     fail_8
    cmpw    $0x8000, out80+8(%rip)
    jne     fail_8
    fnstsw  %ax
    testw   $1, %ax
    jnz     fail_8

    // A small helper-count benchmark for the new common compare/sqrt path.
    // Default mode executes four exact helpers per iteration; opt-in mode
    // keeps FLD1/FSQRT/FTST/FSTP entirely inside EmitX87Op.
    mov     $10000, %ecx
bench_loop:
    fld1
    fsqrt
    ftst
    fstp    %st(0)
    dec     %ecx
    jnz     bench_loop

    xor     %edi, %edi
    jmp     exit
fail_1: mov $1, %edi; jmp exit
fail_2: mov $2, %edi; jmp exit
fail_3: mov $3, %edi; jmp exit
fail_4: mov $4, %edi; jmp exit
fail_5: mov $5, %edi; jmp exit
fail_6: mov $6, %edi; jmp exit
fail_7: mov $7, %edi; jmp exit
fail_8: mov $8, %edi
exit:
    mov     $231, %eax
    syscall

    .p2align 4
int_2p53_plus_1: .quad 0x0020000000000001
int_min32:       .long 0x80000000
cw_up:           .short 0x0b7f
cw_down:         .short 0x077f
one_half:        .quad 0x3ff8000000000000
plus_2p31:       .quad 0x41e0000000000000
two_f32:         .long 0x40000000
half_f64:        .quad 0x3fe0000000000000
minus_four:      .quad 0xc010000000000000
minus_zero:      .quad 0x8000000000000000
    .p2align 4
out80:           .space 16
out32:           .long 0
