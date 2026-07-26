// The floating-point compare-to-mask family: VCMPPS / VCMPPD / VCMPSS /
// VCMPSD across all 32 AVX predicates, plus the four legacy SSE mnemonics they
// extend.
//
// Shared VERBATIM by the Rosetta reference generator (avx_cmp_rosetta_ref.c)
// and the SwiftVM differential case (avx_cmp_test.cpp).  As in
// avx_fp2_ops.inc, only the GENERATOR encodes: every reference row carries the
// literal instruction bytes it executed and the test replays those, so this
// table's job is (a) naming what must be covered and (b) letting the test
// assert that every named mnemonic actually produced rows.
//
// WHY THE LEGACY SSE MNEMONICS ARE IN AN "AVX" TABLE
// --------------------------------------------------
// Supporting predicates 8..31 changed what ir::OpCode::VecFCmpMask's predicate
// immediate MEANS -- it is now a relation set over {<, ==, >, unordered}
// rather than the x86 SSE encoding (see runtime/frontend/x86/
// fp_cmp_predicate.h).  CMPPS/CMPPD/CMPSS/CMPSD are that opcode's OTHER
// caller, and nothing else in the suite executes them, so without rows here a
// silent regression of the eight legacy predicates would pass unnoticed.  They
// are therefore measured against the same hardware oracle, which is what makes
// "the old call site is unchanged" a measurement rather than a claim.
//
// Field meaning (all values UN-inverted):
//   vex    : 1 = VEX-encoded (predicates 0..31), 0 = legacy SSE (0..7 only)
//   pp     : 0 = none (ps), 1 = 66 (pd), 2 = F3 (ss), 3 = F2 (sd)
//   scalar : 1 = ss/sd -- one lane, src1's upper lanes merged into the result
//   l256   : 1 = the generator also emits a VEX.L=1 (256-bit) form
//
// Every row is run at both r/m shapes (register and memory) and against every
// input pair, and the destination is a POISONED ymm0 read back in full, so a
// VEX.128 form's zeroing of bits 255:128 -- and a legacy SSE form's
// PRESERVING of them -- are both measured rather than assumed.

#ifndef SVM_CMP
#define SVM_CMP(name, vex, pp, scalar, l256)
#endif

// ---- VEX: the whole point of this file, imm8 0..31 ------------------------
SVM_CMP(vcmpps, 1, 0, 0, 1)
SVM_CMP(vcmppd, 1, 1, 0, 1)
// VEX.LIG: the scalar forms ignore VEX.L, so only L=0 is emitted.
SVM_CMP(vcmpss, 1, 2, 1, 0)
SVM_CMP(vcmpsd, 1, 3, 1, 0)

// ---- legacy SSE: the regression guard for the OTHER VecFCmpMask caller ----
SVM_CMP(cmpps, 0, 0, 0, 0)
SVM_CMP(cmppd, 0, 1, 0, 0)
SVM_CMP(cmpss, 0, 2, 1, 0)
SVM_CMP(cmpsd, 0, 3, 1, 0)
