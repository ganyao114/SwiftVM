// VEX floating-point instruction table, shared VERBATIM by the Rosetta
// reference generator (avx_fp_rosetta_ref.c) and the SwiftVM differential case
// (avx_fp_test.cpp).
//
// The point of a shared table is that the two sides CANNOT drift: the generator
// encodes what this table says and runs it on real x86-64 through Rosetta 2;
// the test encodes the same entries and runs them through SwiftVM.  A wrong
// opcode byte here would make both sides test the same wrong instruction and
// the differential would pass vacuously -- so every entry was cross-checked
// against the Intel SDM VEX encodings, and the generator prints each stub's
// bytes (--dump-encodings) so they can be disassembled and audited.
//
// Field meaning (all values UN-inverted, matching EmitVexC4 on both sides):
//   pp     : 0 = none, 1 = 66, 2 = F3, 3 = F2
//   opcode : the opcode byte
//   lane   : element width in bits (32 = single, 64 = double)
// EVERY instruction in this family lives in the implied-0F map (VEX.mmmmm = 1)
// and every one is VEX.WIG, so those two fields are not parameters.
//
// Widths: a "packed" entry is exercised at BOTH VEX.L=0 and VEX.L=1 and gets a
// separate reference row for each, because the two differ in more than size --
// contract C3 says VEX.128 must ZERO bits 255:128 of the destination, and the
// generator reads back all 32 bytes of ymm0 (pre-poisoned) precisely so that
// zeroing is measured on hardware rather than assumed.  Scalar entries are
// VEX.LIG and are emitted with L=0 only.
//
// Macros; a consumer defines the ones it cares about and leaves the rest
// defined-empty.
//
//   SVM_AVXFP_PACKED(name, pp, opcode, lane)
//       Three-operand packed: dst = vvvv OP r/m.  Both reg-reg and reg-mem
//       shapes are exercised against the same reference (identical inputs must
//       give identical results; what differs is only SwiftVM's load path).
//
//   SVM_AVXFP_SCALAR(name, pp, opcode, lane)
//       Three-operand scalar: dst[lane 0] = vvvv[lane 0] OP r/m[lane 0], and
//       dst's REMAINING low-128 lanes are copied from vvvv (NOT from dst).
//       That copy is the single most commonly mis-implemented part of the VEX
//       scalar forms, so vvvv is deliberately a different register from dst.
//
//   SVM_AVXFP_UNARY(name, pp, opcode, lane)
//       Two-operand, source and destination the same width (vsqrtps/pd and the
//       lane-preserving conversions).  VEX.vvvv must encode 1111b.
//
//   SVM_AVXFP_WIDEN(name, pp, opcode)
//       Two-operand where the SOURCE is half the destination width
//       (vcvtps2pd: xmm/m64 -> xmm at L=0, xmm/m128 -> ymm at L=1).
//
//   SVM_AVXFP_NARROW(name, pp, opcode)
//       Two-operand where the DESTINATION is half the source width
//       (vcvtpd2ps: xmm/m128 -> xmm at L=0, ymm/m256 -> xmm at L=1).  The
//       destination is an XMM at BOTH widths, so C3 applies to both.
//
//   SVM_AVXFP_CMP(name, pp, opcode, lane, scalar)
//       vcmpps/pd/ss/sd.  Driven over imm8 predicates 0..7 (the SSE set).
//       Predicates 8..31 are an AVX extension that SwiftVM's VecFCmpMask
//       cannot express and its decoder declines; they are deliberately NOT in
//       this table, because a reference row for a predicate the decoder
//       refuses would just be an unexplained failure.
//
//   SVM_AVXFP_COMIS(name, pp, opcode, lane)
//       Scalar ordered compare into EFLAGS.  The reference records CF, PF, ZF,
//       OF and SF as five bytes, captured with SETcc.
//
//   SVM_AVXFP_MOVMSK(name, pp, opcode, lane)
//       Sign-bit extraction into a GPR.  The 256-bit form is the only FP
//       instruction here whose two 128-bit halves are not independent.

// Destination poison.  Before every instruction the generator loads ymm0 with
// these 32 bytes and the SwiftVM test writes the identical bytes into
// xmms[0] / ymm_high[0], so the reference data MEASURES contract C3 (a VEX.128
// write zeroes bits 255:128) instead of assuming it: if the hardware left the
// upper half alone the reference would show the poison, not zeros.  It matches
// the poison the VEX.256 integer case already uses for register 0.
#ifndef SVM_AVXFP_POISON
#define SVM_AVXFP_POISON(byte_index) ((unsigned char)(0xA5u ^ (unsigned)(byte_index)))
#endif

