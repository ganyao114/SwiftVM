// Serial LP64D sentinel check for a generated block and its helper calls.
    .text
    .align 4
    .globl SwiftLoongArchCheckABI
    .type SwiftLoongArchCheckABI, @function
SwiftLoongArchCheckABI:
    addi.d $r3, $r3, -176
    st.d $r1, $r3, 0
    st.d $r22, $r3, 8
    st.d $r23, $r3, 16
    st.d $r24, $r3, 24
    st.d $r25, $r3, 32
    st.d $r26, $r3, 40
    st.d $r27, $r3, 48
    st.d $r28, $r3, 56
    st.d $r29, $r3, 64
    st.d $r30, $r3, 72
    st.d $r31, $r3, 80
    fst.d $f24, $r3, 88
    fst.d $f25, $r3, 96
    fst.d $f26, $r3, 104
    fst.d $f27, $r3, 112
    fst.d $f28, $r3, 120
    fst.d $f29, $r3, 128
    fst.d $f30, $r3, 136
    fst.d $f31, $r3, 144
    movfcsr2gr $r13, $fcsr0
    st.w $r13, $r3, 152
    la.pcrel $r12, swift_loongarch_abi_sp
    st.d $r3, $r12, 0
    move $r12, $r4
    move $r4, $r5
    li.d $r22, 1192960
    li.d $r23, 1193217
    li.d $r24, 1193474
    li.d $r25, 1193731
    li.d $r26, 1193988
    li.d $r27, 1194245
    li.d $r28, 1194502
    li.d $r29, 1194759
    li.d $r30, 1195016
    li.d $r31, 1195273
    li.d $r13, 4412672
    movgr2fr.d $f24, $r13
    li.d $r13, 4412929
    movgr2fr.d $f25, $r13
    li.d $r13, 4413186
    movgr2fr.d $f26, $r13
    li.d $r13, 4413443
    movgr2fr.d $f27, $r13
    li.d $r13, 4413700
    movgr2fr.d $f28, $r13
    li.d $r13, 4413957
    movgr2fr.d $f29, $r13
    li.d $r13, 4414214
    movgr2fr.d $f30, $r13
    li.d $r13, 4414471
    movgr2fr.d $f31, $r13
    jirl $r1, $r12, 0
    la.pcrel $r12, swift_loongarch_abi_sp
    ld.d $r12, $r12, 0
    bne $r3, $r12, .Lbad_sp
    andi $r13, $r3, 15
    bne $r13, $r0, .Lbad
    li.d $r13, 1192960
    bne $r22, $r13, .Lbad
    li.d $r13, 1193217
    bne $r23, $r13, .Lbad
    li.d $r13, 1193474
    bne $r24, $r13, .Lbad
    li.d $r13, 1193731
    bne $r25, $r13, .Lbad
    li.d $r13, 1193988
    bne $r26, $r13, .Lbad
    li.d $r13, 1194245
    bne $r27, $r13, .Lbad
    li.d $r13, 1194502
    bne $r28, $r13, .Lbad
    li.d $r13, 1194759
    bne $r29, $r13, .Lbad
    li.d $r13, 1195016
    bne $r30, $r13, .Lbad
    li.d $r13, 1195273
    bne $r31, $r13, .Lbad
    movfr2gr.d $r14, $f24
    li.d $r13, 4412672
    bne $r14, $r13, .Lbad
    movfr2gr.d $r14, $f25
    li.d $r13, 4412929
    bne $r14, $r13, .Lbad
    movfr2gr.d $r14, $f26
    li.d $r13, 4413186
    bne $r14, $r13, .Lbad
    movfr2gr.d $r14, $f27
    li.d $r13, 4413443
    bne $r14, $r13, .Lbad
    movfr2gr.d $r14, $f28
    li.d $r13, 4413700
    bne $r14, $r13, .Lbad
    movfr2gr.d $r14, $f29
    li.d $r13, 4413957
    bne $r14, $r13, .Lbad
    movfr2gr.d $r14, $f30
    li.d $r13, 4414214
    bne $r14, $r13, .Lbad
    movfr2gr.d $r14, $f31
    li.d $r13, 4414471
    bne $r14, $r13, .Lbad
    movfcsr2gr $r14, $fcsr0
    ld.wu $r13, $r3, 152
    bne $r14, $r13, .Lbad
    addi.d $r4, $r0, 1
    b .Lrestore
