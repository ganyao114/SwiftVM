#include "emit.h"

namespace swift::runtime::backend::riscv64::detail {
void Writer::SelectValue(OpContext& ctx) {
    if (ctx.operation.opcode == OpCode::CondSelect)
        Condition(a0, static_cast<ir::Condition>(ctx.Arg(0).Payload()));
    else
        Read(ctx.Arg(0), a0);
    Label other, done;
    if (ctx.operation.opcode == OpCode::SelectZero)
        as.BNE(a0, x0, &other);
    else
        as.BEQ(a0, x0, &other);
    ReadPair(ctx.Arg(1), a1, a2);
    as.J(&done);
    as.Bind(&other);
    ReadPair(ctx.Arg(2), a1, a2);
    as.Bind(&done);
    as.MV(ctx.destination, a1);
    if (ctx.operation.type == Type::V128) as.MV(ctx.high, a2);
}

void Writer::Handle(Op<OpCode::Select>, OpContext& ctx) { SelectValue(ctx); }

void Writer::Handle(Op<OpCode::SelectZero>, OpContext& ctx) { SelectValue(ctx); }

void Writer::Handle(Op<OpCode::CondSelect>, OpContext& ctx) { SelectValue(ctx); }

void Writer::Handle(Op<OpCode::AdvancePC>, OpContext& ctx) {
    if (options.pc_offset == UINT32_MAX) Reject(CompileError::UnsupportedOperand);
    Load(a0, s1, options.pc_offset, 64);
    as.LI(a1, ctx.Arg(0).Payload());
    as.ADD(a0, a0, a1);
    Store(a0, s1, options.pc_offset, 64);
    static_pc_known = false;
}

void Writer::Handle(Op<OpCode::GetLocation>, OpContext& ctx) {
    if (options.pc_offset == UINT32_MAX) Reject(CompileError::UnsupportedOperand);
    Load(ctx.destination, s1, options.pc_offset, 64);
}

void Writer::Handle(Op<OpCode::SetLocation>, OpContext& ctx) {
    if (options.pc_offset == UINT32_MAX) Reject(CompileError::UnsupportedOperand);
    as.LI(a0, ctx.Arg(0).Payload());
    Store(a0, s1, options.pc_offset, 64);
    static_pc = ctx.Arg(0).Payload();
    static_pc_known = true;
}

void Writer::Handle(Op<OpCode::EntryPoint>, OpContext&) {}

void Writer::Handle(Op<OpCode::FaultLocation>, OpContext&) {}

void Writer::Handle(Op<OpCode::Nop>, OpContext&) {}

void Writer::Handle(Op<OpCode::UniformBarrier>, OpContext&) {}

void Writer::Handle(Op<OpCode::XchgBarrier>, OpContext&) {}

void Writer::Handle(Op<OpCode::BranchOnlyEdges>, OpContext&) {}

// These legacy-shaped IR forms are not emitted by the current x86 frontend
// and are also outside the rebuilt ARM64 writer's accepted input contract.
#define RV_REJECT(name)                                                                            \
    void Writer::Handle(Op<OpCode::name>, OpContext&) { Reject(CompileError::UnsupportedOpcode); }
RV_REJECT(DefineLocal)
RV_REJECT(LoadLocal)
RV_REJECT(StoreLocal)
RV_REJECT(AddPhi)
RV_REJECT(LoadMemoryExtend)
RV_REJECT(LoadMemoryExtendTSO)
RV_REJECT(MemoryCopy)
RV_REJECT(MemoryCopyTSO)
RV_REJECT(GetHostGPR)
RV_REJECT(GetHostFPR)
RV_REJECT(SetHostGPR)
RV_REJECT(SetHostFPR)
RV_REJECT(PushRSB)
RV_REJECT(CallReturn)
RV_REJECT(PopRSB)
#undef RV_REJECT
}  // namespace swift::runtime::backend::riscv64::detail
