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
INST(LoadMemory, Value, Operand)
INST(StoreMemory, Void, Operand, Value)
INST(LoadMemoryTSO, Value, Operand)
INST(StoreMemoryTSO, Void, Operand, Value)
INST(MemoryCopy, Void, Lambda, Lambda, Imm)
INST(MemoryCopyTSO, Void, Lambda, Lambda, Imm)
INST(CompareAndSwap, Value, Value, Value, Value)

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

// Shuffle/duplication operations.
INST(VecShuffle32, Value, Value, Imm)
INST(VecShuffle16, Value, Value, Imm, Imm)
INST(VecZip, Value, Value, Value, Imm, Imm)
INST(VecDupPairs32, Value, Value, Imm)
INST(VecDup64, Value, Value)

// Scalar f32 arithmetic preserving bits [127:32] of the left operand.
INST(VecFAddScalar32, Value, Value, Value)
INST(VecFSubScalar32, Value, Value, Value)
INST(VecFMulScalar32, Value, Value, Value)
INST(VecFDivScalar32, Value, Value, Value)
