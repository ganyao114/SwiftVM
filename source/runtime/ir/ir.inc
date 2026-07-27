// IR instruction table — the single source of truth for instruction signatures.
// Format: INST(Name, RetToken, ArgTokens...)
//   Name      -> OpCode::Name, builder method name, Emit##Name/Run##Name
//   RetToken  -> C++ return type in builders (args.h: Void, Value, BOOL, U32, U64)
//                AND ArgType via meta::tokens (ir_meta.h)
//   ArgTokens -> must be exact ArgType enumerator names (ir_types.h)
// Slot rule:   Operand = 3 physical slots, everything else 1 (ir_types.h PhysicalSlots).

// Read Write
INST(DefineLocal, Void, Local)
INST(LoadLocal, Value, Local)
INST(StoreLocal, Void, Local, Value)
INST(LoadUniform, Value, Uniform)
INST(StoreUniform, Void, Uniform, Value)
// Host address of a byte within the current uniform buffer.  This is only for
// stateful frontend helpers that need to update several architectural fields
// atomically (x87); ordinary guest code must keep using Load/StoreUniform.
INST(GetUniformAddress, Value, Imm)
// Stateful x87 operation. The dedicated opcode keeps x87 out of the generic
// CallLambda classification so ARM64 can inline selected commands and
// whole-function compilation can retain blocks containing x87.
INST(X87Op, Value, Value, Imm, Value)
INST(LoadMemory, Value, Operand)
INST(StoreMemory, Void, Operand, Value)
INST(LoadMemoryTSO, Value, Operand)
INST(StoreMemoryTSO, Void, Operand, Value)
INST(MemoryCopy, Void, Lambda, Lambda, Imm)
INST(MemoryCopyTSO, Void, Lambda, Lambda, Imm)
INST(MemoryBarrierTSO, Void)
INST(CompareAndSwap, Value, Value, Value, Value)
// 128-bit CAS takes scalar halves to stay within the IR's five physical-arg
// limit and returns the observed {high,low} pair as V128.
INST(CompareAndSwap128, Value, Value, Value, Value, Value, Value)
// Dynamic architectural-alignment check. A failure terminates the current
// guest block with PageFatal; the frontend sets the precise guest location
// immediately before it.
INST(CheckMemoryAlignment, Void, Value, Imm)
INST(AtomicExchange, Value, Value, Value)
INST(AtomicFetchAdd, Value, Value, Value)
INST(AtomicRMW, Value, Imm, Value, Value, Value)

// Help
INST(UniformBarrier, Void)
INST(Goto, Value, BOOL)
INST(NotGoto, Value, BOOL)
INST(Select, Value, BOOL, Value, Value)
INST(CondSelect, Value, Cond, Value, Value)
INST(BindLabel, Void, Value)
INST(CallLambda, Value, Lambda, Value, Value, Value)
INST(CallLocation, Value, Lambda, Params)
INST(CallDynamic, Value, Lambda, Params)
INST(AddPhi, Value, Params)
INST(Nop, Void)
INST(AdvancePC, Void, Imm)
INST(SetLocation, Void, Lambda)
INST(GetLocation, Value)
INST(Zero, Value)
INST(GetHostGPR, Value, Imm, Imm)
INST(GetHostFPR, Value, Imm, Imm)
INST(SetHostGPR, Void, Value, Imm, Imm)
INST(SetHostFPR, Void, Value, Imm, Imm)
INST(BitCast, Value, Value)
INST(GetOperand, Value, Operand)
INST(GetResult, Value, Value, Value)

// Imm
INST(LoadImm, Value, Imm)

// Help
INST(PushRSB, Void, Lambda)
INST(PopRSB, Void)

