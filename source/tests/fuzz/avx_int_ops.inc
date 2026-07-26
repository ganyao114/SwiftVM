// VEX integer / data-rearrangement instruction table, shared VERBATIM by the
// Rosetta reference generator (avx_int_rosetta_ref.c) and the SwiftVM
// differential case (avx_int_test.cpp).
//
// The point of the shared table is that the two sides cannot drift.  The
// generator encodes what this table says and executes it on real x86-64
// hardware through Rosetta 2; the test encodes the same entries and runs them
// through SwiftVM.  A wrong opcode byte here would make both sides test the
// same wrong instruction and the differential would pass vacuously -- which is
// exactly why the generator also has a --dump-encodings mode that prints every
// instruction's bytes for disassembly against an independent decoder.
//
// Field meaning (all values UN-inverted, matching vex3()/EmitVexC4):
//   pp     : 0 = none, 1 = 66, 2 = F3, 3 = F2
//   mm     : 1 = implied 0F, 2 = 0F38, 3 = 0F3A
//   op     : the opcode byte
//   w      : VEX.W
//   lmask  : 1 = VEX.128 only, 2 = VEX.256 only, 3 = both widths exist
//   imm    : the imm8 byte for the forms that carry one
//   ext    : ModRM.reg opcode extension (the /n of the shift-by-imm groups)
//   sb128  : bytes the MEMORY form reads at VEX.128 (0 = the full 16)
//   sb256  : bytes the MEMORY form reads at VEX.256 (0 = the full 32)
//
// Operand assignment is fixed for every shape so the generator and the test
// build byte-identical encodings:
//   dst  = ymm0/xmm0      src1 = ymm1/xmm1 (= input A)
//   src2 = ymm2/xmm2 (= input B)            mask = ymm3/xmm3 (= the fixed mask)
//
// SHAPES
// ------
//   SVM_AVX_INT_ALU(name, pp, mm, op, w, lmask)
//       dst = src1 OP src2.  ModRM.reg = dst, VEX.vvvv = src1, r/m = src2.
//       The r/m operand is the FULL width of the instruction.
//
//   SVM_AVX_INT_SHIFTX(name, pp, mm, op, lmask)
//       Same encoding shape as ALU, but the r/m operand is an xmm/m128 SHIFT
//       COUNT even at VEX.256 -- the count is the low qword of a 128-bit
//       operand, not a 256-bit one.  Split out from ALU precisely because the
//       memory form reads 16 bytes rather than 32; folding the two together is
//       how a 256-bit read of a 128-bit operand gets introduced.
//
//   SVM_AVX_INT_SHIFTI(name, pp, mm, op, ext, imm, lmask)
//       Shift by imm8.  These are NDD forms: the DESTINATION is VEX.vvvv, the
//       source is r/m, and ModRM.reg carries the /n opcode extension.  Getting
//       that backwards silently swaps source and destination.
//
//   SVM_AVX_INT_UN(name, pp, mm, op, w, lmask, sb128, sb256)
//       dst = f(src2).  Two operands, no VEX.vvvv (encoded 1111).  r/m is the
//       source; sb128/sb256 give its width in bytes, which is NARROWER than
//       the destination for the vpmovsx/vpmovzx and vpbroadcast families.
//
//   SVM_AVX_INT_ALUI(name, pp, mm, op, w, imm, lmask)
//       dst = f(src1, src2, imm8).
//
//   SVM_AVX_INT_UNI(name, pp, mm, op, w, imm, lmask)
//       dst = f(src2, imm8).  No VEX.vvvv.
//
// Four instructions have shapes that occur exactly once each and are therefore
// hand-written on both sides rather than given a single-use macro:
// vpblendvb (/is4 register operand), vinserti128 (128-bit r/m into a 256-bit
// result), vextracti128 (the DESTINATION is r/m) and vbroadcasti128 (memory
// operand only; the register form is #UD).

#ifndef SVM_AVX_INT_ALU
#define SVM_AVX_INT_ALU(name, pp, mm, op, w, lmask)
#endif
#ifndef SVM_AVX_INT_SHIFTX
#define SVM_AVX_INT_SHIFTX(name, pp, mm, op, lmask)
#endif
#ifndef SVM_AVX_INT_SHIFTI
#define SVM_AVX_INT_SHIFTI(name, pp, mm, op, ext, imm, lmask)
#endif
#ifndef SVM_AVX_INT_UN
#define SVM_AVX_INT_UN(name, pp, mm, op, w, lmask, sb128, sb256)
#endif
#ifndef SVM_AVX_INT_ALUI
#define SVM_AVX_INT_ALUI(name, pp, mm, op, w, imm, lmask)
#endif
#ifndef SVM_AVX_INT_UNI
#define SVM_AVX_INT_UNI(name, pp, mm, op, w, imm, lmask)
#endif

