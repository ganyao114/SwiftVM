#include "abi_layout.inc"
#include "atomic_helpers.h"
#include "emit.h"
#include "runtime/atomic.h"
#include "runtime/exit.h"

namespace swift::runtime::backend::loongarch64::detail {
namespace {

enum class AtomicKind {
    Compare,
    Exchange,
    Add,
    And,
    Or,
    Xor,
    Neg,
    SignedMax,
    SignedMin,
    UnsignedMax,
    UnsignedMin
};

AtomicOperation ServiceOperation(Writer& writer, const OpContext& ctx) {
    switch (ctx.operation.opcode) {
        case OpCode::CompareAndSwap:
            return AtomicOperation::CompareExchange;
        case OpCode::CompareAndSwap128:
            return AtomicOperation::CompareExchange128;
        case OpCode::AtomicExchange:
            return AtomicOperation::Exchange;
        case OpCode::AtomicFetchAdd:
            return AtomicOperation::FetchAdd;
        default:
            break;
    }
    switch (static_cast<AtomicRMWOp>(ctx.Arg(0).Payload())) {
        case AtomicRMWOp::Add:
            return AtomicOperation::Add;
        case AtomicRMWOp::Sub:
            return AtomicOperation::Sub;
        case AtomicRMWOp::And:
            return AtomicOperation::And;
        case AtomicRMWOp::Or:
            return AtomicOperation::Or;
        case AtomicRMWOp::Xor:
            return AtomicOperation::Xor;
        case AtomicRMWOp::AddCarry:
            return AtomicOperation::AddCarry;
        case AtomicRMWOp::SubBorrow:
            return AtomicOperation::SubBorrow;
        case AtomicRMWOp::Neg:
            return AtomicOperation::Neg;
        case AtomicRMWOp::SignedMax:
            return AtomicOperation::SignedMax;
        case AtomicRMWOp::SignedMin:
            return AtomicOperation::SignedMin;
        case AtomicRMWOp::UnsignedMax:
            return AtomicOperation::UnsignedMax;
        case AtomicRMWOp::UnsignedMin:
            return AtomicOperation::UnsignedMin;
    }
    writer.Reject(CompileError::UnsupportedOperand);
    return {};
}

void ColdAtomic(Writer& writer, OpContext& ctx) {
    auto& as = writer.as;
    if (!(writer.options.dispatcher && writer.options.exit_handler)) {
        writer.Exit(ExitValue(NativeExit::UnsupportedAtomic));
        return;
    }
    const auto operation = ServiceOperation(writer, ctx);
    const bool wide = operation == AtomicOperation::CompareExchange128;
    // Stage every fixed home before moving sp; no spill-home bias or permanent
    // per-operation slot is needed. The transient packet is the cold C ABI.
    writer.Read(ctx.Arg(ctx.operation.opcode == OpCode::AtomicRMW ? 1 : 0), a0);
    as.MV(a1, x0);
    as.MV(a2, x0);
    as.MV(a3, x0);
    as.MV(a4, x0);
    as.MV(a5, x0);
    if (wide) {
        writer.Read(ctx.Arg(1), a1);
        writer.Read(ctx.Arg(2), a2);
        writer.Read(ctx.Arg(3), a3);
        writer.Read(ctx.Arg(4), a4);
    } else if (operation == AtomicOperation::CompareExchange) {
        writer.Read(ctx.Arg(1), a1);
        writer.Read(ctx.Arg(2), a3);
    } else {
        const bool rmw = ctx.operation.opcode == OpCode::AtomicRMW;
        writer.Read(ctx.Arg(rmw ? 2 : 1), a3);
        if (rmw) {
            writer.Read(ctx.Arg(3), a5);
            writer.Mask(a5, 1);
        }
    }
    if (!wide) {
        writer.Mask(a1, ctx.width);
        writer.Mask(a3, ctx.width);
    }
    if (writer.options.pc_offset != UINT32_MAX) {
        as.LI(t0, writer.guest_pc);
        writer.Store(t0, state, writer.options.pc_offset, 64);
    }
    writer.Load(t5, state, SVM_LA_CTX_REL(SVM_LA_CTX_ATOMIC_SERVICE), 64);
    Label available;
    as.BNE(t5, x0, &available);
    writer.Exit(ExitValue(NativeExit::UnsupportedAtomic));
    as.Bind(&available);
    constexpr auto request = offsetof(AtomicCall, request);
    constexpr auto frame = sizeof(AtomicCall);
    writer.FrameDelta(-static_cast<std::int64_t>(frame));
    writer.FrameStore(a0, request + offsetof(AtomicRequest, address));
    // Both host ISAs are little endian; operation and bits occupy one word.
    as.LI(t0, static_cast<u64>(operation) | (u64{ctx.width} << 32));
    writer.FrameStore(t0, request + offsetof(AtomicRequest, operation));
    writer.FrameStore(a1, request + offsetof(AtomicRequest, expected));
    writer.FrameStore(a2, request + offsetof(AtomicRequest, expected) + 8);
    writer.FrameStore(a3, request + offsetof(AtomicRequest, desired));
    writer.FrameStore(a4, request + offsetof(AtomicRequest, desired) + 8);
    writer.FrameStore(a5, request + offsetof(AtomicRequest, carry));
    writer.Load(a0, state, SVM_LA_CTX_REL(SVM_LA_CTX_OPAQUE), 64);
    as.MV(a1, sp);
    writer.CallRegister(t5);
    as.MV(t0, a0);
    writer.FrameLoad(a0, offsetof(AtomicCall, observed));
    writer.FrameLoad(a1, offsetof(AtomicCall, observed) + 8);
    writer.FrameDelta(frame);
    Label completed, fault;
    as.BEQ(t0, x0, &completed);
    as.LI(t1, static_cast<u64>(AtomicStatus::Fault));
    as.BEQ(t0, t1, &fault);
    writer.Exit(ExitValue(NativeExit::UnsupportedAtomic));
    as.Bind(&fault);
    writer.Exit(ExitValue(NativeExit::MemoryFault));
    as.Bind(&completed);
    as.MV(ctx.destination, a0);
    if (wide)
        as.MV(ctx.high, a1);
    else
        writer.Mask(ctx.destination, ctx.width);
}

void CheckAlignment(Writer& writer, unsigned bytes) {
    if (bytes == 1) return;
    Label aligned;
    writer.as.ANDI(t0, a0, bytes - 1);
    writer.as.BEQ(t0, x0, &aligned);
    // Standalone CAS128 has no execution owner to qualify a cold access.
    // The frontend separately enforces the guest instruction's alignment.
    writer.Exit(ExitValue(NativeExit::UnsupportedAtomic));
    writer.as.Bind(&aligned);
}

void EmitAmo(Assembler& as, AtomicKind kind, unsigned bits) {
    const auto emit = [&](auto word, auto wide) {
        if (bits == 32)
            (as.*word)(Ordering::AQRL, t0, a1, a0);
        else
            (as.*wide)(Ordering::AQRL, t0, a1, a0);
    };
    switch (kind) {
        case AtomicKind::Exchange:
            emit(&Assembler::AMOSWAP_W, &Assembler::AMOSWAP_D);
            break;
        case AtomicKind::Add:
            emit(&Assembler::AMOADD_W, &Assembler::AMOADD_D);
            break;
        case AtomicKind::And:
            emit(&Assembler::AMOAND_W, &Assembler::AMOAND_D);
            break;
        case AtomicKind::Or:
            emit(&Assembler::AMOOR_W, &Assembler::AMOOR_D);
            break;
        case AtomicKind::Xor:
            emit(&Assembler::AMOXOR_W, &Assembler::AMOXOR_D);
            break;
        default:
            break;  // Compare and Neg use the reservation loop.
    }
}

void Replacement(Writer& writer, AtomicKind kind, unsigned bits, Label& write) {
    auto& as = writer.as;
    switch (kind) {
        case AtomicKind::Compare:
            as.MV(t2, t0);
            as.BNE(t0, a1, &write);  // Failure still stores the observed word.
            as.MV(t2, a2);
            break;
        case AtomicKind::Exchange:
            as.MV(t2, a1);
            break;
        case AtomicKind::Add:
            as.ADD(t2, t0, a1);
            break;
        case AtomicKind::And:
            as.AND(t2, t0, a1);
            break;
        case AtomicKind::Or:
            as.OR(t2, t0, a1);
            break;
        case AtomicKind::Xor:
            as.XOR(t2, t0, a1);
            break;
        case AtomicKind::Neg:
            as.NEG(t2, t0);
            break;
        case AtomicKind::SignedMax:
        case AtomicKind::SignedMin:
            as.MV(t2, t0);
            as.MV(a4, t0);
            as.MV(a5, a1);
            writer.SignExtend(a4, bits);
            writer.SignExtend(a5, bits);
            if (kind == AtomicKind::SignedMax)
                as.BGE(a4, a5, &write);
            else
                as.BGE(a5, a4, &write);
            as.MV(t2, a1);
            break;
        case AtomicKind::UnsignedMax:
        case AtomicKind::UnsignedMin:
            as.MV(t2, t0);
            if (kind == AtomicKind::UnsignedMax)
                as.BGEU(t0, a1, &write);
            else
                as.BGEU(a1, t0, &write);
            as.MV(t2, a1);
            break;
    }
}

void EmitReservation(Writer& writer, AtomicKind kind, unsigned bits) {
    auto& as = writer.as;
    if (bits < 32) {
        // The aligned word stays in the same page as the naturally aligned
        // byte/halfword. Preserve every neighboring bit in the SC replacement.
        as.SRLI(a3, a0, 2);
        as.SLLI(a3, a3, 2);
        as.ANDI(t5, a0, 3);
        as.SLLI(t5, t5, 3);
        as.LI(t4, (u64{1} << bits) - 1);
        as.SLL(a6, t4, t5);
        as.NOT(a6, a6);
    }
    Label retry, write;
    as.Bind(&retry);
    const auto first_load = static_cast<u32>(writer.output.words.size() * 4);
    if (bits == 64)
        as.LR_D(Ordering::AQ, t0, a0);
    else if (bits == 32) {
        as.LR_W(Ordering::AQ, t0, a0);
        writer.Mask(t0, 32);
    } else {
        as.LR_W(Ordering::AQ, t1, a3);
        as.SRL(t0, t1, t5);
        as.AND(t0, t0, t4);
    }
    writer.RecordGuestAccess(first_load, 3, bits / 8);
    Replacement(writer, kind, bits, write);
    as.Bind(&write);
    if (bits < 32) {
        as.AND(t2, t2, t4);
        as.SLL(t2, t2, t5);
        as.AND(t1, t1, a6);
        as.OR(t2, t2, t1);
        as.SC_W(Ordering::RL, t3, t2, a3);
    } else if (bits == 32)
        as.SC_W(Ordering::RL, t3, t2, a0);
    else
        as.SC_D(Ordering::RL, t3, t2, a0);
    writer.RecordGuestAccess(static_cast<u32>(writer.output.words.size() * 4 - 4), 3, bits / 8);
    as.BNE(t3, x0, &retry);
}

AtomicKind PrepareRmw(Writer& writer, AtomicRMWOp kind) {
    auto& as = writer.as;
    if (kind == AtomicRMWOp::AddCarry || kind == AtomicRMWOp::SubBorrow) {
        as.ANDI(a2, a2, 1);
        as.ADD(a1, a1, a2);
    }
    switch (kind) {
        case AtomicRMWOp::Sub:
        case AtomicRMWOp::SubBorrow:
            as.NEG(a1, a1);
            return AtomicKind::Add;
        case AtomicRMWOp::And:
            return AtomicKind::And;
        case AtomicRMWOp::Or:
            return AtomicKind::Or;
        case AtomicRMWOp::Xor:
            return AtomicKind::Xor;
        case AtomicRMWOp::Neg:
            return AtomicKind::Neg;
        case AtomicRMWOp::SignedMax:
            return AtomicKind::SignedMax;
        case AtomicRMWOp::SignedMin:
            return AtomicKind::SignedMin;
        case AtomicRMWOp::UnsignedMax:
            return AtomicKind::UnsignedMax;
        case AtomicRMWOp::UnsignedMin:
            return AtomicKind::UnsignedMin;
        default:
            return AtomicKind::Add;
    }
}
}  // namespace

void Writer::Atomic(OpContext& ctx) {
    const auto opcode = ctx.operation.opcode;
    const bool compare = opcode == OpCode::CompareAndSwap;
    const bool rmw = opcode == OpCode::AtomicRMW;
    if (ctx.width != 8 && ctx.width != 16 && ctx.width != 32 && ctx.width != 64)
        Reject(CompileError::UnsupportedType);
    const auto raw_kind = rmw ? ctx.Arg(0).Payload() : 0;
    if (rmw && raw_kind > static_cast<u64>(AtomicRMWOp::UnsignedMin))
        Reject(CompileError::UnsupportedOperand);
    Read(ctx.Arg(rmw ? 1 : 0), a0);
    Read(ctx.Arg(rmw ? 2 : 1), a1);
    Mask(a1, ctx.width);
    if (compare || rmw) {
        Read(ctx.Arg(rmw ? 3 : 2), a2);
        Mask(a2, compare ? ctx.width : 1);
    }
    Label aligned, done;
    if (ctx.width != 8) {
        as.ANDI(t0, a0, ctx.width / 8 - 1);
        as.BEQ(t0, x0, &aligned);
        ColdAtomic(*this, ctx);
        as.J(&done);
        as.Bind(&aligned);
    }
    auto kind = compare                            ? AtomicKind::Compare
                : opcode == OpCode::AtomicExchange ? AtomicKind::Exchange
                                                   : AtomicKind::Add;
    if (rmw) kind = PrepareRmw(*this, static_cast<AtomicRMWOp>(raw_kind));
    // Full fences include the comparison-mismatch path and surround ordinary
    // guest accesses, in addition to the acquire/release atomic instruction.
    as.FENCE(FenceOrder::RW, FenceOrder::RW);
    if (ctx.width >= 32 && kind >= AtomicKind::Exchange && kind <= AtomicKind::Xor) {
        EmitAmo(as, kind, ctx.width);
        RecordGuestAccess(static_cast<u32>(output.words.size() * 4 - 4), 3, ctx.width / 8);
    } else
        EmitReservation(*this, kind, ctx.width);
    as.FENCE(FenceOrder::RW, FenceOrder::RW);
    as.MV(ctx.destination, t0);
    Mask(ctx.destination, ctx.width);
    if (ctx.width != 8) as.Bind(&done);
}

void Writer::Handle(Op<OpCode::CompareAndSwap>, OpContext& ctx) { Atomic(ctx); }

void Writer::Handle(Op<OpCode::AtomicExchange>, OpContext& ctx) { Atomic(ctx); }

void Writer::Handle(Op<OpCode::AtomicFetchAdd>, OpContext& ctx) { Atomic(ctx); }

void Writer::Handle(Op<OpCode::AtomicRMW>, OpContext& ctx) { Atomic(ctx); }

void Writer::Handle(Op<OpCode::CompareAndSwap128>, OpContext& ctx) {
    if (ctx.operation.type != Type::V128) Reject(CompileError::UnsupportedType);
    if (options.dispatcher && options.exit_handler) {
        ColdAtomic(*this, ctx);
        return;
    }
    Read(ctx.Arg(0), a0);
    CheckAlignment(*this, 16);
    Read(ctx.Arg(1), a1);
    Read(ctx.Arg(2), a2);
    Read(ctx.Arg(3), a3);
    Read(ctx.Arg(4), a4);
    Call(reinterpret_cast<std::uintptr_t>(&CompareExchange128),
         HostSymbol::LoongCompareExchange128);
    as.MV(ctx.destination, a0);
    as.MV(ctx.high, a1);
}

}  // namespace swift::runtime::backend::loongarch64::detail
