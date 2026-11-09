// Neutral IR opcode signatures. This declaration generates OpCode and Schema.
// Format: INST(Name, RetToken, ResultPolicy, Effects, ArgTokens...)
// RetToken names a default result type; ResultPolicy determines how the
// constructor resolves it. Explicit requires a caller-supplied type; Call
// also permits Void. Integer policies require an integer result view.
// Effects declare implicit reads/writes, ordering and faults once per opcode.
// Unknown is conservative; helper traits never imply purity by their bit pattern.
// ArgTokens describe logical arguments, independent of the stored layout.
// Value arguments retain their consumer view; IntValue/IntOperand constrain
// that view's domain, and Int32Value/Int64Value additionally constrain width.
// OptionalData accepts a Value or Immediate. OptionalOperand accepts an atom
// or a complete expression. Optional arguments may be absent as a suffix.
// Storage uses compact words, defined by types.h and program.h. A direct
// operand has no Identity header; real expressions/Params/Lambda retain their
// actual descriptor and children. Full-width payloads use an extension word.
// Physical word counts are not logical argument counts or Value-use counts.

// Read Write
INST(DefineLocal, Void, Void, (Effect::WriteLocal), Local)
INST(LoadLocal, Value, Arg0, (Effect::ReadLocal), Local)
INST(StoreLocal, Void, Void, (Effect::WriteLocal), Local, Value)
INST(LoadUniform, Value, Arg0, (Effect::ReadGuestState), Uniform)
// Scalar stores read the value's logical view, zero-extend it to the uniform
// width and write that width. A U32 view into a U64 slot therefore clears its
// high half without an extra conversion node. Raw transport opcodes remain
// explicit exceptions to logical-view reads.
INST(StoreUniform, Void, Void, (Effect::WriteGuestState), Uniform, Value)
// Host address of a byte within the current uniform buffer.  This is only for
// stateful frontend helpers that need to update several architectural fields
// atomically (x87); ordinary guest code must keep using Load/StoreUniform.
INST(GetUniformAddress, Value, Explicit, (Effect::ReadGuestState), Imm)
// Stateful x87 operation. The dedicated opcode keeps x87 out of the generic
// CallLambda classification so ARM64 can inline selected commands and
// whole-function compilation can retain blocks containing x87.
INST(X87Op,
     Value,
     Explicit,
     (Effect::ReadGuestState | Effect::WriteGuestState | Effect::External | Effect::Control),
     Value,
     Imm,
     Value)
INST(LoadMemory, Value, Explicit, (Effect::ReadMemory | Effect::MayFault), Operand)
// Exact scalar access width and extension semantics live in the descriptor
// (load_extend.h); ReturnType describes the final 32/64-bit integer result.
INST(LoadMemoryExtend,
     Value,
     IntegerExplicit,
     (Effect::ReadMemory | Effect::MayFault),
     IntOperand,
     IntImm)
INST(StoreMemory, Void, Void, (Effect::WriteMemory | Effect::MayFault), Operand, Value)
INST(LoadMemoryTSO,
     Value,
     Explicit,
     (Effect::ReadMemory | Effect::MayFault | Effect::Ordered),
     Operand)
INST(LoadMemoryExtendTSO,
     Value,
     IntegerExplicit,
     (Effect::ReadMemory | Effect::MayFault | Effect::Ordered),
     IntOperand,
     IntImm)
INST(StoreMemoryTSO,
     Void,
     Void,
     (Effect::WriteMemory | Effect::MayFault | Effect::Ordered),
     Operand,
     Value)
INST(MemoryCopy, Void, Void, (Effect::Unknown), Lambda, Lambda, Imm)
INST(MemoryCopyTSO, Void, Void, (Effect::Unknown), Lambda, Lambda, Imm)
INST(MemoryBarrierTSO, Void, Void, (Effect::Ordered))
// Scalar locked operations return the observed old value at the requested
// width. CAS inputs are address/expected/desired; failure leaves memory alone.
// All paths retain full locked ordering, including a failed comparison.
INST(CompareAndSwap,
     Value,
     Explicit,
     (Effect::ReadMemory | Effect::WriteMemory | Effect::MayFault | Effect::Ordered),
     Value,
     Value,
     Value)
// 128-bit CAS takes scalar halves to stay within the IR's five physical-arg
// limit and returns the observed {high,low} pair as V128.
INST(CompareAndSwap128,
     Value,
     Explicit,
     (Effect::ReadMemory | Effect::WriteMemory | Effect::MayFault | Effect::Ordered),
     Value,
     Value,
     Value,
     Value,
     Value)