// ---- multiply -------------------------------------------------------------
SVM_AVX_INT_ALU(vpmullw, 1, 1, 0xD5, 0, 3)
SVM_AVX_INT_ALU(vpmulhw, 1, 1, 0xE5, 0, 3)
SVM_AVX_INT_ALU(vpmulhuw, 1, 1, 0xE4, 0, 3)
SVM_AVX_INT_ALU(vpmulld, 1, 2, 0x40, 0, 3)

// ---- horizontal -----------------------------------------------------------
// vpmaddwd and vpsadbw both reduce across lanes WITHIN a 128-bit lane, so the
// two-halves split is exact; the reference data proves that rather than
// assuming it.
SVM_AVX_INT_ALU(vpmaddwd, 1, 1, 0xF5, 0, 3)
SVM_AVX_INT_ALU(vpsadbw, 1, 1, 0xF6, 0, 3)
SVM_AVX_INT_ALU(vpavgb, 1, 1, 0xE0, 0, 3)
SVM_AVX_INT_ALU(vpavgw, 1, 1, 0xE3, 0, 3)

// ---- saturating add / sub -------------------------------------------------
SVM_AVX_INT_ALU(vpaddsb, 1, 1, 0xEC, 0, 3)
SVM_AVX_INT_ALU(vpaddsw, 1, 1, 0xED, 0, 3)
SVM_AVX_INT_ALU(vpaddusb, 1, 1, 0xDC, 0, 3)
SVM_AVX_INT_ALU(vpaddusw, 1, 1, 0xDD, 0, 3)
SVM_AVX_INT_ALU(vpsubsb, 1, 1, 0xE8, 0, 3)
SVM_AVX_INT_ALU(vpsubsw, 1, 1, 0xE9, 0, 3)
SVM_AVX_INT_ALU(vpsubusb, 1, 1, 0xD8, 0, 3)
SVM_AVX_INT_ALU(vpsubusw, 1, 1, 0xD9, 0, 3)

// ---- signed min / max (the unsigned b/d forms already exist) --------------
SVM_AVX_INT_ALU(vpminsb, 1, 2, 0x38, 0, 3)
SVM_AVX_INT_ALU(vpminsw, 1, 1, 0xEA, 0, 3)
SVM_AVX_INT_ALU(vpminsd, 1, 2, 0x39, 0, 3)
SVM_AVX_INT_ALU(vpmaxsb, 1, 2, 0x3C, 0, 3)
SVM_AVX_INT_ALU(vpmaxsw, 1, 1, 0xEE, 0, 3)
SVM_AVX_INT_ALU(vpmaxsd, 1, 2, 0x3D, 0, 3)
SVM_AVX_INT_ALU(vpminuw, 1, 2, 0x3A, 0, 3)
SVM_AVX_INT_ALU(vpmaxuw, 1, 2, 0x3E, 0, 3)

// ---- pack (narrow with saturation) and unpack (interleave) ---------------
SVM_AVX_INT_ALU(vpacksswb, 1, 1, 0x63, 0, 3)
SVM_AVX_INT_ALU(vpackssdw, 1, 1, 0x6B, 0, 3)
SVM_AVX_INT_ALU(vpackuswb, 1, 1, 0x67, 0, 3)
SVM_AVX_INT_ALU(vpackusdw, 1, 2, 0x2B, 0, 3)
SVM_AVX_INT_ALU(vpunpcklbw, 1, 1, 0x60, 0, 3)
SVM_AVX_INT_ALU(vpunpcklwd, 1, 1, 0x61, 0, 3)
SVM_AVX_INT_ALU(vpunpckldq, 1, 1, 0x62, 0, 3)
SVM_AVX_INT_ALU(vpunpcklqdq, 1, 1, 0x6C, 0, 3)
SVM_AVX_INT_ALU(vpunpckhbw, 1, 1, 0x68, 0, 3)
SVM_AVX_INT_ALU(vpunpckhwd, 1, 1, 0x69, 0, 3)
SVM_AVX_INT_ALU(vpunpckhdq, 1, 1, 0x6A, 0, 3)
SVM_AVX_INT_ALU(vpunpckhqdq, 1, 1, 0x6D, 0, 3)