#ifndef SVM_AVXFP_PACKED
#define SVM_AVXFP_PACKED(name, pp, opcode, lane)
#endif
#ifndef SVM_AVXFP_SCALAR
#define SVM_AVXFP_SCALAR(name, pp, opcode, lane)
#endif
#ifndef SVM_AVXFP_UNARY
#define SVM_AVXFP_UNARY(name, pp, opcode, lane)
#endif
#ifndef SVM_AVXFP_WIDEN
#define SVM_AVXFP_WIDEN(name, pp, opcode)
#endif
#ifndef SVM_AVXFP_NARROW
#define SVM_AVXFP_NARROW(name, pp, opcode)
#endif
#ifndef SVM_AVXFP_CMP
#define SVM_AVXFP_CMP(name, pp, opcode, lane, scalar)
#endif
#ifndef SVM_AVXFP_COMIS
#define SVM_AVXFP_COMIS(name, pp, opcode, lane)
#endif
#ifndef SVM_AVXFP_MOVMSK
#define SVM_AVXFP_MOVMSK(name, pp, opcode, lane)
#endif

// ---- packed arithmetic ----------------------------------------------------
// NaN propagation is the interesting part: x86 returns the QUIETED first NaN
// operand, ARM64's FADD/FSUB/FMUL/FDIV return a default NaN, and the SwiftVM
// backend patches the difference afterwards (EmitVecFloatNaNFixup).  The input
// pairs carry NaNs with DISTINCT PAYLOADS on the two sides, so "which operand's
// NaN came through" is readable from the reference bytes and not merely "some
// NaN came through".
SVM_AVXFP_PACKED(vaddps, 0, 0x58, 32)
SVM_AVXFP_PACKED(vaddpd, 1, 0x58, 64)
SVM_AVXFP_PACKED(vsubps, 0, 0x5C, 32)
SVM_AVXFP_PACKED(vsubpd, 1, 0x5C, 64)
SVM_AVXFP_PACKED(vmulps, 0, 0x59, 32)
SVM_AVXFP_PACKED(vmulpd, 1, 0x59, 64)
SVM_AVXFP_PACKED(vdivps, 0, 0x5E, 32)
SVM_AVXFP_PACKED(vdivpd, 1, 0x5E, 64)

// ---- packed min / max -----------------------------------------------------
// x86 MIN/MAX are "if (src1 OP src2) then src1 else src2", which is NOT
// commutative: with either operand NaN the answer is src2, and min(+0,-0)
// differs from min(-0,+0).  ARM64's FMIN/FMAX do the opposite on NaN, so this
// is exactly where a naive lowering breaks.  The f32nan/f64nan input pairs come
// in MIRRORED versions (A and B swapped) so both operand orders are measured.
SVM_AVXFP_PACKED(vminps, 0, 0x5D, 32)
SVM_AVXFP_PACKED(vminpd, 1, 0x5D, 64)
SVM_AVXFP_PACKED(vmaxps, 0, 0x5F, 32)
SVM_AVXFP_PACKED(vmaxpd, 1, 0x5F, 64)

// ---- packed bitwise -------------------------------------------------------
// vandnps/vandnpd are "NOT(src1) AND src2" -- src1 is the inverted one, which
// is the operand order a split implementation is most likely to reverse.  The
// input pairs are chosen so A OP B != B OP A for every entry here.
SVM_AVXFP_PACKED(vandps, 0, 0x54, 32)
SVM_AVXFP_PACKED(vandpd, 1, 0x54, 64)
SVM_AVXFP_PACKED(vandnps, 0, 0x55, 32)
SVM_AVXFP_PACKED(vandnpd, 1, 0x55, 64)
SVM_AVXFP_PACKED(vorps, 0, 0x56, 32)
SVM_AVXFP_PACKED(vorpd, 1, 0x56, 64)
SVM_AVXFP_PACKED(vxorps, 0, 0x57, 32)
SVM_AVXFP_PACKED(vxorpd, 1, 0x57, 64)

