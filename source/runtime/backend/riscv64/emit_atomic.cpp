#include "emit.h"
#include "runtime/exit.h"
#include "atomic_helpers.h"
#include "abi_layout.inc"

namespace swift::runtime::backend::riscv64::detail {
namespace {

enum class AtomicKind { Compare, Exchange, Add, And, Or, Xor, Neg };

void CheckAlignment(Writer& writer, unsigned bytes) {
    if (bytes == 1) return;
    Label aligned;
    writer.as.ANDI(t0, a0, bytes - 1);
    writer.as.BEQ(t0, x0, &aligned);
    // Native reason 6 is an unsupported misaligned atomic, before any
    // memory access. Runtime stops without guest-fault fabrication or
    // interpreter replay. A mutex+memcpy fallback cannot synchronize with
    // the ordinary hardware-atomic path and is deliberately not used.
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

void Replacement(Assembler& as, AtomicKind kind, Label& write) {
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
    }
}

void EmitReservation(Writer& writer, AtomicKind kind, unsigned bits) {
    auto& as = writer.as;
    if (bits < 32) {
        // The aligned word stays in the same page as the naturally aligned
        // byte/halfword. Preserve every neighboring bit in the SC replacement.
        as.ANDI(a3, a0, -4);
        as.ANDI(t5, a0, 3);
        as.SLLI(t5, t5, 3);
        as.LI(t4, (u64{1} << bits) - 1);
        as.SLL(a6, t4, t5);
        as.NOT(a6, a6);
    }
    Label retry, write;
    as.Bind(&retry);
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
    Replacement(as, kind, write);
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
    if (rmw && raw_kind > static_cast<u64>(AtomicRMWOp::SubBorrow))
        Reject(CompileError::UnsupportedOperand);
    Read(ctx.Arg(rmw ? 1 : 0), a0);
    Read(ctx.Arg(rmw ? 2 : 1), a1);
    Mask(a1, ctx.width);
    if (compare || rmw) {
        Read(ctx.Arg(rmw ? 3 : 2), a2);
        Mask(a2, compare ? ctx.width : 1);
    }
    CheckAlignment(*this, ctx.width / 8);
    auto kind = compare                            ? AtomicKind::Compare
                : opcode == OpCode::AtomicExchange ? AtomicKind::Exchange
                                                   : AtomicKind::Add;
    if (rmw) kind = PrepareRmw(*this, static_cast<AtomicRMWOp>(raw_kind));
    // Full fences include the comparison-mismatch path and surround ordinary
    // guest accesses, in addition to the acquire/release atomic instruction.
    as.FENCE(FenceOrder::RW, FenceOrder::RW);
    if (ctx.width >= 32 && kind != AtomicKind::Compare && kind != AtomicKind::Neg)
        EmitAmo(as, kind, ctx.width);
    else
        EmitReservation(*this, kind, ctx.width);
    as.FENCE(FenceOrder::RW, FenceOrder::RW);
    as.MV(ctx.destination, t0);
    Mask(ctx.destination, ctx.width);
}

void Writer::Handle(Op<OpCode::CompareAndSwap>, OpContext& ctx) { Atomic(ctx); }

void Writer::Handle(Op<OpCode::AtomicExchange>, OpContext& ctx) { Atomic(ctx); }

void Writer::Handle(Op<OpCode::AtomicFetchAdd>, OpContext& ctx) { Atomic(ctx); }

void Writer::Handle(Op<OpCode::AtomicRMW>, OpContext& ctx) { Atomic(ctx); }

void Writer::Handle(Op<OpCode::CompareAndSwap128>, OpContext& ctx) {
    if (ctx.operation.type != Type::V128) Reject(CompileError::UnsupportedType);
    Read(ctx.Arg(0), a0);
    CheckAlignment(*this, 16);
    if (options.dispatcher && options.exit_handler) {
        // Qualify anonymous stable memory and remove an armed SMC watch
        // before libatomic can hold a lock. Never longjmp out of that lock.
        Load(t5, s1, SVM_RV_CTX_REL(SVM_RV_CTX_PREPARE_ATOMIC), 64);
        Label unsupported, fault, ready;
        as.BEQ(t5, x0, &unsupported);
        as.MV(a1, a0);
        Load(a0, s1, SVM_RV_CTX_REL(SVM_RV_CTX_OPAQUE), 64);
        as.LI(a2, 16);
        CallRegister(t5);
        as.BEQ(a0, x0, &ready);
        as.LI(t0, 1);
        as.BEQ(a0, t0, &fault);
        as.Bind(&unsupported);
        Exit(ExitValue(NativeExit::UnsupportedAtomic));
        as.Bind(&fault);
        Exit(ExitValue(NativeExit::MemoryFault));
        as.Bind(&ready);
    }
    Read(ctx.Arg(0), a0);
    Read(ctx.Arg(1), a1);
    Read(ctx.Arg(2), a2);
    Read(ctx.Arg(3), a3);
    Read(ctx.Arg(4), a4);
    Call(reinterpret_cast<std::uintptr_t>(&CompareExchange128));
    as.MV(ctx.destination, a0);
    as.MV(ctx.high, a1);
}

}  // namespace swift::runtime::backend::riscv64::detail