// ---- sign / absolute value -----------------------------------------------
SVM_AVX_INT_ALU(vpsignb, 1, 2, 0x08, 0, 3)
SVM_AVX_INT_ALU(vpsignw, 1, 2, 0x09, 0, 3)
SVM_AVX_INT_ALU(vpsignd, 1, 2, 0x0A, 0, 3)
SVM_AVX_INT_UN(vpabsb, 1, 2, 0x1C, 0, 3, 0, 0)
SVM_AVX_INT_UN(vpabsw, 1, 2, 0x1D, 0, 3, 0, 0)
SVM_AVX_INT_UN(vpabsd, 1, 2, 0x1E, 0, 3, 0, 0)

// ---- AVX2 per-lane variable shifts ---------------------------------------
// Each lane shifts by its OWN count.  These are the only shifts in the family
// whose count is not shared across the register.
SVM_AVX_INT_ALU(vpsllvd, 1, 2, 0x47, 0, 3)
SVM_AVX_INT_ALU(vpsllvq, 1, 2, 0x47, 1, 3)
SVM_AVX_INT_ALU(vpsrlvd, 1, 2, 0x45, 0, 3)
SVM_AVX_INT_ALU(vpsrlvq, 1, 2, 0x45, 1, 3)
SVM_AVX_INT_ALU(vpsravd, 1, 2, 0x46, 0, 3)

// ---- shift by an xmm/m128 count ------------------------------------------
SVM_AVX_INT_SHIFTX(vpsllw, 1, 1, 0xF1, 3)
SVM_AVX_INT_SHIFTX(vpslld, 1, 1, 0xF2, 3)
SVM_AVX_INT_SHIFTX(vpsllq, 1, 1, 0xF3, 3)
SVM_AVX_INT_SHIFTX(vpsrlw, 1, 1, 0xD1, 3)
SVM_AVX_INT_SHIFTX(vpsrld, 1, 1, 0xD2, 3)
SVM_AVX_INT_SHIFTX(vpsrlq, 1, 1, 0xD3, 3)
SVM_AVX_INT_SHIFTX(vpsraw, 1, 1, 0xE1, 3)
SVM_AVX_INT_SHIFTX(vpsrad, 1, 1, 0xE2, 3)

// ---- shift by imm8 (NDD: destination is VEX.vvvv) ------------------------
// Three counts per mnemonic: in range, exactly the lane width (result 0, or
// all sign bits for the arithmetic forms) and far out of range.  x86 does NOT
// take the count modulo the lane width, which is the single most common way to
// get these wrong.
SVM_AVX_INT_SHIFTI(vpsrlw, 1, 1, 0x71, 2, 3, 3)
SVM_AVX_INT_SHIFTI(vpsrlw, 1, 1, 0x71, 2, 16, 3)
SVM_AVX_INT_SHIFTI(vpsrlw, 1, 1, 0x71, 2, 64, 3)
SVM_AVX_INT_SHIFTI(vpsraw, 1, 1, 0x71, 4, 3, 3)
SVM_AVX_INT_SHIFTI(vpsraw, 1, 1, 0x71, 4, 16, 3)
SVM_AVX_INT_SHIFTI(vpsraw, 1, 1, 0x71, 4, 64, 3)
SVM_AVX_INT_SHIFTI(vpsllw, 1, 1, 0x71, 6, 3, 3)
SVM_AVX_INT_SHIFTI(vpsllw, 1, 1, 0x71, 6, 16, 3)
SVM_AVX_INT_SHIFTI(vpsllw, 1, 1, 0x71, 6, 64, 3)
SVM_AVX_INT_SHIFTI(vpsrld, 1, 1, 0x72, 2, 3, 3)
SVM_AVX_INT_SHIFTI(vpsrld, 1, 1, 0x72, 2, 32, 3)
SVM_AVX_INT_SHIFTI(vpsrld, 1, 1, 0x72, 2, 64, 3)
SVM_AVX_INT_SHIFTI(vpsrad, 1, 1, 0x72, 4, 3, 3)
SVM_AVX_INT_SHIFTI(vpsrad, 1, 1, 0x72, 4, 32, 3)
SVM_AVX_INT_SHIFTI(vpsrad, 1, 1, 0x72, 4, 64, 3)
SVM_AVX_INT_SHIFTI(vpslld, 1, 1, 0x72, 6, 3, 3)
SVM_AVX_INT_SHIFTI(vpslld, 1, 1, 0x72, 6, 32, 3)
SVM_AVX_INT_SHIFTI(vpslld, 1, 1, 0x72, 6, 64, 3)
SVM_AVX_INT_SHIFTI(vpsrlq, 1, 1, 0x73, 2, 3, 3)
SVM_AVX_INT_SHIFTI(vpsrlq, 1, 1, 0x73, 2, 64, 3)
SVM_AVX_INT_SHIFTI(vpsrlq, 1, 1, 0x73, 2, 200, 3)
SVM_AVX_INT_SHIFTI(vpsllq, 1, 1, 0x73, 6, 3, 3)
SVM_AVX_INT_SHIFTI(vpsllq, 1, 1, 0x73, 6, 64, 3)
SVM_AVX_INT_SHIFTI(vpsllq, 1, 1, 0x73, 6, 200, 3)