// ---- scalar arithmetic / min / max / sqrt ---------------------------------
// dst[127:32] (ss) or dst[127:64] (sd) come from VEX.vvvv, and dst[255:128] is
// zeroed.  The generator puts src1 in ymm1 and the destination in ymm0, so an
// implementation that merged from the DESTINATION (the legacy SSE rule) instead
// of from vvvv produces visibly different bytes.
SVM_AVXFP_SCALAR(vaddss, 2, 0x58, 32)
SVM_AVXFP_SCALAR(vaddsd, 3, 0x58, 64)
SVM_AVXFP_SCALAR(vsubss, 2, 0x5C, 32)
SVM_AVXFP_SCALAR(vsubsd, 3, 0x5C, 64)
SVM_AVXFP_SCALAR(vmulss, 2, 0x59, 32)
SVM_AVXFP_SCALAR(vmulsd, 3, 0x59, 64)
SVM_AVXFP_SCALAR(vdivss, 2, 0x5E, 32)
SVM_AVXFP_SCALAR(vdivsd, 3, 0x5E, 64)
SVM_AVXFP_SCALAR(vminss, 2, 0x5D, 32)
SVM_AVXFP_SCALAR(vminsd, 3, 0x5D, 64)
SVM_AVXFP_SCALAR(vmaxss, 2, 0x5F, 32)
SVM_AVXFP_SCALAR(vmaxsd, 3, 0x5F, 64)
SVM_AVXFP_SCALAR(vsqrtss, 2, 0x51, 32)
SVM_AVXFP_SCALAR(vsqrtsd, 3, 0x51, 64)

// ---- two-operand, same width ----------------------------------------------
// vcvtps2dq rounds per MXCSR (round-to-nearest-even by default) while
// vcvttps2dq truncates; the f32cvt input pair contains 0.5/1.5/2.5/-2.5 so the
// two are distinguishable, and out-of-range / NaN inputs so the x86
// "integer indefinite" (0x80000000) result is measured rather than assumed.
SVM_AVXFP_UNARY(vsqrtps, 0, 0x51, 32)
SVM_AVXFP_UNARY(vsqrtpd, 1, 0x51, 64)
SVM_AVXFP_UNARY(vcvtdq2ps, 0, 0x5B, 32)
SVM_AVXFP_UNARY(vcvtps2dq, 1, 0x5B, 32)
SVM_AVXFP_UNARY(vcvttps2dq, 2, 0x5B, 32)

// ---- width-changing conversions -------------------------------------------
// These are the two FP instructions whose 128-bit halves are NOT independent
// (see decoder_avx_fp.cc): vcvtps2pd's result high half comes from the SOURCE's
// high 64 bits, and vcvtpd2ps concatenates two half-results into one register.
SVM_AVXFP_WIDEN(vcvtps2pd, 0, 0x5A)
SVM_AVXFP_NARROW(vcvtpd2ps, 1, 0x5A)

// ---- compares into a mask -------------------------------------------------
SVM_AVXFP_CMP(vcmpps, 0, 0xC2, 32, 0)
SVM_AVXFP_CMP(vcmppd, 1, 0xC2, 64, 0)
SVM_AVXFP_CMP(vcmpss, 2, 0xC2, 32, 1)
SVM_AVXFP_CMP(vcmpsd, 3, 0xC2, 64, 1)

// ---- compares into EFLAGS -------------------------------------------------
// UCOMIS and COMIS differ only in which NaN class raises #IA.  SwiftVM models
// no FP exceptions, so both must produce identical flags; both are listed so
// that "identical" is measured on hardware instead of being an assumption.
SVM_AVXFP_COMIS(vucomiss, 0, 0x2E, 32)
SVM_AVXFP_COMIS(vucomisd, 1, 0x2E, 64)
SVM_AVXFP_COMIS(vcomiss, 0, 0x2F, 32)
SVM_AVXFP_COMIS(vcomisd, 1, 0x2F, 64)

// ---- sign-bit extraction --------------------------------------------------
// At 256 bits the two half-masks must combine as `lo | hi << 4` (ps) or
// `lo | hi << 2` (pd).  The input pairs give the two halves different,
// non-degenerate masks so a swapped combine is distinguishable.
SVM_AVXFP_MOVMSK(vmovmskps, 0, 0x50, 32)
SVM_AVXFP_MOVMSK(vmovmskpd, 1, 0x50, 64)

#undef SVM_AVXFP_PACKED
#undef SVM_AVXFP_SCALAR
#undef SVM_AVXFP_UNARY
#undef SVM_AVXFP_WIDEN
#undef SVM_AVXFP_NARROW
#undef SVM_AVXFP_CMP
#undef SVM_AVXFP_COMIS
#undef SVM_AVXFP_MOVMSK
