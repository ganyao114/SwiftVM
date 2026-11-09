#include "abi_layout.inc"
#include "emit.h"
#include "runtime/exit.h"

namespace swift::runtime::backend::loongarch64::detail {
void Writer::Handle(Op<OpCode::GuestSafepoint>, OpContext& ctx) {
    if (!options.dispatcher || !options.exit_handler || options.pc_offset == UINT32_MAX)
        Reject(CompileError::UnsupportedOperand);
    Label ready;
    Load(a0, state, SVM_LA_CTX_PENDING_EVENTS - SVM_LA_CTX_STATE, 64);
    as.BEQ(a0, x0, &ready);
    const auto previous_pc = guest_pc;
    guest_pc = ctx.Arg(0).Payload();
    as.LI(t0, guest_pc);
    Store(t0, state, options.pc_offset, 64);
    Exit(runtime::ExitValue(runtime::NativeExit::GuestEvents));
    guest_pc = previous_pc;
    as.Bind(&ready);
}

void Writer::Handle(Op<OpCode::GuestDispatchSafepoint>, OpContext& ctx) {
    Handle(Op<OpCode::GuestSafepoint>{}, ctx);
}

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
    Load(a0, state, options.pc_offset, 64);
    as.LI(a1, ctx.Arg(0).Payload());
    as.ADD(a0, a0, a1);
    Store(a0, state, options.pc_offset, 64);
    static_pc_known = false;
}

void Writer::Handle(Op<OpCode::GetLocation>, OpContext& ctx) {
    if (options.pc_offset == UINT32_MAX) Reject(CompileError::UnsupportedOperand);
    Load(ctx.destination, state, options.pc_offset, 64);
}

void Writer::Handle(Op<OpCode::SetLocation>, OpContext& ctx) {
    if (options.pc_offset == UINT32_MAX) Reject(CompileError::UnsupportedOperand);
    as.LI(a0, ctx.Arg(0).Payload());
    Store(a0, state, options.pc_offset, 64);
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
#define LA_REJECT(name)                                                                            \
    void Writer::Handle(Op<OpCode::name>, OpContext&) { Reject(CompileError::UnsupportedOpcode); }
LA_REJECT(DefineLocal)
LA_REJECT(LoadLocal)
LA_REJECT(StoreLocal)
LA_REJECT(AddPhi)
LA_REJECT(LoadMemoryExtend)
LA_REJECT(LoadMemoryExtendTSO)
LA_REJECT(MemoryCopy)
LA_REJECT(MemoryCopyTSO)
LA_REJECT(GetHostGPR)
LA_REJECT(GetHostFPR)
LA_REJECT(SetHostGPR)
LA_REJECT(SetHostFPR)
LA_REJECT(PushRSB)
LA_REJECT(CallReturn)
LA_REJECT(PopRSB)
#undef LA_REJECT
}  // namespace swift::runtime::backend::loongarch64::detail