// Dynamic architectural-alignment check. A failure terminates the current
// guest block with PageFatal; the frontend sets the precise guest location
// immediately before it.
INST(CheckMemoryAlignment, Void, Void, (Effect::MayFault | Effect::Control), Value, Imm)
INST(AtomicExchange,
     Value,
     Explicit,
     (Effect::ReadMemory | Effect::WriteMemory | Effect::MayFault | Effect::Ordered),
     Value,
     Value)
INST(AtomicFetchAdd,
     Value,
     Explicit,
     (Effect::ReadMemory | Effect::WriteMemory | Effect::MayFault | Effect::Ordered),
     Value,
     Value)
// AtomicRMWOp immediate, address, operand, explicit carry (see types.h).
INST(AtomicRMW,
     Value,
     Explicit,
     (Effect::ReadMemory | Effect::WriteMemory | Effect::MayFault | Effect::Ordered),
     Imm,
     Value,
     Value,
     Value)

// Help
INST(UniformBarrier, Void, Void, (Effect::Ordered))
// Metadata marker between the two fixed-home publications of register XCHG.
// The immediate packs the two parent guest-GPR indices in nibbles; the
// allocator uses the first target as the point-clobber boundary.
INST(XchgBarrier, Void, Void, (Effect::Ordered), Imm)
// Conditions observe every bit of their integer read view: zero is false,
// every other value is true. Raw bits outside that view do not participate.
// Selection arms retain their existing value-transport contract.
INST(Goto, Value, Arg0, (Effect::Control), IntValue)
INST(NotGoto, Value, Arg0, (Effect::Control), IntValue)
INST(Select, Value, Explicit, (Effect::None), IntValue, Value, Value)
INST(SelectZero, Value, Explicit, (Effect::None), IntValue, Value, Value)
INST(CondSelect, Value, Explicit, (Effect::ReadFlags), Cond, Value, Value)
INST(BindLabel, Void, Void, (Effect::Control), Value)
INST(CallLambda, Value, Call, (Effect::Unknown), Lambda, OptionalData, OptionalData, OptionalData)
// SSE4.2 PCMPISTRI/M common core.  Both operands are V128 and imm8 is a
// compile-time immediate; the result packs IntRes2, index and ZF/SF/CF/OF.
// PCMPESTRI/M retain their helper path because their explicit lengths are
// dynamic and are not the W58 hot path.
INST(Sse42Str, Value, Explicit, (Effect::None), Value, Value, Imm)
INST(CallLocation, Value, Call, (Effect::Unknown), Lambda, Params)
INST(CallDynamic, Value, Call, (Effect::Unknown), Lambda, Params)
INST(AddPhi, Value, Explicit, (Effect::Control), Params)
INST(Nop, Void, Void, (Effect::None))
INST(AdvancePC, Void, Void, (Effect::WriteGuestState), Imm)
INST(SetLocation, Void, Void, (Effect::WriteGuestState), Lambda)
INST(GetLocation, Value, Explicit, (Effect::ReadGuestState))
INST(Zero, Value, Explicit, (Effect::None))
// Accrued numerical exceptions: invalid, denormal input, divide-by-zero,
// overflow, underflow and inexact in bits 0..5. Merge the explicit canonical
// status shadow (Value U32); hosts without an input-denormal bit retain it.
INST(ReadFPStatus, U32, IntegerFixed, (Effect::ReadHostState), Value)
INST(GetHostGPR, Value, Explicit, (Effect::ReadHostState), Imm, Imm)
INST(GetHostFPR, Value, Explicit, (Effect::ReadHostState), Imm, Imm)
INST(SetHostGPR, Void, Void, (Effect::WriteHostState), Value, Imm, Imm)
INST(SetHostFPR, Void, Void, (Effect::WriteHostState), Value, Imm, Imm)
INST(BitCast, Value, Arg0, (Effect::None), Value)
INST(GetOperand, Value, Explicit, (Effect::None), Operand)
INST(GetResult, Value, Explicit, (Effect::None), Value, Value)

// Imm
INST(LoadImm, Value, Arg0, (Effect::None), Imm)

// Help
INST(PushRSB, Void, Void, (Effect::Unknown), Lambda)
INST(CallReturn, Void, Void, (Effect::Unknown), Value, Imm, Lambda)
INST(PopRSB, Void, Void, (Effect::Unknown), Lambda)