// ---- sign / zero extension ------------------------------------------------
// The source is narrower than the destination, and at VEX.256 the source's
// HIGH 64 bits feed the destination's HIGH 128-bit lane -- the one place in
// this family where a 128-bit source register spans both output lanes.
SVM_AVX_INT_UN(vpmovsxbw, 1, 2, 0x20, 0, 3, 8, 16)
SVM_AVX_INT_UN(vpmovsxbd, 1, 2, 0x21, 0, 3, 4, 8)
SVM_AVX_INT_UN(vpmovsxbq, 1, 2, 0x22, 0, 3, 2, 4)
SVM_AVX_INT_UN(vpmovsxwd, 1, 2, 0x23, 0, 3, 8, 16)
SVM_AVX_INT_UN(vpmovsxwq, 1, 2, 0x24, 0, 3, 4, 8)
SVM_AVX_INT_UN(vpmovsxdq, 1, 2, 0x25, 0, 3, 8, 16)
SVM_AVX_INT_UN(vpmovzxbw, 1, 2, 0x30, 0, 3, 8, 16)
SVM_AVX_INT_UN(vpmovzxbd, 1, 2, 0x31, 0, 3, 4, 8)
SVM_AVX_INT_UN(vpmovzxbq, 1, 2, 0x32, 0, 3, 2, 4)
SVM_AVX_INT_UN(vpmovzxwd, 1, 2, 0x33, 0, 3, 8, 16)
SVM_AVX_INT_UN(vpmovzxwq, 1, 2, 0x34, 0, 3, 4, 8)
SVM_AVX_INT_UN(vpmovzxdq, 1, 2, 0x35, 0, 3, 8, 16)

// ---- AVX2 broadcasts ------------------------------------------------------
// The source element comes from the LOW 128-bit lane of the source register
// and is replicated across BOTH lanes of the destination: cross-lane, but
// expressible as "compute one V128 and write it to both halves".
SVM_AVX_INT_UN(vpbroadcastb, 1, 2, 0x78, 0, 3, 1, 1)
SVM_AVX_INT_UN(vpbroadcastw, 1, 2, 0x79, 0, 3, 2, 2)
SVM_AVX_INT_UN(vpbroadcastd, 1, 2, 0x58, 0, 3, 4, 4)
SVM_AVX_INT_UN(vpbroadcastq, 1, 2, 0x59, 0, 3, 8, 8)

// ---- shuffles with an imm8 ------------------------------------------------
// vpshufd / vpshuflw / vpshufhw are per-128-bit-lane in AVX2: the SAME imm8
// controls both lanes and no byte crosses between them.
SVM_AVX_INT_UNI(vpshufd, 1, 1, 0x70, 0, 0x1B, 3)
SVM_AVX_INT_UNI(vpshufd, 1, 1, 0x70, 0, 0xE4, 3)
SVM_AVX_INT_UNI(vpshufd, 1, 1, 0x70, 0, 0x00, 3)
SVM_AVX_INT_UNI(vpshuflw, 3, 1, 0x70, 0, 0x1B, 3)
SVM_AVX_INT_UNI(vpshuflw, 3, 1, 0x70, 0, 0x93, 3)
SVM_AVX_INT_UNI(vpshufhw, 2, 1, 0x70, 0, 0x1B, 3)
SVM_AVX_INT_UNI(vpshufhw, 2, 1, 0x70, 0, 0x93, 3)