// Calculator
INST(Add, Value, Value, Operand)
INST(Sub, Value, Value, Operand)
// With Carry
INST(Adc, Value, Value, Operand)
INST(Sbb, Value, Value, Operand)
// Mul & Div
INST(Mul, Value, Value, Operand)
INST(Div, Value, Value, Operand)
INST(ZeroExtend32, U32, Value)
INST(ZeroExtend64, U64, Value)
INST(SignExtend, Value, Value)
// And & Or
INST(And, Value, Value, Operand)
INST(Or, Value, Value, Operand)
INST(Xor, Value, Value, Operand)
// Not
INST(Not, Value, Value, Operand)
INST(AndNot, Value, Value, Operand)
// Flags from result
INST(GetFlags, Value, Value, Flags)
INST(SaveFlags, Void, Value, Flags)
INST(TestFlags, Value, Flags)
INST(TestNotFlags, Value, Flags)
INST(ClearFlags, Void, Flags)
// Set a single guest flag (C / V) from a computed 0/1 value. Used where the
// flag is derived rather than produced by a host flag-setting op (shift CF/OF).
INST(SetCarry, Void, Value)
INST(SetOverflow, Void, Value)
// Shift
// LogicalShiftLeft
INST(LslImm, Value, Value, Imm)
INST(LslValue, Value, Value, Value)
// LogicalShiftRight
INST(LsrImm, Value, Value, Imm)
INST(LsrValue, Value, Value, Value)
// ArithmeticShiftRight
INST(AsrImm, Value, Value, Imm)
INST(AsrValue, Value, Value, Value)
// RotateRight
INST(RorImm, Value, Value, Imm)
INST(RorValue, Value, Value, Value)
// Reverse the byte order within a 16/32/64-bit scalar.
INST(ByteSwap, Value, Value, Imm)
// Bit
INST(BitExtract, Value, Value, Imm, Imm)
INST(BitInsert, Value, Value, Value, Imm, Imm)
INST(BitClear, Value, Value, Imm, Imm)
INST(TestBit, BOOL, Value, Imm)
INST(TestZero, BOOL, Value)
INST(TestNotZero, BOOL, Value)

// Vec4 Add Sub
INST(Vec4Add, Value, Value, Value)
INST(Vec4Sub, Value, Value, Value)
INST(Vec4Mul, Value, Value, Value)

INST(Vec4And, Value, Value, Value)
INST(Vec4Or, Value, Value, Value)

// Lane-agnostic vector bitwise operations.
INST(VecXor, Value, Value, Value)
INST(VecAnd, Value, Value, Value)
INST(VecOr, Value, Value, Value)
INST(VecAndNot, Value, Value, Value)

// Integer vector operations. The immediate is the lane width in bits.
INST(VecAdd, Value, Value, Value, Imm)
INST(VecSub, Value, Value, Value, Imm)
INST(VecCmpEq, Value, Value, Value, Imm)
INST(VecCmpGt, Value, Value, Value, Imm)
INST(VecAvg, Value, Value, Value, Imm)
INST(VecMin, Value, Value, Value, Imm, Imm)
INST(VecMax, Value, Value, Value, Imm, Imm)
INST(VecMul, Value, Value, Value, Imm)
// High 16 bits of eight signed/unsigned 16x16-bit products.
INST(VecMulHigh16, Value, Value, Value, Imm)
// Widening multiply of the EVEN lanes.  Imms are (src_lane_bits, is_signed);
// src_lane_bits is 8, 16 or 32 and the destination lane is twice as wide, so
// destination lane i is widen(a.lane[2i]) * widen(b.lane[2i]) with both source
// lanes read at src_lane_bits and the full 2*src_lane_bits product kept.
//
// A 128-bit IR value cannot hold the widening product of ALL its lanes, so any
// widening multiply has to say WHICH source lanes take part.  Two conventions
// exist in real hardware and this is the lane-indexed one: destination lane i
// comes from source lane 2i, so the operation is width-generic (8/16/32) and
// says nothing about any particular guest ISA.  SVE2's UMULLB/SMULLB are
// exactly this instruction.  AArch64 NEON's UMULL/UMULL2 -- the other
// convention, "the low/high contiguous half of the lanes" -- reaches it with
// one existing VecZip: `UMULL Vd.2D, Vn.2S, Vm.2S` is this opcode applied to
// VecZip(x, x, 32, 0) (ZIP1 puts lanes 0 and 1 into the even positions), and
// UMULL2 the same with VecZip(x, x, 32, 1).  RVV reaches it with a compaction
// (vnsrl) plus vwmul.  Nothing here is x86-shaped: the x86 description "the low
// 32 bits of each 64-bit lane" is the same bits stated in the other direction.
INST(VecMulWiden, Value, Value, Value, Imm, Imm)
INST(VecSatAdd, Value, Value, Value, Imm, Imm)
INST(VecSatSub, Value, Value, Value, Imm, Imm)
// Narrow and concatenate signed lanes; unsigned destination selects PACKUSWB.
INST(VecPack, Value, Value, Value, Imm, Imm)