// Calculator
INST(Add, Value, IntegerArg0, (Effect::None), IntValue, IntOperand)
INST(Sub, Value, IntegerArg0, (Effect::None), IntValue, IntOperand)
// With Carry
INST(Adc, Value, IntegerArg0, (Effect::ReadFlags), IntValue, IntOperand)
INST(Sbb, Value, IntegerArg0, (Effect::ReadFlags), IntValue, IntOperand)
// Mul & Div
INST(Mul, Value, IntegerArg0, (Effect::None), IntValue, IntOperand)
INST(MulSub, Value, IntegerArg0, (Effect::None), IntValue, IntValue, IntValue)
INST(Div, Value, IntegerArg0, (Effect::MayFault), IntValue, IntOperand)
INST(SignedDiv64, U64, IntegerFixed, (Effect::MayFault), IntValue, IntValue)
// A 2N-bit high:low dividend divided by an N-bit divisor, where all three
// value views have the same N (8/16/32/64). Imm selects signed (1)/unsigned
// (0). Division by zero and a quotient outside N bits fault. Quotient and
// remainder are low-N-bit patterns in zero-extended U64 carriers.
INST(Div128, U64, IntegerFixed, (Effect::MayFault), IntValue, IntValue, IntValue, IntImm)
INST(Div128Remainder, U64, Fixed, (Effect::None), Value)
// Serialized guest CPU query: leaf, subleaf, immutable model callback.
// One callback returns {EAX|EBX<<32, ECX|EDX<<32}; CpuidUpper reads that
// producer's second result. No unused legacy feature-mask argument remains.
INST(Cpuid, U64, Fixed, (Effect::Unknown), Value, Value, Imm)
INST(CpuidUpper, U64, Fixed, (Effect::Unknown), Value)
// Integer conversions read only the input view's bits. The 32-bit forms
// additionally truncate a wider input to 32; SignExtend replicates the view's
// sign bit. An explicit integer result view keeps its low result-width bits.
// BitCast/GetResult remain raw transport and do not apply this normalization.
INST(ZeroExtend32, U32, IntegerFixed, (Effect::None), IntValue)
// Zero-extend the low 32 bits and retain a U64 result. This is distinct from
// changing ZeroExtend32's return type: backends must still perform the 32-bit
// truncation before the value is consumed as U64.
INST(ZeroExtend32To64, U64, IntegerFixed, (Effect::None), IntValue)
INST(ZeroExtend64, U64, IntegerFixed, (Effect::None), IntValue)
INST(SignExtend, Value, IntegerExplicit, (Effect::None), IntValue)
// And & Or
INST(And, Value, IntegerArg0, (Effect::None), IntValue, IntOperand)
INST(Or, Value, IntegerArg0, (Effect::None), IntValue, IntOperand)
INST(Xor, Value, IntegerArg0, (Effect::None), IntValue, IntOperand)
// Unary Not is a logical zero test. With an Operand, it complements that
// operand's bits and retains the first Value as the result-type source.
INST(Not, Value, IntegerArg0, (Effect::None), IntValue, OptionalIntOperand)
INST(AndNot, Value, IntegerArg0, (Effect::None), IntValue, IntOperand)
// Flags from result
INST(GetFlags, Value, Explicit, (Effect::ReadFlags), Value, Flags)
INST(SaveFlags, Void, Void, (Effect::WriteFlags), Value, Flags)
// A SaveFlags-shaped pseudo whose value is consumed only by the immediately
// following terminal condition.  The interpreter materializes the requested
// bits exactly like SaveFlags.  ARM64 instead keeps the producer's NZCV live
// for LocalCondSet and emits no architectural flags-word write.
//
// FlagsEliminationPass is the only producer of this IR and only after proving
// both CFG successors have dead flags-in.  It must never cross a unit edge.
INST(BranchOnlyFlags, Void, Void, (Effect::WriteFlags | Effect::Ordered), Value, Flags)
INST(TestFlags, BOOL, Fixed, (Effect::ReadFlags), Flags)
INST(TestNotFlags, BOOL, Fixed, (Effect::ReadFlags), Flags)
INST(ClearFlags, Void, Void, (Effect::WriteFlags), Flags)
// Set a single guest flag (C / V) from a computed 0/1 value. Used where the
// flag is derived rather than produced by a host flag-setting op (shift CF/OF).
INST(SetCarry, Void, Void, (Effect::WriteFlags), Value)
INST(SetOverflow, Void, Void, (Effect::WriteFlags), Value)
// Toggle the current carry representation in place. FlagM units use this to
// keep a Direct cross-block carry ABI and to adjust ADC/SBB inputs; the
// interpreter toggles the matching virtual host-C bit.
INST(InvertCarry, Void, Void, (Effect::ReadFlags | Effect::WriteFlags))
// Publish VecFCmp's neutral bit0 CF / bit1 PF / bit2 ZF result, clearing
// OF/SF/AF as UCOMIS/COMIS require. No physical flags encoding enters IR.
INST(PublishFCmpFlags, Void, Void, (Effect::WriteFlags), Value)
INST(PublishSse42StrFlags, Void, Void, (Effect::WriteFlags), Value, Flags)
// Shift
// Scalar shifts operate in the result width. Read only the input view's low
// min(source, result) bits, zero-extending them (sign-extending for ASR).
// Immediate and variable counts both reduce modulo the result width. Narrow
// guest shifts needing 32-bit counts must use an explicit 32-bit container.
// LogicalShiftLeft
INST(LslImm, Value, IntegerArg0, (Effect::None), IntValue, IntImm)
INST(LslValue, Value, IntegerArg0, (Effect::None), IntValue, IntValue)
// LogicalShiftRight
INST(LsrImm, Value, IntegerArg0, (Effect::None), IntValue, IntImm)
INST(LsrValue, Value, IntegerArg0, (Effect::None), IntValue, IntValue)
// ArithmeticShiftRight
INST(AsrImm, Value, IntegerArg0, (Effect::None), IntValue, IntImm)
INST(AsrValue, Value, IntegerArg0, (Effect::None), IntValue, IntValue)
// RotateRight
INST(RorImm, Value, IntegerArg0, (Effect::None), IntValue, IntImm)
INST(RorValue, Value, IntegerArg0, (Effect::None), IntValue, IntValue)
// Reverse the raw scalar container's low Imm bits (16/32/64), independently
// of its consumer view. Zero-extend that swapped width, then mask to the
// natural result type. CLZ/CTZ below instead use their declared 32/64 width;
// zero input returns that width.
INST(ByteSwap, Value, IntegerArg0, (Effect::None), IntValue, Imm)
INST(CountLeadingZeros64, U64, IntegerFixed, (Effect::None), Int64Value)
INST(CountTrailingZeros64, U64, IntegerFixed, (Effect::None), Int64Value)
INST(CountLeadingZeros32, U32, IntegerFixed, (Effect::None), Int32Value)
INST(CountTrailingZeros32, U32, IntegerFixed, (Effect::None), Int32Value)
// Bit fields read the raw 64-bit scalar containers, including bits retained
// through a narrow BitCast view. Numeric producers have zero outside their
// natural width; raw aliases preserve their carrier. Validate lsb<64 and
// length<=64-lsb before shifting. Length zero extracts zero or leaves the
// insert/clear destination unchanged. Mask numeric results to their natural
// type, without restricting a field to that result width.
INST(BitExtract, Value, IntegerArg0, (Effect::None), IntValue, IntImm, IntImm)
INST(BitInsert, Value, IntegerArg0, (Effect::None), IntValue, IntValue, IntImm, IntImm)
INST(BitClear, Value, IntegerArg0, (Effect::None), IntValue, IntImm, IntImm)
// Integer predicates return scalar 0/1 and read the input's logical width,
// independently of the result width or the input's signed token. TestBit's
// full integer index is in [0, 64); an index outside the input view reads 0.
// Materialized predicate results contain a complete scalar 0/1, including
// bits outside a narrow result view; this is not a rule for raw aliases.
INST(TestBit, BOOL, IntegerFixed, (Effect::None), IntValue, IntImm)
INST(TestZero, BOOL, IntegerFixed, (Effect::None), IntValue)
INST(TestNotZero, BOOL, IntegerFixed, (Effect::None), IntValue)