// vpermq: AVX2, VEX.256.66.0F3A.W1 -- an ARBITRARY permutation of the four
// qwords of a ymm, so it reads qwords from the other 128-bit lane.  0x1B
// reverses all four (every qword crosses), 0xE4 is the identity, 0x00
// broadcasts qword 0 into all four, 0x4E swaps the two 128-bit lanes.
SVM_AVX_INT_UNI(vpermq, 1, 3, 0x00, 1, 0x1B, 2)
SVM_AVX_INT_UNI(vpermq, 1, 3, 0x00, 1, 0xE4, 2)
SVM_AVX_INT_UNI(vpermq, 1, 3, 0x00, 1, 0x00, 2)
SVM_AVX_INT_UNI(vpermq, 1, 3, 0x00, 1, 0x4E, 2)
SVM_AVX_INT_UNI(vpermq, 1, 3, 0x00, 1, 0x93, 2)

// ---- three-operand forms with an imm8 ------------------------------------
// vpalignr concatenates src1:src2 and byte-shifts right, PER 128-bit LANE in
// AVX2.  Counts 0, 8 and 16 are the boundaries; 3 and 11 straddle a qword; 20
// and 33 exercise the >=16 and >=32 zero-fill cases.
SVM_AVX_INT_ALUI(vpalignr, 1, 3, 0x0F, 0, 0, 3)
SVM_AVX_INT_ALUI(vpalignr, 1, 3, 0x0F, 0, 3, 3)
SVM_AVX_INT_ALUI(vpalignr, 1, 3, 0x0F, 0, 8, 3)
SVM_AVX_INT_ALUI(vpalignr, 1, 3, 0x0F, 0, 11, 3)
SVM_AVX_INT_ALUI(vpalignr, 1, 3, 0x0F, 0, 16, 3)
SVM_AVX_INT_ALUI(vpalignr, 1, 3, 0x0F, 0, 20, 3)
SVM_AVX_INT_ALUI(vpalignr, 1, 3, 0x0F, 0, 33, 3)

// vpblendw: imm8 selects per WORD, and in AVX2 the same 8 bits apply to both
// 128-bit lanes.
SVM_AVX_INT_ALUI(vpblendw, 1, 3, 0x0E, 0, 0x00, 3)
SVM_AVX_INT_ALUI(vpblendw, 1, 3, 0x0E, 0, 0xFF, 3)
SVM_AVX_INT_ALUI(vpblendw, 1, 3, 0x0E, 0, 0xA5, 3)
SVM_AVX_INT_ALUI(vpblendw, 1, 3, 0x0E, 0, 0x0F, 3)

// vpblendd: imm8 selects per DWORD.  Unlike vpblendw the control is NOT
// replicated -- at VEX.256 bits 3:0 drive the low lane and bits 7:4 the high
// lane, so the two halves use different masks.
SVM_AVX_INT_ALUI(vpblendd, 1, 3, 0x02, 0, 0x00, 3)
SVM_AVX_INT_ALUI(vpblendd, 1, 3, 0x02, 0, 0xFF, 3)
SVM_AVX_INT_ALUI(vpblendd, 1, 3, 0x02, 0, 0x5A, 3)
SVM_AVX_INT_ALUI(vpblendd, 1, 3, 0x02, 0, 0x0F, 3)

// vperm2i128: selects each output 128-bit lane from any of the four input
// lanes, or zeroes it.  0x20/0x31 are the "concatenate" forms, 0x01 swaps
// within src1, 0x13 mixes both sources, 0x8F zeroes the high lane, 0x88
// zeroes both.
SVM_AVX_INT_ALUI(vperm2i128, 1, 3, 0x46, 0, 0x20, 2)
SVM_AVX_INT_ALUI(vperm2i128, 1, 3, 0x46, 0, 0x31, 2)
SVM_AVX_INT_ALUI(vperm2i128, 1, 3, 0x46, 0, 0x01, 2)
SVM_AVX_INT_ALUI(vperm2i128, 1, 3, 0x46, 0, 0x13, 2)
SVM_AVX_INT_ALUI(vperm2i128, 1, 3, 0x46, 0, 0x8F, 2)
SVM_AVX_INT_ALUI(vperm2i128, 1, 3, 0x46, 0, 0x88, 2)

// vpermd: AVX2, VEX.256.66.0F38.W0 -- src1 supplies eight RUNTIME dword
// indices selecting freely from all eight dwords of src2.  This is the only
// instruction in the family whose cross-lane routing is not known at decode
// time.
SVM_AVX_INT_ALU(vpermd, 1, 2, 0x36, 0, 2)

#undef SVM_AVX_INT_ALU
#undef SVM_AVX_INT_SHIFTX
#undef SVM_AVX_INT_SHIFTI
#undef SVM_AVX_INT_UN
#undef SVM_AVX_INT_ALUI
#undef SVM_AVX_INT_UNI
