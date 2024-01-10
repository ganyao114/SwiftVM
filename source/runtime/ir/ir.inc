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
INST(SignExtend, Value, Value, Imm)
// And & Or
INST(AndImm, Value, Value, Imm)
INST(OrImm, Value, Value, Imm)
INST(AndValue, Value, Value, Value)
INST(OrValue, Value, Value, Value)
INST(XorValue, Value, Value, Value)
// Not
INST(Not, Value, Value, Value)
// Get flag from result
INST(GetCarry, BOOL, Value)
INST(GetOverFlow, BOOL, Value)
INST(GetZero, BOOL, Value)
INST(GetNegate, BOOL, Value)
INST(GetPositive, BOOL, Value)
INST(GetSigned, BOOL, Value)
INST(GetNegZero, BOOL, Value)
INST(GetParity, BOOL, Value)
INST(GetNZCV, Value, Value)
INST(GetAllFlags, Value, Value)
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