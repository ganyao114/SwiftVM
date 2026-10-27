// IR instruction table — the single source of truth for instruction signatures.
// Format: INST(Name, RetToken, ResultPolicy, ArgTokens...)
//   Name      -> OpCode::Name, builder method name, Emit##Name/Run##Name
//   RetToken  -> C++ return type in builders (args.h: Void, Value, BOOL, U32, U64)
//                AND ArgType / default result ValueType via ir_meta.h
//   ResultPolicy -> Void, Fixed, Arg0, Explicit or Call (ir_types.h). Arg0
//                   uses only logical argument 0; Explicit/Call require a type
//                   at construction. Call alone permits an explicit VOID result.
//   ArgTokens -> ArgSignature names (ir_types.h). OptionalData accepts Value
//                or Imm; OptionalOperand accepts one complete Operand. Both
//                may be absent only as a trailing suffix.
// Slot rule:   Operand = 3 physical slots, everything else 1 (ir_types.h PhysicalSlots).

// Read Write
INST(DefineLocal, Void, Void, Local)
INST(LoadLocal, Value, Arg0, Local)
INST(StoreLocal, Void, Void, Local, Value)
INST(LoadUniform, Value, Arg0, Uniform)
INST(StoreUniform, Void, Void, Uniform, Value)
// Host address of a byte within the current uniform buffer.  This is only for
// stateful frontend helpers that need to update several architectural fields
// atomically (x87); ordinary guest code must keep using Load/StoreUniform.
INST(GetUniformAddress, Value, Explicit, Imm)
// Stateful x87 operation. The dedicated opcode keeps x87 out of the generic
// CallLambda classification so ARM64 can inline selected commands and
// whole-function compilation can retain blocks containing x87.
INST(X87Op, Value, Explicit, Value, Imm, Value)
INST(LoadMemory, Value, Explicit, Operand)
INST(StoreMemory, Void, Void, Operand, Value)
INST(LoadMemoryTSO, Value, Explicit, Operand)
INST(StoreMemoryTSO, Void, Void, Operand, Value)
INST(MemoryCopy, Void, Void, Lambda, Lambda, Imm)
INST(MemoryCopyTSO, Void, Void, Lambda, Lambda, Imm)
INST(MemoryBarrierTSO, Void, Void)
INST(CompareAndSwap, Value, Explicit, Value, Value, Value)
// 128-bit CAS takes scalar halves to stay within the IR's five physical-arg
// limit and returns the observed {high,low} pair as V128.
INST(CompareAndSwap128, Value, Explicit, Value, Value, Value, Value, Value)
// Dynamic architectural-alignment check. A failure terminates the current
// guest block with PageFatal; the frontend sets the precise guest location
// immediately before it.
INST(CheckMemoryAlignment, Void, Void, Value, Imm)
INST(AtomicExchange, Value, Explicit, Value, Value)
INST(AtomicFetchAdd, Value, Explicit, Value, Value)
INST(AtomicRMW, Value, Explicit, Imm, Value, Value, Value)

// Help
INST(UniformBarrier, Void, Void)
// Metadata marker between the two fixed-home publications of register XCHG.
// The immediate packs the two parent guest-GPR indices in nibbles; the
// allocator uses the first target as the point-clobber boundary.
INST(XchgBarrier, Void, Void, Imm)
// Conditions observe every bit of their integer read view: zero is false,
// every other value is true. Raw bits outside that view do not participate.
// Selection arms retain their existing value-transport contract.
INST(Goto, Value, Arg0, BOOL)
INST(NotGoto, Value, Arg0, BOOL)
INST(Select, Value, Explicit, BOOL, Value, Value)
INST(SelectZero, Value, Explicit, Value, Value, Value)
INST(CondSelect, Value, Explicit, Cond, Value, Value)
INST(BindLabel, Void, Void, Value)
INST(CallLambda, Value, Call, Lambda, OptionalData, OptionalData, OptionalData)
// SSE4.2 PCMPISTRI/M common core.  Both operands are V128 and imm8 is a
// compile-time immediate; the result packs IntRes2, index and ZF/SF/CF/OF.
// PCMPESTRI/M retain their helper path because their explicit lengths are
// dynamic and are not the W58 hot path.
INST(Sse42Str, Value, Explicit, Value, Value, Imm)
INST(CallLocation, Value, Call, Lambda, Params)
INST(CallDynamic, Value, Call, Lambda, Params)
INST(AddPhi, Value, Explicit, Params)
INST(Nop, Void, Void)
INST(AdvancePC, Void, Void, Imm)
INST(SetLocation, Void, Void, Lambda)
INST(GetLocation, Value, Explicit)
INST(Zero, Value, Explicit)
INST(GetHostGPR, Value, Explicit, Imm, Imm)
INST(GetHostFPR, Value, Explicit, Imm, Imm)
INST(SetHostGPR, Void, Void, Value, Imm, Imm)
INST(SetHostFPR, Void, Void, Value, Imm, Imm)
INST(BitCast, Value, Arg0, Value)
INST(GetOperand, Value, Explicit, Operand)
INST(GetResult, Value, Explicit, Value, Value)