// Vec4 Add Sub
INST(Vec4Add, Value, Arg0, (Effect::Unknown), Value, Value)
INST(Vec4Sub, Value, Arg0, (Effect::Unknown), Value, Value)
INST(Vec4Mul, Value, Arg0, (Effect::Unknown), Value, Value)

INST(Vec4And, Value, Arg0, (Effect::None), Value, Value)
INST(Vec4Or, Value, Arg0, (Effect::None), Value, Value)

// Lane-agnostic vector bitwise operations.
INST(VecXor, Value, Arg0, (Effect::None), Value, Value)
INST(VecAnd, Value, Arg0, (Effect::None), Value, Value)
INST(VecOr, Value, Arg0, (Effect::None), Value, Value)
INST(VecAndNot, Value, Arg0, (Effect::None), Value, Value)

// Integer vector operations. The immediate is the lane width in bits.
INST(VecAdd, Value, Arg0, (Effect::None), Value, Value, Imm)
INST(VecSub, Value, Arg0, (Effect::None), Value, Value, Imm)
INST(VecCmpEq, Value, Arg0, (Effect::None), Value, Value, Imm)
INST(VecCmpGt, Value, Arg0, (Effect::None), Value, Value, Imm)
INST(VecAvg, Value, Arg0, (Effect::None), Value, Value, Imm)
INST(VecMin, Value, Arg0, (Effect::None), Value, Value, Imm, Imm)
INST(VecMax, Value, Arg0, (Effect::None), Value, Value, Imm, Imm)
INST(VecMul, Value, Arg0, (Effect::None), Value, Value, Imm)
// High 16 bits of eight signed/unsigned 16x16-bit products.
INST(VecMulHigh16, Value, Arg0, (Effect::None), Value, Value, Imm)
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
INST(VecMulWiden, Value, Explicit, (Effect::None), Value, Value, Imm, Imm)
INST(VecSatAdd, Value, Arg0, (Effect::None), Value, Value, Imm, Imm)
INST(VecSatSub, Value, Arg0, (Effect::None), Value, Value, Imm, Imm)
// Narrow and concatenate signed lanes; unsigned destination selects PACKUSWB.
INST(VecPack, Value, Arg0, (Effect::None), Value, Value, Imm, Imm)

