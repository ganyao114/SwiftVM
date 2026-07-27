// Second-wave VEX instruction table: the scalar moves, scalar conversions,
// shuffles, lane moves and the VEX.128 twins added to
// source/runtime/frontend/x86/decoder_avx_fp.cc.
//
// Shared VERBATIM by the Rosetta reference generator (avx_fp2_rosetta_ref.c)
// and the SwiftVM differential case (avx_fp2_test.cpp).
//
// HOW THIS TABLE DIFFERS FROM avx_fp_ops.inc
// ------------------------------------------
// In the first wave both programs BUILT the encoding from the table, so a
// wrong field here made the two sides test the same wrong instruction and the
// differential passed vacuously.  Here only the GENERATOR encodes; every
// reference row carries the literal instruction BYTES it executed, and the
// test replays those bytes.  Encoder drift between the two sides is therefore
// structurally impossible rather than merely unlikely, and this table's job
// shrinks to (a) naming what the generator must cover and (b) letting the test
// assert that every named mnemonic actually produced rows.
//
// Field meaning (all values UN-inverted):
//   shape  : how the generator builds the operands -- see the S_* list below
//   map    : 1 = 0F, 2 = 0F38, 3 = 0F3A
//   pp     : 0 = none, 1 = 66, 2 = F3, 3 = F2
//   w      : VEX.W
//   opcode : the opcode byte
//   aux    : shape-dependent -- imm8, ModRM.reg group number, or 0
//
// SHAPES
//   S_BIN    dst = ymm0, VEX.vvvv = ymm1, r/m = ymm2 or [rdi+B].  Both VEX.L,
//            both operand shapes.  `aux` is an imm8 when the opcode has one.
//   S_BINR   S_BIN, VEX.128, register r/m only (vmovlhps / vmovhlps).
//   S_BIN256 S_BIN, VEX.256 only (vperm2f128 is #UD at VEX.L=0).
//   S_UN     dst = ymm0, no vvvv, r/m = ymm1 or [rdi+A].  Both VEX.L.
//   S_UNM    S_UN, memory r/m only (vbroadcastsd / vbroadcastf128).
//   S_REV    ModRM.reg is the SOURCE and r/m the DESTINATION (vextractf128).
//   S_MOVS   vmovss / vmovsd: all four forms of opcodes 10 and 11 -- the
//            three-operand merge at 10 /r, the SAME merge with dst and src2
//            swapped at 11 /r, the zeroing load, and the store.
//   S_MOVLH  vmovlps / vmovhps / vmovlpd / vmovhpd load and store forms.
//   S_SI2F   vcvtsi2ss / vcvtsi2sd: the r/m operand is a GENERAL-PURPOSE
//            register (or memory), which no other shape here has.
//   S_F2SI   vcvtss2si / vcvttss2si / ...: destination is a GPR.
//   S_MSK    vpmovmskb: GPR destination, vector source, no memory form.
//   S_NDD    vpsrldq / vpslldq: the DESTINATION is VEX.vvvv and the source is
//            ModRM.rm -- the reverse of every other shape.  `aux` is the /n
//            group number; the imm8 is swept separately.
//   S_TEST   vptest: the answer is EFLAGS, captured with five SETcc.
//   S_EXTR   vpextrb / vpextrw / vpextrd / vpextrq.
//   S_INSR   vpinsrb / vpinsrw / vpinsrd / vpinsrq.
//   S_INS128 vinsertf128.
//
// Every register-destination row is run against a POISONED ymm0 and the whole
// 32 bytes are read back, so contract C3 (a VEX.128 write zeroes bits 255:128)
// is MEASURED on hardware for each one rather than assumed.  For the scalar
// moves that is the main event: `vmovss xmm0, [rax]` zeroes 28 bytes above the
// dword it loads, and nothing short of reading the YMM back can see it.

#ifndef SVM_FP2
#define SVM_FP2(name, shape, map, pp, w, opcode, aux)
#endif

// ---- vmovss / vmovsd ------------------------------------------------------
// The single most likely first casualty of an AVX-enabled guest: every scalar
// float load, store and register copy a compiler emits.
SVM_FP2(vmovss, S_MOVS, 1, 2, 0, 0x10, 32)
SVM_FP2(vmovsd, S_MOVS, 1, 3, 0, 0x10, 64)

