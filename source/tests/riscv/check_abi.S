// Check every LP64D callee-saved register across a generated block/helper.
// This serial fixture keeps its expected SP in a private test-process word.
    .text
    .global SwiftRiscvCheckABI
    .type SwiftRiscvCheckABI, @function
SwiftRiscvCheckABI:
    addi sp, sp, -208
    sd ra, 0(sp)
    sd s0, 8(sp)
    sd s1, 16(sp)
    sd s2, 24(sp)
    sd s3, 32(sp)
    sd s4, 40(sp)
    sd s5, 48(sp)
    sd s6, 56(sp)
    sd s7, 64(sp)
    sd s8, 72(sp)
    sd s9, 80(sp)
    sd s10, 88(sp)
    sd s11, 96(sp)
    fsd fs0, 104(sp)
    fsd fs1, 112(sp)
    fsd fs2, 120(sp)
    fsd fs3, 128(sp)
    fsd fs4, 136(sp)
    fsd fs5, 144(sp)
    fsd fs6, 152(sp)
    fsd fs7, 160(sp)
    fsd fs8, 168(sp)
    fsd fs9, 176(sp)
    fsd fs10, 184(sp)
    fsd fs11, 192(sp)
    la t0, swift_riscv_abi_sp
    sd sp, 0(t0)
    mv t0, a0
    mv a0, a1
    li s0, 0x102
    li s1, 0x203
    li s2, 0x304
    li s3, 0x405
    li s4, 0x506
    li s5, 0x607
    li s6, 0x708
    li s7, 0x809
    li s8, 0x90a
    li s9, 0xa0b
    li s10, 0xb0c
    li s11, 0xc0d
    li t1, 4352
    fmv.d.x fs0, t1
    li t1, 4353
    fmv.d.x fs1, t1
    li t1, 4354
    fmv.d.x fs2, t1
    li t1, 4355
    fmv.d.x fs3, t1
    li t1, 4356
    fmv.d.x fs4, t1
    li t1, 4357
    fmv.d.x fs5, t1
    li t1, 4358
    fmv.d.x fs6, t1
    li t1, 4359
    fmv.d.x fs7, t1
    li t1, 4360
    fmv.d.x fs8, t1
    li t1, 4361
    fmv.d.x fs9, t1
    li t1, 4362
    fmv.d.x fs10, t1
    li t1, 4363
    fmv.d.x fs11, t1
    jalr t0
    la t0, swift_riscv_abi_sp
    ld t0, 0(t0)
    bne sp, t0, .Lbad_sp
    andi t1, sp, 15
    bnez t1, .Lbad
    li t1, 0x102
    bne s0, t1, .Lbad
    li t1, 0x203
    bne s1, t1, .Lbad
    li t1, 0x304
    bne s2, t1, .Lbad
    li t1, 0x405
    bne s3, t1, .Lbad
    li t1, 0x506
    bne s4, t1, .Lbad
    li t1, 0x607
    bne s5, t1, .Lbad
    li t1, 0x708
    bne s6, t1, .Lbad
    li t1, 0x809
    bne s7, t1, .Lbad
    li t1, 0x90a
    bne s8, t1, .Lbad
    li t1, 0xa0b
    bne s9, t1, .Lbad
    li t1, 0xb0c
    bne s10, t1, .Lbad
    li t1, 0xc0d
    bne s11, t1, .Lbad
    fmv.x.d t2, fs0
    li t1, 4352
    bne t2, t1, .Lbad
    fmv.x.d t2, fs1
    li t1, 4353
    bne t2, t1, .Lbad
    fmv.x.d t2, fs2
    li t1, 4354
    bne t2, t1, .Lbad
    fmv.x.d t2, fs3
    li t1, 4355
    bne t2, t1, .Lbad
    fmv.x.d t2, fs4
    li t1, 4356
    bne t2, t1, .Lbad
    fmv.x.d t2, fs5
    li t1, 4357
    bne t2, t1, .Lbad
    fmv.x.d t2, fs6
    li t1, 4358
    bne t2, t1, .Lbad
    fmv.x.d t2, fs7
    li t1, 4359
    bne t2, t1, .Lbad
    fmv.x.d t2, fs8
    li t1, 4360
    bne t2, t1, .Lbad
    fmv.x.d t2, fs9
    li t1, 4361
    bne t2, t1, .Lbad
    fmv.x.d t2, fs10
    li t1, 4362
    bne t2, t1, .Lbad
    fmv.x.d t2, fs11
    li t1, 4363
    bne t2, t1, .Lbad
    li a0, 1
    j .Lrestore