// Packed integer horizontal operations.
INST(VecAbsDiffSum8, Value, Value, Value)
INST(VecMadd16, Value, Value, Value)

// Packed shifts. The scalar count is the raw x86 imm8 / low qword count.
INST(VecShiftLeft, Value, Value, Value, Imm)
INST(VecShiftRight, Value, Value, Value, Imm)
INST(VecShiftRightArithmetic, Value, Value, Value, Imm)

// Shuffle/duplication operations.
INST(VecShuffle32, Value, Value, Imm)
INST(VecShuffle16, Value, Value, Imm, Imm)
INST(VecZip, Value, Value, Value, Imm, Imm)
INST(VecDupPairs32, Value, Value, Imm)
INST(VecDup64, Value, Value)
INST(VecExtract64, Value, Value, Imm)
INST(VecExtract16, Value, Value, Imm)
INST(VecInsert16, Value, Value, Value, Imm)
INST(VecMovMask, Value, Value, Imm)
INST(VecTableLookup8, Value, Value, Value)

// Scalar f32 arithmetic preserving bits [127:32] of the left operand.
INST(VecFAddScalar32, Value, Value, Value)
INST(VecFSubScalar32, Value, Value, Value)
INST(VecFMulScalar32, Value, Value, Value)
INST(VecFDivScalar32, Value, Value, Value)
INST(VecFAddScalar64, Value, Value, Value)
INST(VecFSubScalar64, Value, Value, Value)
INST(VecFMulScalar64, Value, Value, Value)
INST(VecFDivScalar64, Value, Value, Value)

// Packed IEEE floating-point operations. Imm is the lane width (32 or 64).
INST(VecFAdd, Value, Value, Value, Imm)
INST(VecFSub, Value, Value, Value, Imm)
INST(VecFMul, Value, Value, Value, Imm)
INST(VecFDiv, Value, Value, Value, Imm)
INST(VecFMinMax, Value, Value, Value, Imm, Imm, Imm)
// kind: 0=sqrt, 1=reciprocal estimate, 2=reciprocal-sqrt estimate.
INST(VecFUnary, Value, Value, Value, Imm, Imm, Imm)

// Scalar compare result: bit 0 = CF, bit 1 = PF, bit 2 = ZF.
INST(VecFCmp, Value, Value, Value, Imm)
// Lane-wise compare to an all-ones / all-zeros mask.  The predicate (second
// Imm) is a RELATION SET, not an ISA's compare encoding: an IEEE 754 compare
// of two lanes has exactly four mutually exclusive outcomes, and the predicate
// names the subset for which the lane's mask is set --
//     bit 0 = a < b, bit 1 = a == b, bit 2 = a > b, bit 3 = unordered.
// All 16 subsets are legal: 0 is "never" and 15 is "always".  So `le` is 3,
// `ord` is 7, `neq` (x86's NEQ_UQ, which is true when unordered) is 13, and
// x86's NEQ_OQ, which is not, is 5.  Front ends translate their own encoding
// into this -- for x86 see frontend/x86/fp_cmp_predicate.h, which also records
// why the AVX signalling-vs-quiet dimension is NOT represented here (it is an
// MXCSR exception-flag distinction only, measured on hardware, and SwiftVM
// models no FP exception state).
// Final Imm is scalar (preserve upper lanes) when non-zero.
INST(VecFCmpMask, Value, Value, Value, Imm, Imm, Imm)