// ---- scalar conversions ---------------------------------------------------
SVM_FP2(vcvtss2sd, S_BIN, 1, 2, 0, 0x5A, -1)
SVM_FP2(vcvtsd2ss, S_BIN, 1, 3, 0, 0x5A, -1)
SVM_FP2(vcvtsi2ss_w0, S_SI2F, 1, 2, 0, 0x2A, 0)
SVM_FP2(vcvtsi2ss_w1, S_SI2F, 1, 2, 1, 0x2A, 0)
SVM_FP2(vcvtsi2sd_w0, S_SI2F, 1, 3, 0, 0x2A, 0)
SVM_FP2(vcvtsi2sd_w1, S_SI2F, 1, 3, 1, 0x2A, 0)
SVM_FP2(vcvttss2si_w0, S_F2SI, 1, 2, 0, 0x2C, 0)
SVM_FP2(vcvttss2si_w1, S_F2SI, 1, 2, 1, 0x2C, 0)
SVM_FP2(vcvttsd2si_w0, S_F2SI, 1, 3, 0, 0x2C, 0)
SVM_FP2(vcvttsd2si_w1, S_F2SI, 1, 3, 1, 0x2C, 0)
SVM_FP2(vcvtss2si_w0, S_F2SI, 1, 2, 0, 0x2D, 0)
SVM_FP2(vcvtss2si_w1, S_F2SI, 1, 2, 1, 0x2D, 0)
SVM_FP2(vcvtsd2si_w0, S_F2SI, 1, 3, 0, 0x2D, 0)
SVM_FP2(vcvtsd2si_w1, S_F2SI, 1, 3, 1, 0x2D, 0)
// The 0xE6 trio.  vcvtdq2pd widens (source is half the destination); the other
// two narrow, and their destination is an XMM at BOTH VEX.L values -- so C3
// applies to the 256-bit encoding too, which is easy to get wrong.
SVM_FP2(vcvtdq2pd, S_UN, 1, 2, 0, 0xE6, -1)
SVM_FP2(vcvttpd2dq, S_UN, 1, 1, 0, 0xE6, -1)
SVM_FP2(vcvtpd2dq, S_UN, 1, 3, 0, 0xE6, -1)

// ---- the six VEX.128 twins ------------------------------------------------
// Their 256-bit forms already worked; only the 128-bit ones were fatal.  Both
// widths are exercised so the pair can no longer drift apart.
SVM_FP2(vpmovmskb, S_MSK, 1, 1, 0, 0xD7, 0)
SVM_FP2(vpshufb, S_BIN, 2, 1, 0, 0x00, -1)
SVM_FP2(vpminub, S_BIN, 1, 1, 0, 0xDA, -1)
SVM_FP2(vpmaxub, S_BIN, 1, 1, 0, 0xDE, -1)
SVM_FP2(vpminud, S_BIN, 2, 1, 0, 0x3B, -1)
SVM_FP2(vpmaxud, S_BIN, 2, 1, 0, 0x3F, -1)

// ---- shuffles and unpacks -------------------------------------------------
// vshufps/vshufpd take several imm8 values because the failure mode is a
// mis-split bit field, which one control value can easily not expose.  For
// vshufpd the 256-bit form uses DIFFERENT imm8 bits for its upper lane
// (3:2 rather than 1:0), so imm values that differ between nibbles matter.
SVM_FP2(vshufps, S_BIN, 1, 0, 0, 0xC6, 0x1B)
SVM_FP2(vshufps, S_BIN, 1, 0, 0, 0xC6, 0xE4)
SVM_FP2(vshufps, S_BIN, 1, 0, 0, 0xC6, 0x00)
SVM_FP2(vshufps, S_BIN, 1, 0, 0, 0xC6, 0xFF)
SVM_FP2(vshufps, S_BIN, 1, 0, 0, 0xC6, 0x27)
SVM_FP2(vshufps, S_BIN, 1, 0, 0, 0xC6, 0x93)
SVM_FP2(vshufpd, S_BIN, 1, 1, 0, 0xC6, 0x00)
SVM_FP2(vshufpd, S_BIN, 1, 1, 0, 0xC6, 0x01)
SVM_FP2(vshufpd, S_BIN, 1, 1, 0, 0xC6, 0x02)
SVM_FP2(vshufpd, S_BIN, 1, 1, 0, 0xC6, 0x03)
SVM_FP2(vshufpd, S_BIN, 1, 1, 0, 0xC6, 0x05)
SVM_FP2(vshufpd, S_BIN, 1, 1, 0, 0xC6, 0x0A)
SVM_FP2(vshufpd, S_BIN, 1, 1, 0, 0xC6, 0x0F)
SVM_FP2(vunpcklps, S_BIN, 1, 0, 0, 0x14, -1)
SVM_FP2(vunpckhps, S_BIN, 1, 0, 0, 0x15, -1)
SVM_FP2(vunpcklpd, S_BIN, 1, 1, 0, 0x14, -1)
SVM_FP2(vunpckhpd, S_BIN, 1, 1, 0, 0x15, -1)
// vmovlhps takes src2's LOW qword to dst's HIGH; vmovhlps takes src2's HIGH to
// dst's LOW.  The names are the opposite way round from the opcodes, which is
// exactly why both are here.
SVM_FP2(vmovlhps, S_BINR, 1, 0, 0, 0x16, -1)
SVM_FP2(vmovhlps, S_BINR, 1, 0, 0, 0x12, -1)
SVM_FP2(vmovlps, S_MOVLH, 1, 0, 0, 0x12, 0)
SVM_FP2(vmovhps, S_MOVLH, 1, 0, 0, 0x16, 0)
SVM_FP2(vmovlpd, S_MOVLH, 1, 1, 0, 0x12, 0)
SVM_FP2(vmovhpd, S_MOVLH, 1, 1, 0, 0x16, 0)
// vmovddup at VEX.256 duplicates each 128-bit lane's OWN low qword, so its
// upper half comes from the source's bits 191:128 -- not a broadcast.
SVM_FP2(vmovddup, S_UN, 1, 3, 0, 0x12, -1)
SVM_FP2(vmovsldup, S_UN, 1, 2, 0, 0x12, -1)
SVM_FP2(vmovshdup, S_UN, 1, 2, 0, 0x16, -1)

