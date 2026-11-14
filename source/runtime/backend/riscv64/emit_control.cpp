#include "abi_layout.inc"
#include "emit.h"
#include "runtime/exit.h"

namespace swift::runtime::backend::riscv64::detail {
namespace {
void EmitCondMove(Assembler& as,
                  GPR destination,
                  GPR true_val,
                  GPR false_val,
                  GPR cond,
                  bool select_zero) {
    if (select_zero) {
        if (false_val == x0) {
            as.CZERO_NEZ(destination, true_val, cond);
        } else if (true_val == x0) {
            as.CZERO_EQZ(destination, false_val, cond);
        } else {
            as.CZERO_NEZ(t0, true_val, cond);
            as.CZERO_EQZ(t1, false_val, cond);
            as.OR(destination, t0, t1);
        }
    } else {
        if (false_val == x0) {
            as.CZERO_EQZ(destination, true_val, cond);
        } else if (true_val == x0) {
            as.CZERO_NEZ(destination, false_val, cond);
        } else {
            as.CZERO_EQZ(t0, true_val, cond);
            as.CZERO_NEZ(t1, false_val, cond);
            as.OR(destination, t0, t1);
        }
    }
}
}  // namespace

void Writer::Handle(Op<OpCode::GuestSafepoint>, OpContext& ctx) {
    if (!Linked() || options.pc_offset == UINT32_MAX) Reject(CompileError::UnsupportedOperand);
    Label ready;
    Load(a0, s1, SVM_RV_CTX_PENDING_EVENTS - SVM_RV_CTX_STATE, 64);
    as.BEQ(a0, x0, &ready);
    const auto previous_pc = guest_pc;
    guest_pc = ctx.Arg(0).Payload();
    as.LI(t0, guest_pc);
    Store(t0, s1, options.pc_offset, 64);
    Exit(runtime::ExitValue(runtime::NativeExit::GuestEvents));
    guest_pc = previous_pc;
    as.Bind(&ready);
}

void Writer::Handle(Op<OpCode::GuestDispatchSafepoint>, OpContext& ctx) {
    Handle(Op<OpCode::GuestSafepoint>{}, ctx);
}

void Writer::SelectValue(OpContext& ctx) {
    const bool select_zero = ctx.operation.opcode == OpCode::SelectZero;
    GPR cond = a0;
    if (ctx.operation.opcode == OpCode::CondSelect)
        Condition(a0, static_cast<ir::Condition>(ctx.Arg(0).Payload()));
    else
        cond = ReadRef(ctx.Arg(0), a0);

    if (options.zicond) {
        if (ctx.operation.type == Type::V128) {
            ReadPair(ctx.Arg(1), a1, a2);
            ReadPair(ctx.Arg(2), a3, t2);
            EmitCondMove(as, ctx.destination, a1, a3, cond, select_zero);
            EmitCondMove(as, ctx.high, a2, t2, cond, select_zero);
            return;
        }
        const GPR true_val = ReadRef(ctx.Arg(1), a1, true);
        const GPR false_val = ReadRef(ctx.Arg(2), a2, true);
        EmitCondMove(as, ctx.destination, true_val, false_val, cond, select_zero);
        Mask(ctx.destination, ctx.width);
        return;
    }

    Label other, done;
    if (select_zero)
        as.BNE(cond, x0, &other);
    else
        as.BEQ(cond, x0, &other);
    if (ctx.operation.type == Type::V128) {
        ReadPair(ctx.Arg(1), ctx.destination, ctx.high);
        as.J(&done);
        as.Bind(&other);
        ReadPair(ctx.Arg(2), ctx.destination, ctx.high);
        as.Bind(&done);
        return;
    }
    Read(ctx.Arg(1), ctx.destination, true);
    as.J(&done);
    as.Bind(&other);
    Read(ctx.Arg(2), ctx.destination, true);
    as.Bind(&done);
    Mask(ctx.destination, ctx.width);
}

void Writer::Handle(Op<OpCode::Select>, OpContext& ctx) { SelectValue(ctx); }

void Writer::Handle(Op<OpCode::SelectZero>, OpContext& ctx) { SelectValue(ctx); }

void Writer::Handle(Op<OpCode::CondSelect>, OpContext& ctx) { SelectValue(ctx); }

void Writer::Handle(Op<OpCode::AdvancePC>, OpContext& ctx) {
    if (options.pc_offset == UINT32_MAX) Reject(CompileError::UnsupportedOperand);
    Load(a0, s1, options.pc_offset, 64);
    Address(a0, a0, static_cast<std::int64_t>(ctx.Arg(0).Payload()));
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