// Scalar conversions. The result is the raw IEEE/integer bit pattern in a
// scalar container; widths are supplied as immediates in bits.
INST(VecFCvtIntToFloat, Value, Value, Imm, Imm)
INST(VecFCvtFloatToInt, Value, Value, Imm, Imm, Imm)
INST(VecFCvtScalar, Value, Value, Imm)
// 0=dq2ps, 1=dq2pd, 2=ps2dq, 3=ttps2dq, 4=pd2dq, 5=ttpd2dq,
// 6=ps2pd, 7=pd2ps.
INST(VecFCvtPacked, Value, Value, Imm)

// De-interleave: the exact dual of VecZip, and the primitive every pairwise /
// horizontal operation is built from.  The concatenation {a, b} is viewed as
// two interleaved streams of `lane width` (first Imm) lanes and one of them is
// kept: second Imm 0 selects the EVEN-indexed lanes, non-zero the ODD ones.
// Lanes from `a` fill the low half of the result and lanes from `b` the high
// half, so with four lanes per operand
//     VecUnzip(a, b, w, 0) = [a0 a2 b0 b2]
//     VecUnzip(a, b, w, 1) = [a1 a3 b1 b3]
// and a lane-wise op over the two results is a pairwise op over a and b.  This
// is deliberately a data-movement opcode rather than a fused "pairwise add":
// the arithmetic stays in the existing VecAdd/VecFAdd/VecSatAdd family, so the
// NaN and saturation rules cannot drift between the fused and unfused paths.
// AArch64 lowers it to UZP1/UZP2.
INST(VecUnzip, Value, Value, Value, Imm, Imm)

// Fused multiply-add over floating-point lanes:
//     dst.lane = round( sign_p * (a.lane * b.lane)  +  sign_c * c.lane )
// with ONE rounding of the whole expression.  The exact product feeds the
// addition unrounded, which is the entire reason this cannot be spelled as
// VecFMul followed by VecFAdd: that rounds twice and differs from every FMA
// unit in hardware whenever the exact product lands on a tie.  First Imm is
// the lane width in bits (32 or 64); second Imm is a flag word:
//     bit 0   negate the product before the addition
//     bit 1   negate the addend
// so the four sign combinations are one opcode rather than four.  The x86
// 132/213/231 operand-order numbering is NOT represented here -- that is an
// encoding detail, resolved in the front end into this (a, b, c) order.
//
// Nothing about this is x86-shaped.  AArch64 has FMLA/FMLS (and scalar
// FMADD/FMSUB/FNMADD/FNMSUB, which cover all four sign combinations
// directly); RISC-V V has vfmacc/vfnmacc/vfmsac/vfnmsac -- the same four --
// and scalar RISC-V has fmadd/fmsub/fnmadd/fnmsub; a 32-bit ARM front end
// would reach it from VFMA/VFMS/VFNMA/VFNMS.  A back end without a fused
// unit can still implement it exactly with a double-double product, so the
// opcode is implementable rather than merely expressible everywhere.
//
// SCALAR forms are deliberately absent.  x86's scalar FMA merges the
// untouched lanes from the DESTINATION register, which is a different one of
// the three operands for each of 132/213/231, so no "upper lanes come from
// operand N" rule covers them; the front end computes the packed form and
// merges lane 0 itself.
INST(VecFMulAdd, Value, Value, Value, Value, Imm, Imm)