// Imm
INST(LoadImm, Value, Arg0, Imm)

// Help
INST(PushRSB, Void, Void, Lambda)
INST(CallReturn, Void, Void, Value, Imm, Lambda)
INST(PopRSB, Void, Void, Lambda)

// Calculator
INST(Add, Value, Arg0, Value, Operand)
INST(Sub, Value, Arg0, Value, Operand)
// With Carry
INST(Adc, Value, Arg0, Value, Operand)
INST(Sbb, Value, Arg0, Value, Operand)
// Mul & Div
INST(Mul, Value, Arg0, Value, Operand)
INST(MulSub, Value, Arg0, Value, Value, Value)
INST(Div, Value, Arg0, Value, Operand)
INST(SignedDiv64, U64, Fixed, Value, Value)
INST(Div128, U64, Fixed, Value, Value, Value, Imm)
INST(Div128Remainder, U64, Fixed, Value)
INST(Cpuid, U64, Fixed, Value, Value, Imm, Imm)
INST(CpuidUpper, U64, Fixed, Value)
// Integer conversions read only the input view's bits. The 32-bit forms
// additionally truncate a wider input to 32; SignExtend replicates the view's
// sign bit. An explicit integer result view keeps its low result-width bits.
// BitCast/GetResult remain raw transport and do not apply this normalization.
INST(ZeroExtend32, U32, Fixed, Value)
// Zero-extend the low 32 bits and retain a U64 result. This is distinct from
// changing ZeroExtend32's return type: backends must still perform the 32-bit
// truncation before the value is consumed as U64.
INST(ZeroExtend32To64, U64, Fixed, Value)
INST(ZeroExtend64, U64, Fixed, Value)
INST(SignExtend, Value, Explicit, Value)
// And & Or
INST(And, Value, Arg0, Value, Operand)
INST(Or, Value, Arg0, Value, Operand)
INST(Xor, Value, Arg0, Value, Operand)
// Unary Not is a logical zero test. With an Operand, it complements that
// operand's bits and retains the first Value as the result-type source.
INST(Not, Value, Arg0, Value, OptionalOperand)
INST(AndNot, Value, Arg0, Value, Operand)
// Flags from result
INST(GetFlags, Value, Explicit, Value, Flags)
INST(SaveFlags, Void, Void, Value, Flags)
// A SaveFlags-shaped pseudo whose value is consumed only by the immediately
// following terminal condition.  The interpreter materializes the requested
// bits exactly like SaveFlags.  ARM64 instead keeps the producer's NZCV live
// for LocalCondSet and emits no architectural flags-word write.
//
// FlagsEliminationPass is the only producer of this IR and only after proving
// both CFG successors have dead flags-in.  It must never cross a unit edge.
INST(BranchOnlyFlags, Void, Void, Value, Flags)
INST(TestFlags, BOOL, Fixed, Flags)
INST(TestNotFlags, BOOL, Fixed, Flags)
INST(ClearFlags, Void, Void, Flags)
// Set a single guest flag (C / V) from a computed 0/1 value. Used where the
// flag is derived rather than produced by a host flag-setting op (shift CF/OF).
INST(SetCarry, Void, Void, Value)
INST(SetOverflow, Void, Void, Value)
// Toggle the current carry representation in place. FlagM units use this to
// keep a Direct cross-block carry ABI and to adjust ADC/SBB inputs; the
// interpreter toggles the matching virtual host-C bit.
INST(InvertCarry, Void, Void)
// Publish VecFCmp's result to the unchanged x86 flags ABI, clearing OF/SF/AF
// as UCOMIS/COMIS require.  Imm records the unit-local lowering selected by
// the frontend: 0 consumes the legacy {CF,PF,ZF} packed value, 1 consumes the
// compact path's ordered bit and AXFLAG-transformed NZCV.  The interpreter
// always retains architectural VecFCmp semantics.
INST(PublishFCmpFlags, Void, Void, Value, Imm)
INST(PublishSse42StrFlags, Void, Void, Value, Flags)
// Shift
// Scalar shifts operate in the result width. Read only the input view's low
// min(source, result) bits, zero-extending them (sign-extending for ASR).
// Immediate and variable counts both reduce modulo the result width. Narrow
// guest shifts needing 32-bit counts must use an explicit 32-bit container.
// LogicalShiftLeft
INST(LslImm, Value, Arg0, Value, Imm)
INST(LslValue, Value, Arg0, Value, Value)
// LogicalShiftRight
INST(LsrImm, Value, Arg0, Value, Imm)
INST(LsrValue, Value, Arg0, Value, Value)
// ArithmeticShiftRight
INST(AsrImm, Value, Arg0, Value, Imm)
INST(AsrValue, Value, Arg0, Value, Value)
// RotateRight
INST(RorImm, Value, Arg0, Value, Imm)
INST(RorValue, Value, Arg0, Value, Value)
// Reverse the byte order within a 16/32/64-bit scalar.
INST(ByteSwap, Value, Arg0, Value, Imm)
INST(CountLeadingZeros64, U64, Fixed, Value)
INST(CountTrailingZeros64, U64, Fixed, Value)
INST(CountLeadingZeros32, U32, Fixed, Value)
INST(CountTrailingZeros32, U32, Fixed, Value)
// Bit
INST(BitExtract, Value, Arg0, Value, Imm, Imm)
INST(BitInsert, Value, Arg0, Value, Value, Imm, Imm)
INST(BitClear, Value, Arg0, Value, Imm, Imm)
// Integer predicates return scalar 0/1 and read the input's logical width,
// independently of the result width or the input's signed token. TestBit's
// full integer index is in [0, 64); an index outside the input view reads 0.
// Materialized predicate results contain a complete scalar 0/1, including
// bits outside a narrow result view; this is not a rule for raw aliases.
INST(TestBit, BOOL, Fixed, Value, Imm)
INST(TestZero, BOOL, Fixed, Value)
INST(TestNotZero, BOOL, Fixed, Value)

