#include "emit.h"
#include "runtime/exit.h"

#include <algorithm>

namespace swift::runtime::backend::riscv64::detail {
void Writer::Uniform(OpContext& ctx) {
    const bool store = ctx.operation.opcode == OpCode::StoreUniform;
    const auto offset = ctx.Arg(0).Payload();
    const auto bits = std::max(8u, TypeBits(store ? ctx.Arg(0).TypeView() : ctx.operation.type));
    if (offset > INT32_MAX - 16) Reject(CompileError::UnsupportedOperand);
    if (store) {
        ReadPair(ctx.Arg(1), a0, a1);
        if (TypeBits(View(ctx.Arg(1))) < 64) Mask(a0, TypeBits(View(ctx.Arg(1))));
        Store(a0, s1, offset, std::min(bits, 64u));
        if (bits == 128) Store(a1, s1, offset + 8, 64);
        if (offset == options.flags_offset && bits == 64) as.MV(s11, a0);
        if (offset == options.pc_offset) {
            static_pc_known = false;
            const auto value = ctx.Arg(1);
            if (bits == 64 && value.Kind() == OperandKind::Value &&
                source.Operations()[value.Payload()].opcode == OpCode::LoadImm) {
                const auto first = source.Operands(NodeId{static_cast<u32>(value.Payload())});
                static_pc = (*first.begin()).Payload();
                const auto width = std::min(64u, TypeBits(View(value)));
                if (width < 64) static_pc &= (u64{1} << width) - 1;
                static_pc_known = true;
            }
        }
    } else {
        if (offset == options.flags_offset && bits == 64)
            as.MV(ctx.destination, s11);
        else
            Load(ctx.destination, s1, offset, std::min(bits, 64u));
        if (bits == 128) Load(ctx.high, s1, offset + 8, 64);
    }
}

void Writer::Handle(Op<OpCode::LoadUniform>, OpContext& ctx) { Uniform(ctx); }

void Writer::Handle(Op<OpCode::StoreUniform>, OpContext& ctx) { Uniform(ctx); }

void Writer::Handle(Op<OpCode::GetUniformAddress>, OpContext& ctx) {
    Address(ctx.destination, s1, ctx.Arg(0).Payload());
}

void Writer::Memory(OpContext& ctx) {
    const bool store = ctx.operation.opcode == OpCode::StoreMemory ||
                       ctx.operation.opcode == OpCode::StoreMemoryTSO;
    const bool ordered = ctx.operation.opcode == OpCode::LoadMemoryTSO ||
                         ctx.operation.opcode == OpCode::StoreMemoryTSO;
    auto at = ctx.operands.begin();
    Expression(at, ctx.operands.end(), a0);
    const auto value = store ? *at : CellView{};
    const unsigned bits = std::max(8u, store ? TypeBits(View(value)) : ctx.width);
    if (bits > 128) Reject(CompileError::UnsupportedType);
    // The unaligned byte fallback still reports the logical access width so
    // the fault contract matches the aligned path and the other backends.
    const unsigned access_bytes = std::min(bits, 64u) / 8;
    if (store) {
        ReadPair(value, a1, a2);
        Mask(a1, std::min(bits, 64u));
    }
    if (ordered) as.FENCE(FenceOrder::RW, FenceOrder::RW);
    Label unaligned, done;
    if (bits >= 16) {
        as.ANDI(t0, a0, std::min(8u, bits / 8) - 1);
        as.BNE(t0, x0, &unaligned);
    }
    if (store) {
        const auto first = static_cast<u32>(output.words.size() * 4);
        Store(a1, a0, 0, std::min(bits, 64u));
        RecordGuestAccess(first, store ? 2 : 1, std::min(bits, 64u) / 8);
        if (bits == 128) {
            const auto first = static_cast<u32>(output.words.size() * 4);
            Store(a2, a0, 8, 64);
            RecordGuestAccess(first, store ? 2 : 1, 8);
        }
    } else {
        const auto first = static_cast<u32>(output.words.size() * 4);
        Load(ctx.destination, a0, 0, std::min(bits, 64u));
        RecordGuestAccess(first, store ? 2 : 1, std::min(bits, 64u) / 8);
        if (bits == 128) {
            const auto first = static_cast<u32>(output.words.size() * 4);
            Load(ctx.high, a0, 8, 64);
            RecordGuestAccess(first, store ? 2 : 1, 8);
        }
    }
    if (bits >= 16) {
        as.J(&done);
        as.Bind(&unaligned);
        if (!store) {
            as.MV(ctx.destination, x0);
            if (bits == 128) as.MV(ctx.high, x0);
        }
        for (unsigned byte = 0; byte < bits / 8; ++byte) {
            if (store) {
                const auto source = byte < 8 ? a1 : a2;
                if (byte % 8) as.SRLI(source, source, 8);
                const auto first = static_cast<u32>(output.words.size() * 4);
                as.SB(source, byte, a0);
                RecordGuestAccess(first, store ? 2 : 1, access_bytes);
            } else {
                const auto first = static_cast<u32>(output.words.size() * 4);
                as.LBU(t0, byte, a0);
                RecordGuestAccess(first, store ? 2 : 1, access_bytes);
                if (byte % 8) as.SLLI(t0, t0, (byte % 8) * 8);
                const auto destination = byte < 8 ? ctx.destination : ctx.high;
                as.OR(destination, destination, t0);
            }
        }
        as.Bind(&done);
    }
    if (ordered) as.FENCE(FenceOrder::RW, FenceOrder::RW);
}

#define RV_MEMORY(name)                                                                            \
    void Writer::Handle(Op<OpCode::name>, OpContext& ctx) { Memory(ctx); }
RV_MEMORY(LoadMemory)
RV_MEMORY(StoreMemory)
RV_MEMORY(LoadMemoryTSO)
RV_MEMORY(StoreMemoryTSO)
#undef RV_MEMORY

void Writer::Handle(Op<OpCode::CheckMemoryAlignment>, OpContext& ctx) {
    Read(ctx.Arg(0), a0);
    as.LI(t0, ctx.Arg(1).Payload());
    as.AND(t0, a0, t0);
    Label aligned;
    as.BEQ(t0, x0, &aligned);
    Exit(ExitValue(NativeExit::AlignmentFault));
    as.Bind(&aligned);
}

void Writer::Handle(Op<OpCode::MemoryBarrierTSO>, OpContext&) {
    as.FENCE(FenceOrder::RW, FenceOrder::RW);
}

void Writer::Handle(Op<OpCode::MemoryStoreFence>, OpContext&) {
    as.FENCE(FenceOrder::W, FenceOrder::W);
}

void Writer::Handle(Op<OpCode::MemoryLoadFence>, OpContext&) {
    as.FENCE(FenceOrder::R, FenceOrder::R);
}

void Writer::Handle(Op<OpCode::CpuYield>, OpContext&) { as.NOP(); }

void Writer::Handle(Op<OpCode::StoreUniformSelectImm>, OpContext& ctx) {
    const auto& body = source.Blocks()[block.value];
    const bool dispatch = ctx.Arg(0).Payload() == options.pc_offset &&
                          ctx.node.value + 1 == body.first_node + body.node_count &&
                          body.terminal.kind == TerminalKind::Dispatch && options.dispatcher &&
                          options.exit_handler;
    Read(ctx.Arg(1), a0);
    if (dispatch) {
        // This is the actual last store/terminal decision, so the condition
        // is consumed at its declared use, without extending a hidden SSA use.
        Label zero;
        as.BEQ(a0, x0, &zero);
        as.LI(a0, ctx.Arg(2).Payload());
        Store(a0, s1, options.pc_offset, 64);
        StaticExit(ctx.Arg(2).Payload());
        as.Bind(&zero);
        as.LI(a0, ctx.Arg(3).Payload());
        Store(a0, s1, options.pc_offset, 64);
        StaticExit(ctx.Arg(3).Payload());
        terminated = true;
        return;
    }
    Label zero, done;
    as.BEQ(a0, x0, &zero);
    as.LI(a1, ctx.Arg(2).Payload());
    as.J(&done);
    as.Bind(&zero);
    as.LI(a1, ctx.Arg(3).Payload());
    as.Bind(&done);
    Store(a1, s1, ctx.Arg(0).Payload(), 64);
    if (ctx.Arg(0).Payload() == options.pc_offset) static_pc_known = false;
    if (ctx.Arg(0).Payload() == options.flags_offset) as.MV(s11, a1);
}
}  // namespace swift::runtime::backend::riscv64::detail
