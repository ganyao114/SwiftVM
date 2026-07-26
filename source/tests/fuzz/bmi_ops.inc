// The BMI1 / BMI2 instruction table.  Included VERBATIM by both
// bmi_rosetta_ref.c (which executes these encodings on real hardware under
// Rosetta 2) and bmi_test.cpp (which runs them through SwiftVM), so a wrong
// opcode byte cannot make the two sides test different instructions.
//
// Fields
//   name   spelling, also the reference-table key
//   pp     mandatory-prefix slot: 0 none, 1 = 66, 2 = F3, 3 = F2
//   mm     opcode map: 2 = 0F38, 3 = 0F3A, 0 = LEGACY (F3 0F xx, no VEX)
//   op     opcode byte
//   ext    ModRM.reg when it is an opcode extension (shape VM), else 0
//   shape  which architectural role each encoding field plays -- see below
//   imm    the imm8 for shape RMI, else 0
//
// Shapes (dest is always written, sources are always read):
//   RVM   dest = ModRM.reg, src1 = VEX.vvvv,  src2 = r/m
//   RMV   dest = ModRM.reg, src1 = r/m,       src2 = VEX.vvvv
//   VM    dest = VEX.vvvv,  src  = r/m,       ModRM.reg is the extension
//   RMI   dest = ModRM.reg, src  = r/m,       imm8
//   MULX  dest_hi = ModRM.reg, dest_lo = VEX.vvvv, src = r/m, rdx implicit
//   LEG   dest = ModRM.reg, src = r/m, legacy F3 0F op encoding (no VEX)
//
// The shape is what a hand-written handler most easily gets wrong: BLSI/BLSMSK/
// BLSR put the DESTINATION in VEX.vvvv, which is the reverse of every other
// form, and BEXTR/BZHI/SHLX/SARX/SHRX take their second source from vvvv while
// ANDN/PDEP/PEXT take their FIRST from it.  Swapping them is invisible for
// symmetric inputs, which is why the input pairs are deliberately asymmetric.

#ifndef SVM_BMI_OP
#define SVM_BMI_OP(name, pp, mm, op, ext, shape, imm)
#endif

// ---- BMI1 -----------------------------------------------------------------
SVM_BMI_OP(andn,   0, 2, 0xF2, 0, RVM,  0)
SVM_BMI_OP(blsr,   0, 2, 0xF3, 1, VM,   0)
SVM_BMI_OP(blsmsk, 0, 2, 0xF3, 2, VM,   0)
SVM_BMI_OP(blsi,   0, 2, 0xF3, 3, VM,   0)
SVM_BMI_OP(bextr,  0, 2, 0xF7, 0, RMV,  0)
// ---- BMI2 -----------------------------------------------------------------
SVM_BMI_OP(bzhi,   0, 2, 0xF5, 0, RMV,  0)
SVM_BMI_OP(pext,   2, 2, 0xF5, 0, RVM,  0)
SVM_BMI_OP(pdep,   3, 2, 0xF5, 0, RVM,  0)
SVM_BMI_OP(mulx,   3, 2, 0xF6, 0, MULX, 0)
SVM_BMI_OP(shlx,   1, 2, 0xF7, 0, RMV,  0)
SVM_BMI_OP(sarx,   2, 2, 0xF7, 0, RMV,  0)
SVM_BMI_OP(shrx,   3, 2, 0xF7, 0, RMV,  0)
// RORX's rotate is an immediate, so each interesting amount is its own row.
// 0 is the identity (and the one the backends would have to encode as ROR #0),
// 31 / 63 are the top of each width's mask, and 32 distinguishes the widths:
// at W0 it masks to 0 and at W1 it is a real half-word swap.
SVM_BMI_OP(rorx0,  3, 3, 0xF0, 0, RMI,  0)
SVM_BMI_OP(rorx1,  3, 3, 0xF0, 0, RMI,  1)
SVM_BMI_OP(rorx4,  3, 3, 0xF0, 0, RMI,  4)
SVM_BMI_OP(rorx31, 3, 3, 0xF0, 0, RMI,  31)
SVM_BMI_OP(rorx32, 3, 3, 0xF0, 0, RMI,  32)
SVM_BMI_OP(rorx63, 3, 3, 0xF0, 0, RMI,  63)
// ---- the two legacy-encoded counters --------------------------------------
SVM_BMI_OP(tzcnt,  2, 0, 0xBC, 0, LEG,  0)
SVM_BMI_OP(lzcnt,  2, 0, 0xBD, 0, LEG,  0)