// Packed integer horizontal operations.
INST(VecAbsDiffSum8, Value, Arg0, (Effect::None), Value, Value)
INST(VecMadd16, Value, Arg0, (Effect::None), Value, Value)

// Packed shifts. The scalar count is the raw x86 imm8 / low qword count.
// Scalar counts apply to every lane; a V128 count supplies one count per lane.
INST(VecShiftLeft, Value, Arg0, (Effect::None), Value, Value, Imm)
INST(VecShiftRight, Value, Arg0, (Effect::None), Value, Value, Imm)
INST(VecShiftRightArithmetic, Value, Arg0, (Effect::None), Value, Value, Imm)
// Immediate forms keep a compile-time x86 count out of the scalar register
// allocator and map directly to NEON SHL/USHR/SSHR.  The first Imm is the raw
// count and the second is the lane width.
INST(VecShiftLeftImm, Value, Arg0, (Effect::None), Value, Imm, Imm)
INST(VecShiftRightImm, Value, Arg0, (Effect::None), Value, Imm, Imm)
INST(VecShiftRightArithmeticImm, Value, Arg0, (Effect::None), Value, Imm, Imm)
// Whole-128-bit byte shift.  `zero` is an explicit SSA input so one zero
// vector can be shared safely within a straight-line block region.  The Imms
// are byte count (1..15) and direction (nonzero = left).
INST(VecByteShift, Value, Arg0, (Effect::None), Value, Value, Imm, Imm)

// Shuffle/duplication operations.
INST(VecShuffle32, Value, Arg0, (Effect::None), Value, Imm)
// Legacy SHUFPS: result lanes 0..1 select from `left`, lanes 2..3 from
// `right`; each 2-bit imm8 field selects one dword of its respective source.
INST(VecShuffle32TwoSrc, Value, Arg0, (Effect::None), Value, Value, Imm)
// Block-cached 128-bit constants and an indexed PSHUFD form. Keeping the
// indexes as SSA lets the constant-folding pass reuse one materialization for
// repeated controls without pinning an emitter scratch register.
INST(VecLoadConst, Value, Explicit, (Effect::Unknown), Imm, Imm)
INST(VecShuffle32Indexed, Value, Arg0, (Effect::None), Value, Value)
// One-instruction all-zero vector, block-local CSE-able like VecLoadConst.
INST(VecSharedZero, Value, Explicit, (Effect::None))
INST(VecShuffle16, Value, Arg0, (Effect::None), Value, Imm, Imm)
INST(VecExtractBytes, Value, Arg0, (Effect::None), Value, Value, Imm)
INST(VecZip, Value, Arg0, (Effect::None), Value, Value, Imm, Imm)
INST(VecDupPairs32, Value, Arg0, (Effect::None), Value, Imm)
INST(VecDup64, Value, Explicit, (Effect::None), Value)
INST(VecExtract64, Value, Explicit, (Effect::None), Value, Imm)
INST(VecExtract16, Value, Explicit, (Effect::None), Value, Imm)
INST(VecInsert16, Value, Arg0, (Effect::None), Value, Value, Imm)
// V128 lane sign bits, lane0 -> bit0. Imm is lane width (8/16/32/64),
// result is a zero-extended U32/U64 mask, independent of the input's origin.
INST(VecMovMask, Value, Explicit, (Effect::None), Value, Imm)
INST(VecTableLookup8, Value, Arg0, (Effect::None), Value, Value)

// 128-bit crypto primitives.  These are deliberately whole-instruction IR
// operations: the ARM64 backend keeps them in SIMD registers and emits the
// architectural Crypto Extension instructions, rather than routing a hot
// guest instruction through a host helper.
INST(VecAesEnc, Value, Arg0, (Effect::None), Value, Value)
INST(VecAesEncLast, Value, Arg0, (Effect::None), Value, Value)
INST(VecAesDec, Value, Arg0, (Effect::None), Value, Value)
INST(VecAesDecLast, Value, Arg0, (Effect::None), Value, Value)
// FEX-style round forms take a shared zero-key vector explicitly.  They also
// permit the allocator to tie the result to the dying state input.
INST(VecAesEncFast, Value, Arg0, (Effect::None), Value, Value, Value)
INST(VecAesEncLastFast, Value, Arg0, (Effect::None), Value, Value, Value)
INST(VecAesDecFast, Value, Arg0, (Effect::None), Value, Value, Value)
INST(VecAesDecLastFast, Value, Arg0, (Effect::None), Value, Value, Value)
INST(VecAesKeygenAssist, Value, Arg0, (Effect::None), Value, Imm)
INST(VecPclMul, Value, Arg0, (Effect::None), Value, Value, Imm)
INST(VecSha256Msg1, Value, Arg0, (Effect::None), Value, Value)
INST(VecSha256Msg2, Value, Arg0, (Effect::None), Value, Value)
INST(VecSha256Rnds2, Value, Arg0, (Effect::None), Value, Value, Value)