// Vec4 Add Sub
INST(Vec4Add, Value, Arg0, Value, Value)
INST(Vec4Sub, Value, Arg0, Value, Value)
INST(Vec4Mul, Value, Arg0, Value, Value)

INST(Vec4And, Value, Arg0, Value, Value)
INST(Vec4Or, Value, Arg0, Value, Value)

// Lane-agnostic vector bitwise operations.
INST(VecXor, Value, Arg0, Value, Value)
INST(VecAnd, Value, Arg0, Value, Value)
INST(VecOr, Value, Arg0, Value, Value)
INST(VecAndNot, Value, Arg0, Value, Value)

// Integer vector operations. The immediate is the lane width in bits.
INST(VecAdd, Value, Arg0, Value, Value, Imm)
INST(VecSub, Value, Arg0, Value, Value, Imm)
INST(VecCmpEq, Value, Arg0, Value, Value, Imm)
INST(VecCmpGt, Value, Arg0, Value, Value, Imm)
INST(VecAvg, Value, Arg0, Value, Value, Imm)
INST(VecMin, Value, Arg0, Value, Value, Imm, Imm)
INST(VecMax, Value, Arg0, Value, Value, Imm, Imm)
INST(VecMul, Value, Arg0, Value, Value, Imm)
// High 16 bits of eight signed/unsigned 16x16-bit products.
INST(VecMulHigh16, Value, Arg0, Value, Value, Imm)
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
INST(VecMulWiden, Value, Explicit, Value, Value, Imm, Imm)
INST(VecSatAdd, Value, Arg0, Value, Value, Imm, Imm)
INST(VecSatSub, Value, Arg0, Value, Value, Imm, Imm)
// Narrow and concatenate signed lanes; unsigned destination selects PACKUSWB.
INST(VecPack, Value, Arg0, Value, Value, Imm, Imm)

// Packed integer horizontal operations.
INST(VecAbsDiffSum8, Value, Arg0, Value, Value)
INST(VecMadd16, Value, Arg0, Value, Value)