#undef SVM_BMI_OP

// ---------------------------------------------------------------------------
// Input pairs.  `a` reaches the instruction in rcx (and in rdx, which is MULX's
// implicit multiplicand); `b` reaches it in rsi and, for the memory shape, at
// [r13+0].  Both are always full 64-bit values -- the 32-bit forms read their
// low halves, which is itself worth testing because a handler that operated at
// 64 bits would leak the upper half into the result or the flags.
// ---------------------------------------------------------------------------
#ifndef SVM_BMI_PAIR
#define SVM_BMI_PAIR(name, a, b)
#endif

SVM_BMI_PAIR(zeros,     0x0000000000000000ull, 0x0000000000000000ull)
SVM_BMI_PAIR(zero_one,  0x0000000000000000ull, 0x0000000000000001ull)
SVM_BMI_PAIR(one_zero,  0x0000000000000001ull, 0x0000000000000000ull)
SVM_BMI_PAIR(ones,      0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull)
SVM_BMI_PAIR(msb64,     0x8000000000000000ull, 0x8000000000000000ull)
// The 32-bit sign boundary: SF must come from bit 31 at W0 and bit 63 at W1.
SVM_BMI_PAIR(msb32,     0x0000000080000000ull, 0x0000000080000000ull)
SVM_BMI_PAIR(mixed,     0x123456789ABCDEF0ull, 0x0F1E2D3C4B5A6978ull)
SVM_BMI_PAIR(alt,       0xAAAAAAAAAAAAAAAAull, 0x5555555555555555ull)
SVM_BMI_PAIR(one_one,   0x0000000000000001ull, 0x0000000000000001ull)
// Shift / BZHI index exactly at the 32-bit width: masked to 0 at W0.
SVM_BMI_PAIR(n32,       0x0000000000000020ull, 0x00000000FFFFFFFFull)
SVM_BMI_PAIR(n31,       0x000000000000001Full, 0xFFFFFFFFFFFFFFFFull)
SVM_BMI_PAIR(n63,       0x000000000000003Full, 0xFFFFFFFFFFFFFFFFull)
// Index at and beyond the 64-bit width: BZHI must leave the source alone and
// set CF, and neither backend's shift masking may be allowed to show through.
SVM_BMI_PAIR(n64,       0x0000000000000040ull, 0xFFFFFFFFFFFFFFFFull)
SVM_BMI_PAIR(n255,      0x00000000000000FFull, 0xFFFFFFFFFFFFFFFFull)
// A count of 256: masks to 0 for the shifts, and stays out of range for BZHI
// because only SRC2[7:0] participates there.
SVM_BMI_PAIR(n256,      0x0000000000000100ull, 0x0000000000000001ull)
// BEXTR control words: start=4 len=8, start=0x20 len=0x40, start=0 len=0xFF,
// and len=0 (which must produce exactly zero, not "everything").
SVM_BMI_PAIR(bx_4_8,    0x0000000000000804ull, 0x123456789ABCDEF0ull)
SVM_BMI_PAIR(bx_32_64,  0x0000000000004020ull, 0xFFFFFFFFFFFFFFFFull)
SVM_BMI_PAIR(bx_0_255,  0x000000000000FF00ull, 0x123456789ABCDEF0ull)
SVM_BMI_PAIR(bx_len0,   0x0000000000000008ull, 0xFFFFFFFFFFFFFFFFull)
// 32x32 = the largest product that still fits MULX.W0's two halves.
SVM_BMI_PAIR(u32max,    0x00000000FFFFFFFFull, 0x00000000FFFFFFFFull)
// Only high bits set: LZCNT/TZCNT at W0 see a zero source, at W1 they do not.
SVM_BMI_PAIR(hi_only,   0x0000000000000010ull, 0xFFFFFFFF00000000ull)
SVM_BMI_PAIR(sign_low,  0x8000000000000001ull, 0x0000000000000080ull)

#undef SVM_BMI_PAIR
