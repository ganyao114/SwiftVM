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
