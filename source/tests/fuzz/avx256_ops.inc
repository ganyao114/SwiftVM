// VEX.256 instruction table, shared verbatim by the Rosetta reference generator
// (avx256_rosetta_ref.c) and the SwiftVM differential case in x86_fuzz.cpp.
//
// The whole point of this file is that the two sides CANNOT drift: the generator
// encodes what this table says and runs it on real x86 hardware through Rosetta,
// and the test encodes the same table entries and runs them through SwiftVM.  If
// an opcode byte here were wrong, both sides would be wrong in the same way and
// the differential would silently pass -- so every entry below was cross-checked
// against the Intel SDM VEX.256 encodings, and the generator additionally prints
// each instruction's bytes so the encodings can be disassembled and audited.
//
// Field meaning (matches EmitVexC4 in x86_fuzz.cpp, all values UN-inverted):
//   pp     : 0 = none, 1 = 66, 2 = F3, 3 = F2
//   mmmmm  : 1 = implied 0F, 2 = 0F38, 3 = 0F3A
//   opcode : the opcode byte
// VEX.W is 0 and VEX.L is 1 for every entry here.
//
// Two macros; a consumer defines the ones it cares about and leaves the other
// defined-empty.
//
//   SVM_AVX256_ALU(name, pp, mmmmm, opcode)
//       Three-operand non-destructive form: dst = src1 OP src2, where src1 is
//       VEX.vvvv and src2 is the r/m operand.  Both the reg-reg and the
//       reg-mem shape are exercised; they must produce the identical result.
//
//   SVM_AVX256_MOV(name, pp, mmmmm, ld_opcode, st_opcode)
//       Two-operand data movement.  0xFF means that direction does not exist
//       (the non-temporal stores are store-only, vlddqu is load-only).
//
// vpmovmskb, vpshufb and vbroadcastss are NOT in these macro lists where their
// shape differs: vpshufb IS a plain three-operand ALU form and is listed as
// such, but vpmovmskb (GPR destination) and vbroadcastss (xmm source) are
// hand-written on both sides because they have no other instance of their shape.

#ifndef SVM_AVX256_ALU
#define SVM_AVX256_ALU(name, pp, mmmmm, opcode)
#endif
#ifndef SVM_AVX256_MOV
#define SVM_AVX256_MOV(name, pp, mmmmm, ld_opcode, st_opcode)
#endif

// ---- bitwise: the integer forms and their FP-typed aliases ----------------
// vandnps/vandnpd/vpandn are all "NOT(src1) AND src2", i.e. src1 is inverted,
// which is the operand order the split implementation is most likely to get
// backwards; every input pair below is chosen so A OP B != B OP A.
SVM_AVX256_ALU(vpxor, 1, 1, 0xEF)
SVM_AVX256_ALU(vpor, 1, 1, 0xEB)
SVM_AVX256_ALU(vpand, 1, 1, 0xDB)
SVM_AVX256_ALU(vpandn, 1, 1, 0xDF)
SVM_AVX256_ALU(vxorps, 0, 1, 0x57)
SVM_AVX256_ALU(vxorpd, 1, 1, 0x57)
SVM_AVX256_ALU(vorps, 0, 1, 0x56)
SVM_AVX256_ALU(vorpd, 1, 1, 0x56)
SVM_AVX256_ALU(vandps, 0, 1, 0x54)
SVM_AVX256_ALU(vandpd, 1, 1, 0x54)
SVM_AVX256_ALU(vandnps, 0, 1, 0x55)
SVM_AVX256_ALU(vandnpd, 1, 1, 0x55)

// ---- lane-wise integer add / sub -----------------------------------------
SVM_AVX256_ALU(vpaddb, 1, 1, 0xFC)
SVM_AVX256_ALU(vpaddw, 1, 1, 0xFD)
SVM_AVX256_ALU(vpaddd, 1, 1, 0xFE)
SVM_AVX256_ALU(vpaddq, 1, 1, 0xD4)
SVM_AVX256_ALU(vpsubb, 1, 1, 0xF8)
SVM_AVX256_ALU(vpsubw, 1, 1, 0xF9)
SVM_AVX256_ALU(vpsubd, 1, 1, 0xFA)
SVM_AVX256_ALU(vpsubq, 1, 1, 0xFB)

// ---- compares (signed for vpcmpgt*) --------------------------------------
SVM_AVX256_ALU(vpcmpeqb, 1, 1, 0x74)
SVM_AVX256_ALU(vpcmpeqw, 1, 1, 0x75)
SVM_AVX256_ALU(vpcmpeqd, 1, 1, 0x76)
SVM_AVX256_ALU(vpcmpgtb, 1, 1, 0x64)
SVM_AVX256_ALU(vpcmpgtw, 1, 1, 0x65)
SVM_AVX256_ALU(vpcmpgtd, 1, 1, 0x66)

// ---- unsigned min / max --------------------------------------------------
SVM_AVX256_ALU(vpminub, 1, 1, 0xDA)
SVM_AVX256_ALU(vpmaxub, 1, 1, 0xDE)
SVM_AVX256_ALU(vpminud, 1, 2, 0x3B)
SVM_AVX256_ALU(vpmaxud, 1, 2, 0x3F)

// ---- byte shuffle --------------------------------------------------------
// THE entry this whole exercise exists to settle.  AVX2 vpshufb is defined
// per 128-bit lane: only bits [3:0] of a control byte index, bit 7 zeroes the
// output byte, and bits [6:4] are IGNORED -- so a control byte of 0x1F selects
// byte 15 of its OWN lane, not byte 31 of the register.  decoder_avx.cc
// implements it as two independent VecTableLookup8 calls, which is only
// correct if that reading is right.  Input pair 3 puts control values 0x10-0x1F
// in the low lane precisely so a cross-lane implementation produces different
// bytes and gets caught.
SVM_AVX256_ALU(vpshufb, 1, 2, 0x00)

// ---- data movement -------------------------------------------------------
// Load and store opcodes for the same mnemonic; 0xFF = direction absent.
SVM_AVX256_MOV(vmovdqu, 2, 1, 0x6F, 0x7F)
SVM_AVX256_MOV(vmovdqa, 1, 1, 0x6F, 0x7F)
SVM_AVX256_MOV(vmovups, 0, 1, 0x10, 0x11)
SVM_AVX256_MOV(vmovupd, 1, 1, 0x10, 0x11)
SVM_AVX256_MOV(vmovaps, 0, 1, 0x28, 0x29)
SVM_AVX256_MOV(vmovapd, 1, 1, 0x28, 0x29)
SVM_AVX256_MOV(vmovntps, 0, 1, 0xFF, 0x2B)
SVM_AVX256_MOV(vmovntpd, 1, 1, 0xFF, 0x2B)
SVM_AVX256_MOV(vmovntdq, 1, 1, 0xFF, 0xE7)
SVM_AVX256_MOV(vlddqu, 3, 1, 0xF0, 0xFF)

#undef SVM_AVX256_ALU
#undef SVM_AVX256_MOV