// Packed shifts. The scalar count is the raw x86 imm8 / low qword count.
INST(VecShiftLeft, Value, Arg0, Value, Value, Imm)
INST(VecShiftRight, Value, Arg0, Value, Value, Imm)
INST(VecShiftRightArithmetic, Value, Arg0, Value, Value, Imm)
// Immediate forms keep a compile-time x86 count out of the scalar register
// allocator and map directly to NEON SHL/USHR/SSHR.  The first Imm is the raw
// count and the second is the lane width.
INST(VecShiftLeftImm, Value, Arg0, Value, Imm, Imm)
INST(VecShiftRightImm, Value, Arg0, Value, Imm, Imm)
INST(VecShiftRightArithmeticImm, Value, Arg0, Value, Imm, Imm)
// Whole-128-bit byte shift.  `zero` is an explicit SSA input so one zero
// vector can be shared safely within a straight-line block region.  The Imms
// are byte count (1..15) and direction (nonzero = left).
INST(VecByteShift, Value, Arg0, Value, Value, Imm, Imm)

// Shuffle/duplication operations.
INST(VecShuffle32, Value, Arg0, Value, Imm)
// Legacy SHUFPS: result lanes 0..1 select from `left`, lanes 2..3 from
// `right`; each 2-bit imm8 field selects one dword of its respective source.
INST(VecShuffle32TwoSrc, Value, Arg0, Value, Value, Imm)
// Block-cached 128-bit constants and an indexed PSHUFD form. Keeping the
// indexes as SSA lets the constant-folding pass reuse one materialization for
// repeated controls without pinning an emitter scratch register.
INST(VecLoadConst, Value, Explicit, Imm, Imm)
INST(VecShuffle32Indexed, Value, Arg0, Value, Value)
// One-instruction all-zero vector, block-local CSE-able like VecLoadConst.
INST(VecSharedZero, Value, Explicit)
INST(VecShuffle16, Value, Arg0, Value, Imm, Imm)
INST(VecExtractBytes, Value, Arg0, Value, Value, Imm)
INST(VecZip, Value, Arg0, Value, Value, Imm, Imm)
INST(VecDupPairs32, Value, Arg0, Value, Imm)
INST(VecDup64, Value, Explicit, Value)
INST(VecExtract64, Value, Explicit, Value, Imm)
INST(VecExtract16, Value, Explicit, Value, Imm)
INST(VecInsert16, Value, Arg0, Value, Value, Imm)
INST(VecMovMask, Value, Explicit, Value, Imm)
INST(VecTableLookup8, Value, Arg0, Value, Value)

// 128-bit crypto primitives.  These are deliberately whole-instruction IR
// operations: the ARM64 backend keeps them in SIMD registers and emits the
// architectural Crypto Extension instructions, rather than routing a hot
// guest instruction through a host helper.
INST(VecAesEnc, Value, Arg0, Value, Value)
INST(VecAesEncLast, Value, Arg0, Value, Value)
INST(VecAesDec, Value, Arg0, Value, Value)
INST(VecAesDecLast, Value, Arg0, Value, Value)
// FEX-style round forms take a shared zero-key vector explicitly.  They also
// permit the allocator to tie the result to the dying state input.
INST(VecAesEncFast, Value, Arg0, Value, Value, Value)
INST(VecAesEncLastFast, Value, Arg0, Value, Value, Value)
INST(VecAesDecFast, Value, Arg0, Value, Value, Value)
INST(VecAesDecLastFast, Value, Arg0, Value, Value, Value)
INST(VecAesKeygenAssist, Value, Arg0, Value, Imm)
INST(VecPclMul, Value, Arg0, Value, Value, Imm)
INST(VecSha256Msg1, Value, Arg0, Value, Value)
INST(VecSha256Msg2, Value, Arg0, Value, Value)
INST(VecSha256Rnds2, Value, Arg0, Value, Value, Value)

// Scalar f32 arithmetic preserving bits [127:32] of the left operand.
// Ordinary FP arithmetic and SQRT permit native quiet-NaN sign and payload
// selection; ARM64 follows FEX here. Finite/zero/infinity results retain
// their opcode semantics. This policy does not relax MIN/MAX selection,
// comparison masks, conversion sentinels or preservation of unwritten lanes.
INST(VecFAddScalar32, Value, Arg0, Value, Value)
INST(VecFSubScalar32, Value, Arg0, Value, Value)
INST(VecFMulScalar32, Value, Arg0, Value, Value)
INST(VecFDivScalar32, Value, Arg0, Value, Value)
INST(VecFAddScalar64, Value, Arg0, Value, Value)
INST(VecFSubScalar64, Value, Arg0, Value, Value)
INST(VecFMulScalar64, Value, Arg0, Value, Value)
INST(VecFDivScalar64, Value, Arg0, Value, Value)