.Lbad_sp:
    mv sp, t0
.Lbad:
    li a0, 0
.Lrestore:
    ld ra, 0(sp)
    ld s0, 8(sp)
    ld s1, 16(sp)
    ld s2, 24(sp)
    ld s3, 32(sp)
    ld s4, 40(sp)
    ld s5, 48(sp)
    ld s6, 56(sp)
    ld s7, 64(sp)
    ld s8, 72(sp)
    ld s9, 80(sp)
    ld s10, 88(sp)
    ld s11, 96(sp)
    fld fs0, 104(sp)
    fld fs1, 112(sp)
    fld fs2, 120(sp)
    fld fs3, 128(sp)
    fld fs4, 136(sp)
    fld fs5, 144(sp)
    fld fs6, 152(sp)
    fld fs7, 160(sp)
    fld fs8, 168(sp)
    fld fs9, 176(sp)
    fld fs10, 184(sp)
    fld fs11, 192(sp)
    addi sp, sp, 208
    ret
    .size SwiftRiscvCheckABI, .-SwiftRiscvCheckABI
// A valid C ABI helper whose fault interrupts dirty callee-saved registers.
    .global SwiftRiscvFaultHelper
    .type SwiftRiscvFaultHelper, @function
SwiftRiscvFaultHelper:
    addi sp, sp, -208
    sd ra, 0(sp)
    sd s0, 8(sp)
    sd s1, 16(sp)
    sd s2, 24(sp)
    sd s3, 32(sp)
    sd s4, 40(sp)
    sd s5, 48(sp)
    sd s6, 56(sp)
    sd s7, 64(sp)
    sd s8, 72(sp)
    sd s9, 80(sp)
    sd s10, 88(sp)
    sd s11, 96(sp)
    fsd fs0, 104(sp)
    fsd fs1, 112(sp)
    fsd fs2, 120(sp)
    fsd fs3, 128(sp)
    fsd fs4, 136(sp)
    fsd fs5, 144(sp)
    fsd fs6, 152(sp)
    fsd fs7, 160(sp)
    fsd fs8, 168(sp)
    fsd fs9, 176(sp)
    fsd fs10, 184(sp)
    fsd fs11, 192(sp)
    li s0, 8704
    li s1, 8705
    li s2, 8706
    li s3, 8707
    li s4, 8708
    li s5, 8709
    li s6, 8710
    li s7, 8711
    li s8, 8712
    li s9, 8713
    li s10, 8714
    li s11, 8715
    li t1, 13056
    fmv.d.x fs0, t1
    li t1, 13057
    fmv.d.x fs1, t1
    li t1, 13058
    fmv.d.x fs2, t1
    li t1, 13059
    fmv.d.x fs3, t1
    li t1, 13060
    fmv.d.x fs4, t1
    li t1, 13061
    fmv.d.x fs5, t1
    li t1, 13062
    fmv.d.x fs6, t1
    li t1, 13063
    fmv.d.x fs7, t1
    li t1, 13064
    fmv.d.x fs8, t1
    li t1, 13065
    fmv.d.x fs9, t1
    li t1, 13066
    fmv.d.x fs10, t1
    li t1, 13067
    fmv.d.x fs11, t1
    ld a0, 0(a0)
    ld ra, 0(sp)
    ld s0, 8(sp)
    ld s1, 16(sp)
    ld s2, 24(sp)
    ld s3, 32(sp)
    ld s4, 40(sp)
    ld s5, 48(sp)
    ld s6, 56(sp)
    ld s7, 64(sp)
    ld s8, 72(sp)
    ld s9, 80(sp)
    ld s10, 88(sp)
    ld s11, 96(sp)
    fld fs0, 104(sp)
    fld fs1, 112(sp)
    fld fs2, 120(sp)
    fld fs3, 128(sp)
    fld fs4, 136(sp)
    fld fs5, 144(sp)
    fld fs6, 152(sp)
    fld fs7, 160(sp)
    fld fs8, 168(sp)
    fld fs9, 176(sp)
    fld fs10, 184(sp)
    fld fs11, 192(sp)
    addi sp, sp, 208
    ret
    .size SwiftRiscvFaultHelper, .-SwiftRiscvFaultHelper
    .bss
    .balign 8
swift_riscv_abi_sp:
    .quad 0
    .section .note.GNU-stack,"",@progbits