// Scalar f32 arithmetic preserving bits [127:32] of the left operand.
// Ordinary FP arithmetic and SQRT permit native quiet-NaN sign and payload
// selection; ARM64 follows FEX here. Finite/zero/infinity results retain
// their opcode semantics. This policy does not relax MIN/MAX selection,
// comparison masks, conversion sentinels or preservation of unwritten lanes.
// Numerical FP observes controls and accumulates host exception state. Even
// an unused result can raise an architectural exception and must survive DCE.
INST(VecFAddScalar32, Value, Arg0, (Effect::ReadHostState | Effect::WriteHostState), Value, Value)
INST(VecFSubScalar32, Value, Arg0, (Effect::ReadHostState | Effect::WriteHostState), Value, Value)
INST(VecFMulScalar32, Value, Arg0, (Effect::ReadHostState | Effect::WriteHostState), Value, Value)
INST(VecFDivScalar32, Value, Arg0, (Effect::ReadHostState | Effect::WriteHostState), Value, Value)
INST(VecFAddScalar64, Value, Arg0, (Effect::ReadHostState | Effect::WriteHostState), Value, Value)
INST(VecFSubScalar64, Value, Arg0, (Effect::ReadHostState | Effect::WriteHostState), Value, Value)
INST(VecFMulScalar64, Value, Arg0, (Effect::ReadHostState | Effect::WriteHostState), Value, Value)
INST(VecFDivScalar64, Value, Arg0, (Effect::ReadHostState | Effect::WriteHostState), Value, Value)

// Packed IEEE floating-point operations. Imm is the lane width (32 or 64).
INST(VecFAdd, Value, Arg0, (Effect::ReadHostState | Effect::WriteHostState), Value, Value, Imm)
INST(VecFSub, Value, Arg0, (Effect::ReadHostState | Effect::WriteHostState), Value, Value, Imm)
INST(VecFMul, Value, Arg0, (Effect::ReadHostState | Effect::WriteHostState), Value, Value, Imm)
INST(VecFDiv, Value, Arg0, (Effect::ReadHostState | Effect::WriteHostState), Value, Value, Imm)
INST(VecFMinMax,
     Value,
     Arg0,
     (Effect::ReadHostState | Effect::WriteHostState),
     Value,
     Value,
     Imm,
     Imm,
     Imm)
// Operands are (merge,input,lane_bits,kind,scalar).
// kind: 0=sqrt, 1=reciprocal estimate, 2=reciprocal-sqrt estimate.
INST(VecFUnary,
     Value,
     Arg0,
     (Effect::ReadHostState | Effect::WriteHostState),
     Value,
     Value,
     Imm,
     Imm,
     Imm)

// Scalar compare: low FP lanes of the two vectors, with lane width in bits.
// U32 result: bit0 CF / bit1 PF / bit2 ZF. Greater=0, less=1, equal=4,
// unordered=7. The width immediate bit7 requests signaling on quiet NaNs.
INST(VecFCmp,
     U32,
     IntegerFixed,
     (Effect::ReadHostState | Effect::WriteHostState),
     Value,
     Value,
     Imm)
// Lane-wise compare to an all-ones / all-zeros mask.  The predicate (second
// Imm) is a RELATION SET, not an ISA's compare encoding: an IEEE 754 compare
// of two lanes has exactly four mutually exclusive outcomes, and the predicate
// names the subset for which the lane's mask is set --
//     bit 0 = a < b, bit 1 = a == b, bit 2 = a > b, bit 3 = unordered.
// All 16 subsets are legal: 0 is "never" and 15 is "always".  So `le` is 3,
// `ord` is 7, `neq` (x86's NEQ_UQ, which is true when unordered) is 13, and
// x86's NEQ_OQ, which is not, is 5.  Front ends translate their own encoding
// into this. Predicate bit4 requests signaling on quiet NaNs; low four bits
// retain the relation set. Signaling NaNs always raise invalid.
// Final Imm is scalar (preserve upper lanes) when non-zero.
INST(VecFCmpMask,
     Value,
     Arg0,
     (Effect::ReadHostState | Effect::WriteHostState),
     Value,
     Value,
     Imm,
     Imm,
     Imm)