.Lbad_sp:
    move $r3, $r12
.Lbad:
    move $r4, $r0
.Lrestore:
    ld.wu $r12, $r3, 152
    movgr2fcsr $fcsr0, $r12
    ld.d $r1, $r3, 0
    ld.d $r22, $r3, 8
    ld.d $r23, $r3, 16
    ld.d $r24, $r3, 24
    ld.d $r25, $r3, 32
    ld.d $r26, $r3, 40
    ld.d $r27, $r3, 48
    ld.d $r28, $r3, 56
    ld.d $r29, $r3, 64
    ld.d $r30, $r3, 72
    ld.d $r31, $r3, 80
    fld.d $f24, $r3, 88
    fld.d $f25, $r3, 96
    fld.d $f26, $r3, 104
    fld.d $f27, $r3, 112
    fld.d $f28, $r3, 120
    fld.d $f29, $r3, 128
    fld.d $f30, $r3, 136
    fld.d $f31, $r3, 144
    addi.d $r3, $r3, 176
    jirl $r0, $r1, 0
    .size SwiftLoongArchCheckABI, .-SwiftLoongArchCheckABI
// A valid helper dirties every callee-saved bank before a synchronous fault.
    .globl SwiftLoongArchFaultHelper
    .type SwiftLoongArchFaultHelper, @function
SwiftLoongArchFaultHelper:
    addi.d $r3, $r3, -176
    st.d $r1, $r3, 0
    st.d $r22, $r3, 8
    st.d $r23, $r3, 16
    st.d $r24, $r3, 24
    st.d $r25, $r3, 32
    st.d $r26, $r3, 40
    st.d $r27, $r3, 48
    st.d $r28, $r3, 56
    st.d $r29, $r3, 64
    st.d $r30, $r3, 72
    st.d $r31, $r3, 80
    fst.d $f24, $r3, 88
    fst.d $f25, $r3, 96
    fst.d $f26, $r3, 104
    fst.d $f27, $r3, 112
    fst.d $f28, $r3, 120
    fst.d $f29, $r3, 128
    fst.d $f30, $r3, 136
    fst.d $f31, $r3, 144
    li.d $r22, 7829248
    li.d $r23, 7829505
    li.d $r24, 7829762
    li.d $r25, 7830019
    li.d $r26, 7830276
    li.d $r27, 7830533
    li.d $r28, 7830790
    li.d $r29, 7831047
    li.d $r30, 7831304
    li.d $r31, 7831561
    li.d $r13, 8947712
    movgr2fr.d $f24, $r13
    li.d $r13, 8947969
    movgr2fr.d $f25, $r13
    li.d $r13, 8948226
    movgr2fr.d $f26, $r13
    li.d $r13, 8948483
    movgr2fr.d $f27, $r13
    li.d $r13, 8948740
    movgr2fr.d $f28, $r13
    li.d $r13, 8948997
    movgr2fr.d $f29, $r13
    li.d $r13, 8949254
    movgr2fr.d $f30, $r13
    li.d $r13, 8949511
    movgr2fr.d $f31, $r13
    ld.d $r4, $r4, 0
    ld.d $r1, $r3, 0
    ld.d $r22, $r3, 8
    ld.d $r23, $r3, 16
    ld.d $r24, $r3, 24
    ld.d $r25, $r3, 32
    ld.d $r26, $r3, 40
    ld.d $r27, $r3, 48
    ld.d $r28, $r3, 56
    ld.d $r29, $r3, 64
    ld.d $r30, $r3, 72
    ld.d $r31, $r3, 80
    fld.d $f24, $r3, 88
    fld.d $f25, $r3, 96
    fld.d $f26, $r3, 104
    fld.d $f27, $r3, 112
    fld.d $f28, $r3, 120
    fld.d $f29, $r3, 128
    fld.d $f30, $r3, 136
    fld.d $f31, $r3, 144
    addi.d $r3, $r3, 176
    jirl $r0, $r1, 0
    .size SwiftLoongArchFaultHelper, .-SwiftLoongArchFaultHelper
    .bss
    .align 3
swift_loongarch_abi_sp:
    .dword 0
    .section .note.GNU-stack,"",@progbits