// Packed IEEE floating-point operations. Imm is the lane width (32 or 64).
INST(VecFAdd, Value, Arg0, Value, Value, Imm)
INST(VecFSub, Value, Arg0, Value, Value, Imm)
INST(VecFMul, Value, Arg0, Value, Value, Imm)
INST(VecFDiv, Value, Arg0, Value, Value, Imm)
INST(VecFMinMax, Value, Arg0, Value, Value, Imm, Imm, Imm)
// kind: 0=sqrt, 1=reciprocal estimate, 2=reciprocal-sqrt estimate.
INST(VecFUnary, Value, Arg0, Value, Value, Imm, Imm, Imm)

// Scalar compare result: bit 0 = CF, bit 1 = PF, bit 2 = ZF.
// The final Imm records whether this unit selected the FlagM2 compact
// lowering.  It is deliberately in IR rather than re-checked by the backend,
// so one unit cannot mix paths if process state changes while compiling.
INST(VecFCmp, Value, Explicit, Value, Value, Imm, Imm)
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
INST(VecFCmpMask, Value, Arg0, Value, Value, Imm, Imm, Imm)

// Scalar conversions. The result is the raw IEEE/integer bit pattern in a
// scalar container; widths are supplied as immediates in bits.
INST(VecFCvtIntToFloat, Value, Explicit, Value, Imm, Imm)
INST(VecFCvtFloatToInt, Value, Explicit, Value, Imm, Imm, Imm)
INST(VecFCvtScalar, Value, Explicit, Value, Imm)
// 0=dq2ps, 1=dq2pd, 2=ps2dq, 3=ttps2dq, 4=pd2dq, 5=ttpd2dq,
// 6=ps2pd, 7=pd2ps.
INST(VecFCvtPacked, Value, Explicit, Value, Imm)

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
INST(VecUnzip, Value, Arg0, Value, Value, Imm, Imm)

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
INST(VecFMulAdd, Value, Arg0, Value, Value, Value, Imm, Imm)

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
INST(VecFRoundInt, Value, Arg0, Value, Value, Imm, Imm, Imm)

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
INST(MulHigh, Value, Explicit, Value, Value, Imm)

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
INST(CondSet, BOOL, Fixed, Cond)
// Same architectural result as CondSet, but emitted only when the frontend has
// proved that the immediately preceding guest flag producer is in this unit
// and no intervening guest instruction can have clobbered host NZCV.  A sole
// branch/select consumer can therefore fold this marker to B.cond/CSEL.
INST(LocalCondSet, BOOL, Fixed, Cond)
// Parity condition over the low byte of a named local producer. Imm is 0 for
// x86 PF (even parity) and 1 for its inverse. Unlike TestFlags this does not
// read or write the architectural flags word.
INST(LocalParitySet, BOOL, Fixed, Value, Imm)
// Decoder-side conservative proof that both direct terminal-Jcc successors
// kill incoming arithmetic flags before any read, fault, helper or control
// edge. FlagsEliminationPass consumes and removes this marker; backends keep
// no-op implementations as a defensive fallback.
INST(BranchOnlyEdges, Void, Void)
// Condition over the host FP NZCV produced by the named VecFCmp.  The Value
// edge pins the producer and gives the interpreter the compact comparison
// result; ARM64 can fold a sole branch/select consumer directly into the
// already-emitted FCMP.
INST(FCmpCondSet, BOOL, Fixed, Value, Cond)

// Unit-local canonical form for `0 - value`. FoldValues creates it only
// under SVM_INT_IMM_FOLD after proving the zero LoadImm has this sole use.
// Keeping it at the end preserves every pre-existing opcode number.
INST(Neg, Value, Arg0, Value)

// Count the set bits in the complete scalar input. Narrow inputs must be
// zero-extended by the frontend before using this U64 operation.
INST(PopCount, U64, Fixed, Value)
// CRC-32C without pre/post complement. Only the accumulator's low 32 bits
// and the data's low Imm bits (8, 16, 32 or 64) participate; result is U32.
INST(Crc32c, U32, Fixed, Value, Value, Imm)