// Scalar conversions. The result is the raw IEEE/integer bit pattern in a
// scalar container; widths are supplied as immediates in bits. IntToFloat
// consumes signed source-width bits, then a float width. FloatToInt takes
// float width, integer width, and a truncate-to-zero selector; invalid signed
// conversions return that integer width's minInt and accumulate invalid.
INST(VecFCvtIntToFloat,
     Value,
     Explicit,
     (Effect::ReadHostState | Effect::WriteHostState),
     Value,
     Imm,
     Imm)
INST(VecFCvtFloatToInt,
     Value,
     Explicit,
     (Effect::ReadHostState | Effect::WriteHostState),
     Value,
     Imm,
     Imm,
     Imm)
// Imm is source IEEE width (32 or 64); the result is the other width.
INST(VecFCvtScalar, Value, Explicit, (Effect::ReadHostState | Effect::WriteHostState), Value, Imm)
// 0=dq2ps, 1=dq2pd, 2=ps2dq, 3=ttps2dq, 4=pd2dq, 5=ttpd2dq,
// 6=ps2pd, 7=pd2ps.
INST(VecFCvtPacked, Value, Explicit, (Effect::ReadHostState | Effect::WriteHostState), Value, Imm)

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
INST(VecUnzip, Value, Arg0, (Effect::None), Value, Value, Imm, Imm)

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
// Bit2 selects scalar lane 0 and preserves upper lanes from arg0. The
// frontend performs any additional encoding-specific destination merge.
// Bit3 selects alternating signs; bit4 selects odd lanes for subtraction.
// Effective addend negation is bit1 XOR (bit3 && (lane%2 == bit4)).
INST(VecFMulAdd, Value, Arg0, (Effect::Unknown), Value, Value, Value, Imm, Imm)

// Round each floating-point lane to an INTEGRAL VALUE, result still floating
// point.  First Imm is the lane width in bits (32 or 64); second Imm selects
// the rounding mode:
//     0  to nearest, ties to even   1  toward -infinity (floor)
//     2  toward +infinity (ceil)    3  toward zero (truncate)
// Third Imm non-zero means SCALAR: only lane 0 is rounded and the remaining
// lanes come from `merge` (arg 1), the same shape VecFUnary already uses.
// Rounding immediate low two bits select RC; bit3 suppresses precision status.
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
INST(VecFRoundInt, Value, Arg0, (Effect::Unknown), Value, Value, Imm, Imm, Imm)

// Upper 64 bits of a raw-container 64x64 product (consumer views do not
// truncate the inputs). Imm = 1 for signed, 0 for unsigned. Signed uses bit63;
// the numeric high-half result is then masked to its natural result type.
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
INST(MulHigh, Value, IntegerExplicit, (Effect::None), IntValue, IntValue, IntImm)

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
INST(CondSet, BOOL, Fixed, (Effect::ReadFlags), Cond)
// Same architectural result as CondSet, but emitted only when the frontend has
// proved that the immediately preceding guest flag producer is in this unit
// and no intervening guest instruction can have clobbered host NZCV.  A sole
// branch/select consumer can therefore fold this marker to B.cond/CSEL.
INST(LocalCondSet, BOOL, Fixed, (Effect::ReadFlags), Cond)
// Parity condition over the low byte of a named local producer. Imm is 0 for
// x86 PF (even parity) and 1 for its inverse. Unlike TestFlags this does not
// read or write the architectural flags word.
INST(LocalParitySet, BOOL, Fixed, (Effect::None), Value, Imm)
// Decoder-side conservative proof that both direct terminal-Jcc successors
// kill incoming arithmetic flags before any read, fault, helper or control
// edge. FlagsEliminationPass consumes and removes this marker; backends keep
// no-op implementations as a defensive fallback.
INST(BranchOnlyEdges, Void, Void, (Effect::Ordered))
// Condition over the host FP NZCV produced by the named VecFCmp.  The Value
// edge pins the producer and gives the interpreter the compact comparison
// result; ARM64 can fold a sole branch/select consumer directly into the
// already-emitted FCMP.
INST(FCmpCondSet, BOOL, Fixed, (Effect::Unknown), Value, Cond)

// Unit-local canonical form for `0 - value`. FoldValues creates it only
// under SVM_INT_IMM_FOLD after proving the zero LoadImm has this sole use.
// Keeping it at the end preserves every pre-existing opcode number.
INST(Neg, Value, IntegerArg0, (Effect::None), IntValue)

