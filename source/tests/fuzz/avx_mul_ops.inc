// The widening-multiply family: vpmuludq / vpmuldq and their legacy SSE twins.
//
// Shared VERBATIM by the Rosetta reference generator (avx_mul_rosetta_ref.c)
// and the SwiftVM differential case (avx_mul_test.cpp).  As in avx_fp2_ops.inc,
// only the GENERATOR encodes: every reference row carries the literal
// instruction bytes it executed and the test replays those bytes, so a wrong
// field in this table cannot make both sides test the same wrong instruction
// and pass vacuously.  This table's remaining job is to name what must be
// covered, and to let the test assert that every named mnemonic produced rows.
//
// Field meaning (all values UN-inverted):
//   shape  : how the generator builds the operands -- see below
//   map    : 1 = 0F, 2 = 0F38
//   pp     : 1 = 66 (every form here is 66-prefixed)
//   opcode : the opcode byte
//
// SHAPES
//   S_VEXBIN  VEX.NDS three-operand: dst = ymm0 (POISONED beforehand),
//             VEX.vvvv = ymm1 (A), r/m = ymm2 (B) or [rdi+B].  Both VEX.L,
//             both operand shapes, and both VEX.W values -- these opcodes are
//             WIG, so W=1 must decode identically to W=0.
//   S_SSEBIN  legacy two-operand SSE: dst = xmm1, which already holds A, and
//             r/m = xmm2 (B) or [rdi+B].  The answer is read out of ymm1, all
//             32 bytes, because the point of these rows is the OPPOSITE of C3:
//             a legacy SSE write must LEAVE bits 255:128 alone, and ymm1's
//             upper half holds A's upper half throughout.
//
// WHY THE ODD LANES OF EVERY INPUT DIFFER FROM THE EVEN ONES
// ----------------------------------------------------------
// PMULUDQ/PMULDQ multiply source lanes 0 and 2 of the 32-bit view (the low
// dword of each qword), not lanes 0 and 1.  AArch64's UMULL/SMULL widen lanes
// 0 and 1, so the natural-looking lowering is WRONG and only inputs whose lane
// 1 differs from lane 2 can see it.  Every pair below is built so that
// a[1]*b[1] != a[2]*b[2]; avx_mul_test.cpp asserts that property on the data
// rather than trusting it.

#ifndef SVM_MUL
#define SVM_MUL(name, shape, map, pp, opcode)
#endif

// VEX.NDS.128/256.66.0F.WIG F4 /r -- unsigned 32x32 -> 64 of the even dwords.
SVM_MUL(vpmuludq, S_VEXBIN, 1, 1, 0xF4)
// VEX.NDS.128/256.66.0F38.WIG 28 /r -- the signed form (AVX2 / SSE4.1).
SVM_MUL(vpmuldq, S_VEXBIN, 2, 1, 0x28)
// 66 0F F4 /r -- SSE2.  Already had a handler (scalar, via two 64-bit MULs);
// re-pointed at the vector opcode, so these rows are a regression net as much
// as a new-coverage net.
SVM_MUL(pmuludq, S_SSEBIN, 1, 1, 0xF4)
// 66 0F 38 28 /r -- SSE4.1.  Had NO handler at all: the guest took a FALLBACK
// and then IllegalCode, which is fatal.
SVM_MUL(pmuldq, S_SSEBIN, 2, 1, 0x28)
