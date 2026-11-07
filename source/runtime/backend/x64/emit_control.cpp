#include "emit.h"

namespace swift::runtime::backend::x64::detail {
void Writer::SelectValue(OpContext& c) {
    if (c.operation.opcode == OpCode::CondSelect)
        Condition(rax, static_cast<ir::Condition>(c.Arg(0).Payload()));
    else
        Read(c.Arg(0), rax);
    test(rax, rax);
    Label other, done;
    if (c.operation.opcode == OpCode::SelectZero)
        jnz(other, T_NEAR);
    else
        jz(other, T_NEAR);
    if (c.vector_result)
        ReadVector(c.Arg(1), xmm0);
    else
        Read(c.Arg(1), rax, true);
    jmp(done, T_NEAR);
    L(other);
    if (c.vector_result)
        ReadVector(c.Arg(2), xmm0);
    else
        Read(c.Arg(2), rax, true);
    L(done);
}

#define SEL(name)                                                                                  \
    void Writer::Handle(Op<OpCode::name>, OpContext& c) { SelectValue(c); }
// clang-format off
SEL(Select)
SEL(SelectZero)
SEL(CondSelect)
// clang-format on
#undef SEL

void Writer::Handle(Op<OpCode::Goto>, OpContext& c) {
    Read(c.Arg(0), rax);
    WriteResult(c);
    c.saved = true;
    test(rax, rax);
    jnz(locals[c.node.value], T_NEAR);
}

void Writer::Handle(Op<OpCode::NotGoto>, OpContext& c) {
    Read(c.Arg(0), rax);
    WriteResult(c);
    c.saved = true;
    test(rax, rax);
    jz(locals[c.node.value], T_NEAR);
}

void Writer::Handle(Op<OpCode::BindLabel>, OpContext& c) {
    L(locals[c.Arg(0).Payload()]);
    static_pc_known = false;
}

void Writer::Handle(Op<OpCode::AdvancePC>, OpContext& c) {
    if (options.pc_offset == UINT32_MAX) Reject(CompileError::UnsupportedOperand);
    mov(rax, c.Arg(0).Payload());
    add(qword[r12 + options.pc_offset], rax);
    static_pc_known = false;
}

void Writer::Handle(Op<OpCode::GetLocation>, OpContext&) {
    if (options.pc_offset == UINT32_MAX) Reject(CompileError::UnsupportedOperand);
    mov(rax, qword[r12 + options.pc_offset]);
}

void Writer::Handle(Op<OpCode::SetLocation>, OpContext& c) {
    if (options.pc_offset == UINT32_MAX) Reject(CompileError::UnsupportedOperand);
    mov(rax, c.Arg(0).Payload());
    mov(qword[r12 + options.pc_offset], rax);
    static_pc = c.Arg(0).Payload();
    static_pc_known = true;
}

#define EMPTY(name)                                                                                \
    void Writer::Handle(Op<OpCode::name>, OpContext&) {}
// clang-format off
EMPTY(Nop)
EMPTY(FaultLocation)
EMPTY(EntryPoint)
EMPTY(UniformBarrier)
EMPTY(XchgBarrier)
EMPTY(BranchOnlyEdges)
// clang-format on
#undef EMPTY
#define NO(name)                                                                                   \
    void Writer::Handle(Op<OpCode::name>, OpContext&) { Reject(CompileError::UnsupportedOpcode); }
// clang-format off
NO(DefineLocal)
NO(LoadLocal)
NO(StoreLocal)
NO(AddPhi)
NO(LoadMemoryExtend)
NO(LoadMemoryExtendTSO)
NO(MemoryCopy)
NO(MemoryCopyTSO)
NO(GetHostGPR)
NO(GetHostFPR)
NO(SetHostGPR)
NO(SetHostFPR)
NO(PushRSB)
NO(CallReturn)
NO(PopRSB)
NO(CallLocation)
NO(CallDynamic)
// clang-format on
#undef NO
}  // namespace swift::runtime::backend::x64::detail