// Count the set bits in the complete raw 64-bit scalar container, regardless
// of its consumer view. Narrow numeric inputs are already zero-normalized;
// raw aliases retain all carrier bits. Mask to the natural result type.
INST(PopCount, U64, IntegerFixed, (Effect::None), IntValue)
// CRC-32C without pre/post complement. Only the accumulator's low 32 bits
// and the data's low Imm bits (8, 16, 32 or 64) participate; result is U32.
INST(Crc32c, U32, IntegerFixed, (Effect::None), IntValue, IntValue, Imm)

// Restore selected flags from an opaque carrier returned by GetFlags on this
// backend. Value transport/Select may choose a carrier; frontends never depend
// on its bit layout. This expresses dynamic flag preservation as dataflow.
INST(RestoreFlags, Void, Void, (Effect::WriteFlags), Value, Flags)

// Recoverable guest PC for following faulting operations in this block. The
// runtime must resolve the native fault PC; the successful path performs no
// guest-state write. This is actual exception metadata, not a validation pass.
INST(FaultLocation, Void, Void, (Effect::Location), Imm)
// A complete external entry at this Function block's first node. The key is
// a U64 guest PC, not a physical label. Each declared entry is an SSA root:
// definitions must dominate uses from every actual external entry. Internal
// edges keep their ordinary body entry; the backend owns its external prologue.
INST(EntryPoint, Void, Void, (Effect::Location), Imm)

// Write one complete U64 uniform field. Any nonzero bit in the condition's
// logical integer view selects the first U64 literal; zero selects the second.
// This is a guest-state store at any position, independent of terminal use.
INST(StoreUniformSelectImm, Void, Void, (Effect::WriteGuestState), Uniform, IntValue, Imm, Imm)

// Full V128 lane extension. Imms are (src_lane_bits, dst_lane_bits, is_signed):
// source lanes are 8/16/32 bits, destination lanes 16/32/64, source < destination,
// and is_signed is 0 or 1. For i in [0,128/dst_lane_bits), output lane i is the
// zero/sign extension of input lane i; all higher source lanes are ignored.
// Lane zero occupies the low bits. The complete output is defined; flags stay unchanged.
INST(VecExtend, Value, Arg0, (Effect::None), Value, Imm, Imm, Imm)

// Order preceding memory writes before subsequent memory writes. No value or
// flags are read or written; this does not request load ordering.
INST(MemoryStoreFence, Void, Void, (Effect::Ordered))

// Full V128 signed widening product of the low contiguous source lanes.
// Imm is src_lane_bits (8, 16 or 32). For i in [0,128/(2*src_lane_bits)),
// output lane i is signed(a.lane[i]) * signed(b.lane[i]), retaining all
// 2*src_lane_bits product bits. Both inputs are complete V128 carriers;
// higher source lanes are ignored and no flags are changed.
INST(VecSignedMulWidenLow, Value, Explicit, (Effect::None), Value, Value, Imm)

// A bound architectural system query. The kind is an immutable protocol
// selector; the callback has Cpuid's pair-return C ABI. This node retains
// only its first scalar result and has no hidden mutable state.
INST(ReadSystem, U64, Fixed, (Effect::Unknown), Imm, Imm)

// Explicit load ordering and scheduler hints.
INST(MemoryLoadFence, Void, Void, (Effect::Ordered))
INST(CpuYield, Void, Void, (Effect::Ordered))
INST(VecAesImc, Value, Arg0, (Effect::None), Value)
INST(ParallelBitDeposit, Value, Arg0, (Effect::None), Value, Value)
INST(ParallelBitExtract, Value, Arg0, (Effect::None), Value, Value)

// Explicit string lengths retain signed 32/64-bit raw values in a V128 pair.
// Immediate bit8 selects 64-bit lengths; low8 is the string operation control.
INST(Sse42StrExplicit, U64, Fixed, (Effect::None), Value, Value, Value, Imm)

// Stateful x87 operation with explicit failure metadata (see x87.h).
// DP is read on the raised-exception edge before the result home is written.
INST(X87Execute,
     U64,
     Fixed,
     (Effect::ReadGuestState | Effect::WriteGuestState | Effect::External | Effect::Control),
     Value,
     Imm,
     Value,
     Value,
     Imm)
INST(ReadSystemPair, Value, Explicit, (Effect::Unknown), Imm, Imm)
// One architectural metadata commit, including legacy fused x87 windows.
INST(X87CommitMetadata, Void, Void, (Effect::WriteGuestState), Value, Imm, Value, Imm)

// A complete restartable guest boundary. The immediate is the actual resume PC.
// Pending delivery observes every architectural field and flag, even when the
// fast native path merely checks the execution owner's ready request.
INST(GuestSafepoint,
     Void,
     Void,
     (Effect::ReadGuestState | Effect::ReadFlags | Effect::Control),
     Imm)