// ---- broadcasts and 128-bit lane moves ------------------------------------
SVM_FP2(vbroadcastss, S_UN, 2, 1, 0, 0x18, -1)
SVM_FP2(vbroadcastsd, S_UNM, 2, 1, 0, 0x19, -1)
SVM_FP2(vbroadcastf128, S_UNM, 2, 1, 0, 0x1A, -1)
SVM_FP2(vinsertf128, S_INS128, 3, 1, 0, 0x18, 0)
SVM_FP2(vinsertf128, S_INS128, 3, 1, 0, 0x18, 1)
SVM_FP2(vextractf128, S_REV, 3, 1, 0, 0x19, 0)
SVM_FP2(vextractf128, S_REV, 3, 1, 0, 0x19, 1)
SVM_FP2(vperm2f128, S_BIN256, 3, 1, 0, 0x06, 0x20)
SVM_FP2(vperm2f128, S_BIN256, 3, 1, 0, 0x06, 0x31)
SVM_FP2(vperm2f128, S_BIN256, 3, 1, 0, 0x06, 0x02)
SVM_FP2(vperm2f128, S_BIN256, 3, 1, 0, 0x06, 0x13)
SVM_FP2(vperm2f128, S_BIN256, 3, 1, 0, 0x06, 0x88)

// ---- byte shifts of the whole 128-bit lane --------------------------------
// NDD: destination is VEX.vvvv, source is ModRM.rm.  Counts 0, 1, 7, 8, 15,
// 16 and 17 cover both ends including the "everything shifted out" cases.
SVM_FP2(vpsrldq, S_NDD, 1, 1, 0, 0x73, 3)
SVM_FP2(vpslldq, S_NDD, 1, 1, 0, 0x73, 7)

// ---- vpermilps / vpermilpd ------------------------------------------------
SVM_FP2(vpermilps_var, S_BIN, 2, 1, 0, 0x0C, -1)
SVM_FP2(vpermilps, S_UN, 3, 1, 0, 0x04, 0x1B)
SVM_FP2(vpermilps, S_UN, 3, 1, 0, 0x04, 0x93)
SVM_FP2(vpermilps, S_UN, 3, 1, 0, 0x04, 0x00)
SVM_FP2(vpermilpd, S_UN, 3, 1, 0, 0x05, 0x00)
SVM_FP2(vpermilpd, S_UN, 3, 1, 0, 0x05, 0x01)
SVM_FP2(vpermilpd, S_UN, 3, 1, 0, 0x05, 0x06)
SVM_FP2(vpermilpd, S_UN, 3, 1, 0, 0x05, 0x0F)

// ---- 64-bit integer compares ----------------------------------------------
SVM_FP2(vpcmpeqq, S_BIN, 2, 1, 0, 0x29, -1)
SVM_FP2(vpcmpgtq, S_BIN, 2, 1, 0, 0x37, -1)

// ---- vptest ---------------------------------------------------------------
// ZF and CF are whole-register properties, so all four (ZF, CF) combinations
// need different INPUT PAIRS rather than different encodings -- the pair set
// in avx_fp2_rosetta_ref.c has dedicated disjoint / subset / zero entries.
SVM_FP2(vptest, S_TEST, 2, 1, 0, 0x17, 0)

// ---- element insert / extract ---------------------------------------------
SVM_FP2(vpextrb, S_EXTR, 3, 1, 0, 0x14, 8)
SVM_FP2(vpextrw, S_EXTR, 1, 1, 0, 0xC5, 16)
SVM_FP2(vpextrd, S_EXTR, 3, 1, 0, 0x16, 32)
SVM_FP2(vpextrq, S_EXTR, 3, 1, 1, 0x16, 64)
SVM_FP2(vpinsrb, S_INSR, 3, 1, 0, 0x20, 8)
SVM_FP2(vpinsrw, S_INSR, 1, 1, 0, 0xC4, 16)
SVM_FP2(vpinsrd, S_INSR, 3, 1, 0, 0x22, 32)
SVM_FP2(vpinsrq, S_INSR, 3, 1, 1, 0x22, 64)

#undef SVM_FP2