// Round each floating-point lane to an INTEGRAL VALUE, result still floating
// point.  First Imm is the lane width in bits (32 or 64); second Imm selects
// the rounding mode:
//     0  to nearest, ties to even   1  toward -infinity (floor)
//     2  toward +infinity (ceil)    3  toward zero (truncate)
// Third Imm non-zero means SCALAR: only lane 0 is rounded and the remaining
// lanes come from `merge` (arg 1), the same shape VecFUnary already uses.
//
// The mode is an immediate rather than "whatever the current mode is": an IR
// value must mean the same thing wherever it is scheduled, and a back end is
// free to keep the host rounding mode set to anything.  A guest whose own
// rounding-mode register selects the mode resolves that in its front end (the
// x86 one reads MXCSR.RC and selects among the four results).
//
// This is not an x86 shape.  AArch64 has exactly these four as FRINTN/FRINTM/
// FRINTP/FRINTZ; RISC-V V reaches them through vfcvt with an rm field; C's
// rint/floor/ceil/trunc are the same four functions.  A NaN operand is
// returned quieted and otherwise unchanged, and the sign of a zero result is
// preserved (ceil(-0.5) is -0.0), which is what all of those already do.
INST(VecFRoundInt, Value, Value, Value, Imm, Imm, Imm)

// Upper 64 bits of a 64x64 product.  Imm = 1 for signed, 0 for unsigned.
//
// This is not an x86 convenience: every 64-bit ISA has to produce it and every
// one of them has a single instruction for it, because the full 128-bit
// product does not fit a register.  AArch64 has SMULH/UMULH, RISC-V has
// MULH/MULHU, x86 gets it in RDX from one-operand MUL/IMUL.  A front end for
// any of the three needs this; a back end for any of the three emits one
// instruction.  Without it the low and high halves cannot both be expressed,
// so the high half had to go through a host call -- measured at 3.27x slower
// than the same loop without the multiply (docs/perf-baseline.md 5.1).
//
// Deliberately separate from Mul rather than a second result on it: IR values
// are single-valued, and the two halves are wanted independently often enough
// (x86 flags need only the high half's zero-ness) that fusing them would just
// force dead code.  The narrow widths do not use this -- an 8/16/32-bit high
// half is already reachable by widening to 64 and shifting.
INST(MulHigh, Value, Value, Value, Imm)

// Materialize a condition as the integer 1 (true) or 0 (false).
//
// Reads the condition flags exactly the way CondSelect does -- same Cond
// vocabulary, same implicit NZCV source -- so FlagsEliminationPass must keep
// it in the same bucket as CondSelect, and both back ends must resolve it
// against the same flag state.
//
// This exists as its own opcode rather than sugar over CondSelect because
// "condition -> 0/1" is a single instruction on every back end that matters,
// while the CondSelect spelling forces the front end to materialize two
// constants first:  AArch64 has CSET, x86 has SETcc, RISC-V reaches it with
// the SLT/SLTU family (and after a compare-into-register the value IS the
// flag).  Nothing here is x86-shaped -- AArch64's CSET and ARM's IT/MOV pair
// want exactly this, and any front end with a "flag as value" operation
// (x86 SETcc, AArch64 CSET, ARM MRS+shift) lowers into it.
//
// The result is defined as exactly 0 or 1, not "any non-zero value", so a
// front end may use it as an arithmetic operand.  Note what that is and is not
// backed by today: "CondSet materializes exactly one for arithmetic consumers"
// in tests/main_case.cpp feeds the result to Add and inspects the direct JIT
// lowering.  In particular, changing CSET to CSETM makes that directed test
// fail; truthiness-only guest consumers cannot distinguish 1 from all ones.
//
// Measured motivation: the x86 front end reached every NZCV-shaped condition
// through LoadImm(1) + LoadImm(0) + CondSelect, and LoadImm is the single
// most numerous IR opcode in the corpus at 15.15%
// (docs/ir-expansion-attribution.md 2).
INST(CondSet, BOOL, Cond)
