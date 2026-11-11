#include "emit.h"
#include "runtime/exit.h"

#include "runtime/arch/x86/bit_operations.h"

namespace swift::runtime::backend::loongarch64::detail {
void Writer::IntegerArithmetic(OpContext& ctx) {
    const auto op = ctx.operation.opcode;
    auto at = ctx.operands.begin();
    if (op == OpCode::Neg) {
        as.MV(a0, x0);
        Expression(at, ctx.operands.end(), a1);
    } else {
        Read(*at++, a0);
        Expression(at, ctx.operands.end(), a1);
    }
    Mask(a0, ctx.width);
    Mask(a1, ctx.width);
    const bool subtract = op == OpCode::Sub || op == OpCode::Sbb || op == OpCode::Neg;
    if (op == OpCode::Adc || op == OpCode::Sbb) {
        as.SRLI(a2, packed_flags, 29);
        as.ANDI(a2, a2, 1);
    } else
        as.MV(a2, x0);
    if (subtract) {
        as.SUB(t0, a0, a1);
        as.SUB(ctx.destination, t0, a2);
    } else {
        as.ADD(t0, a0, a1);
        as.ADD(ctx.destination, t0, a2);
    }
    const bool need_flags = ctx.operation.flag_outputs ||
                            allocation.homes[ctx.node.value].auxiliary_offset != UINT32_MAX;
    if (!need_flags) {
        Mask(ctx.destination, ctx.width);
        return;
    }
    if (subtract) {
        as.SLTU(t1, a0, a1);
        as.SLTU(t2, t0, a2);
        as.OR(t1, t1, t2);
    } else if (ctx.width == 64) {
        as.SLTU(t1, t0, a0);
        as.SLTU(t2, ctx.destination, t0);
        as.OR(t1, t1, t2);
    } else
        as.SRLI(t1, ctx.destination, ctx.width);
    Mask(ctx.destination, ctx.width);
    as.ANDI(t3, ctx.destination, 255);
    as.SEQZ(t2, ctx.destination);
    as.SLLI(t2, t2, 30);
    as.OR(t3, t3, t2);
    as.SRLI(t2, ctx.destination, ctx.width - 1);
    as.ANDI(t2, t2, 1);
    as.SLLI(t2, t2, 31);
    as.OR(t3, t3, t2);
    as.ANDI(t1, t1, 1);
    as.SLLI(t1, t1, 29);
    as.OR(t3, t3, t1);
    as.XOR(t0, a0, ctx.destination);
    as.XOR(t2, a0, a1);
    if (!subtract) as.NOT(t2, t2);
    as.AND(t2, t2, t0);
    as.SRLI(t2, t2, ctx.width - 1);
    as.ANDI(t2, t2, 1);
    as.SLLI(t2, t2, 28);
    as.OR(t3, t3, t2);
    as.XOR(t0, a0, a1);
    as.XOR(t0, t0, ctx.destination);
    as.ANDI(t0, t0, 16);
    as.SLLI(t0, t0, 22);
    as.OR(t3, t3, t0);
    SaveProducerFlags(ctx, t3);
}

#define LA_ALU(name)                                                                               \
    void Writer::Handle(Op<OpCode::name>, OpContext& ctx) { IntegerArithmetic(ctx); }
LA_ALU(Add)
LA_ALU(Sub)
LA_ALU(Adc)
LA_ALU(Sbb)
LA_ALU(Neg)
#undef LA_ALU

void Writer::LogicalArithmetic(OpContext& ctx) {
    auto at = ctx.operands.begin();
    Read(*at++, a0);
    const bool unary = at == ctx.operands.end();
    if (!unary) Expression(at, ctx.operands.end(), a1);
    switch (ctx.operation.opcode) {
        case OpCode::And:
            as.AND(ctx.destination, a0, a1);
            break;
        case OpCode::Or:
            as.OR(ctx.destination, a0, a1);
            break;
        case OpCode::Xor:
            as.XOR(ctx.destination, a0, a1);
            break;
        case OpCode::AndNot:
            as.NOT(a1, a1);
            as.AND(ctx.destination, a0, a1);
            break;
        case OpCode::Not:
            if (unary)
                as.SEQZ(ctx.destination, a0);
            else
                as.NOT(ctx.destination, a1);
            break;
        default:
            Reject(CompileError::UnsupportedOpcode);
    }
    Mask(ctx.destination, ctx.width);
    if (ctx.operation.flag_outputs ||
        allocation.homes[ctx.node.value].auxiliary_offset != UINT32_MAX) {
        as.ANDI(t3, ctx.destination, 255);
        as.SEQZ(t0, ctx.destination);
        as.SLLI(t0, t0, 30);
        as.OR(t3, t3, t0);
        as.SRLI(t0, ctx.destination, ctx.width - 1);
        as.SLLI(t0, t0, 31);
        as.OR(t3, t3, t0);
        SaveProducerFlags(ctx, t3);
    }
}

#define LA_LOGIC(name)                                                                             \
    void Writer::Handle(Op<OpCode::name>, OpContext& ctx) { LogicalArithmetic(ctx); }
LA_LOGIC(And)
LA_LOGIC(Or)
LA_LOGIC(Xor)
LA_LOGIC(AndNot)
LA_LOGIC(Not)
#undef LA_LOGIC

void Writer::Handle(Op<OpCode::Mul>, OpContext& ctx) {
    Read(ctx.Arg(0), a0);
    Read(ctx.Arg(1), a1);
    as.MUL(ctx.destination, a0, a1);
    Mask(ctx.destination, ctx.width);
}

void Writer::Handle(Op<OpCode::MulSub>, OpContext& ctx) {
    Read(ctx.Arg(0), a0);
    Read(ctx.Arg(1), a1);
    Read(ctx.Arg(2), a2);
    as.MUL(a0, a0, a1);
    as.SUB(ctx.destination, a2, a0);
    Mask(ctx.destination, ctx.width);
}

void Writer::Handle(Op<OpCode::MulHigh>, OpContext& ctx) {
    Read(ctx.Arg(0), a0);
    Read(ctx.Arg(1), a1);
    const auto bits = TypeBits(View(ctx.Arg(0)));
    if (ctx.Arg(2).Payload()) {
        SignExtend(a0, bits);
        SignExtend(a1, bits);
    }
    if (bits == 64) {
        if (ctx.Arg(2).Payload())
            as.MULH(ctx.destination, a0, a1);
        else
            as.MULHU(ctx.destination, a0, a1);
    } else {
        as.MUL(ctx.destination, a0, a1);
        as.SRLI(ctx.destination, ctx.destination, bits);
    }
    Mask(ctx.destination, ctx.width);
}

void Writer::Handle(Op<OpCode::Div>, OpContext& ctx) {
    Read(ctx.Arg(0), a0);
    auto at = ctx.operands.begin();
    ++at;
    Expression(at, ctx.operands.end(), a1);
    Label nonzero;
    as.BNE(a1, x0, &nonzero);
    Exit(ExitValue(NativeExit::DivideFault));
    as.Bind(&nonzero);
    as.DIVU(ctx.destination, a0, a1);
    Mask(ctx.destination, ctx.width);
}

void Writer::Handle(Op<OpCode::SignedDiv64>, OpContext& ctx) {
    Read(ctx.Arg(0), a0);
    Read(ctx.Arg(1), a1);
    Label nonzero, valid;
    as.BNE(a1, x0, &nonzero);
    Exit(ExitValue(NativeExit::DivideFault));
    as.Bind(&nonzero);
    as.LI(t0, u64{1} << 63);
    as.BNE(a0, t0, &valid);
    as.LI(t0, UINT64_MAX);
    as.BNE(a1, t0, &valid);
    Exit(ExitValue(NativeExit::DivideFault));
    as.Bind(&valid);
    as.DIV(ctx.destination, a0, a1);
}

void Writer::Shift(OpContext& ctx) {
    const auto op = ctx.operation.opcode;
    if (op == OpCode::RorImm && (ctx.width == 32 || ctx.width == 64)) {
        Read(ctx.Arg(0), a0, true);
        const auto count = static_cast<unsigned>(ctx.Arg(1).Payload()) & (ctx.width - 1);
        if (ctx.width == 32)
            as.ROTRI_W(ctx.destination, a0, count);
        else
            as.ROTRI_D(ctx.destination, a0, count);
        return;
    }
    Read(ctx.Arg(0), a0);
    const bool left = op == OpCode::LslImm || op == OpCode::LslValue;
    const bool right = op == OpCode::LsrImm || op == OpCode::LsrValue;
    const bool arithmetic = op == OpCode::AsrImm || op == OpCode::AsrValue;
    if (arithmetic)
        SignExtend(a0, std::min(ctx.width, TypeBits(View(ctx.Arg(0)))));
    else
        Mask(a0, ctx.width);
    Read(ctx.Arg(1), a1);
    as.ANDI(a1, a1, ctx.width - 1);
    if (left)
        as.SLL(ctx.destination, a0, a1);
    else if (right)
        as.SRL(ctx.destination, a0, a1);
    else if (arithmetic)
        as.SRA(ctx.destination, a0, a1);
    else {
        as.NEG(t0, a1);
        as.ANDI(t0, t0, ctx.width - 1);
        as.SRL(t1, a0, a1);
        as.SLL(ctx.destination, a0, t0);
        as.OR(ctx.destination, ctx.destination, t1);
    }
    Mask(ctx.destination, ctx.width);
}

#define LA_SHIFT(name)                                                                             \
    void Writer::Handle(Op<OpCode::name>, OpContext& ctx) { Shift(ctx); }
LA_SHIFT(LslImm)
LA_SHIFT(LslValue)
LA_SHIFT(LsrImm)
LA_SHIFT(LsrValue)
LA_SHIFT(AsrImm)
LA_SHIFT(AsrValue)
LA_SHIFT(RorImm)
LA_SHIFT(RorValue)
#undef LA_SHIFT

void Writer::Predicate(OpContext& ctx) {
    const auto op = ctx.operation.opcode;
    if (op == OpCode::TestBit) {
        const auto bit = ctx.Arg(1).Payload();
        const auto bits = TypeBits(View(ctx.Arg(0)));
        if (bit >= bits) {
            as.MV(ctx.destination, x0);
            return;
        }
        Read(ctx.Arg(0), ctx.destination, true);
        if (bits <= 32)
            as.BSTRPICK_W(ctx.destination, ctx.destination, bit, bit);
        else
            as.BSTRPICK_D(ctx.destination, ctx.destination, bit, bit);
        return;
    }
    Read(ctx.Arg(0), ctx.destination);
    if (op == OpCode::TestZero)
        as.SEQZ(ctx.destination, ctx.destination);
    else
        as.SNEZ(ctx.destination, ctx.destination);
}

#define LA_PRED(name)                                                                              \
    void Writer::Handle(Op<OpCode::name>, OpContext& ctx) { Predicate(ctx); }
LA_PRED(TestBit)
LA_PRED(TestZero)
LA_PRED(TestNotZero)
#undef LA_PRED

void Writer::BitField(OpContext& ctx) {
    const auto op = ctx.operation.opcode;
    const bool insert = op == OpCode::BitInsert;
    const bool extract = op == OpCode::BitExtract;
    const auto lsb = ctx.Arg(insert ? 2 : 1).Payload();
    const auto length = ctx.Arg(insert ? 3 : 2).Payload();
    if (lsb >= 64 || length > 64 - lsb) Reject(CompileError::UnsupportedOperand);
    if (!length) {
        if (extract)
            as.MV(ctx.destination, x0);
        else {
            Read(ctx.Arg(0), ctx.destination, true);
            Mask(ctx.destination, ctx.width);
        }
        return;
    }
    const auto msb = lsb + length - 1;
    if (extract) {
        if (ctx.width && ctx.width <= 32 && msb < ctx.width) {
            Read(ctx.Arg(0), ctx.destination, true);
            as.BSTRPICK_W(ctx.destination, ctx.destination, msb, lsb);
        } else if (ctx.width == 64) {
            Read(ctx.Arg(0), ctx.destination, true);
            as.BSTRPICK_D(ctx.destination, ctx.destination, msb, lsb);
        } else {
            Read(ctx.Arg(0), a0, true);
            as.SRLI(ctx.destination, a0, lsb);
            Mask(ctx.destination, length);
            Mask(ctx.destination, ctx.width);
        }
        return;
    }
    if (ctx.width == 64 || (ctx.width == 32 && msb < 32)) {
        // Read the inserted value before writing the destination: allocation may
        // reuse an input home that dies at this operation.
        if (insert) Read(ctx.Arg(1), a1, true);
        Read(ctx.Arg(0), ctx.destination, true);
        if (insert) {
            if (ctx.width == 32)
                as.BSTRINS_W(ctx.destination, a1, msb, lsb);
            else
                as.BSTRINS_D(ctx.destination, a1, msb, lsb);
        } else if (ctx.width == 32)
            as.BSTRINS_W(ctx.destination, x0, msb, lsb);
        else
            as.BSTRINS_D(ctx.destination, x0, msb, lsb);
        return;
    }
    const u64 field = length == 64 ? UINT64_MAX : ((u64{1} << length) - 1) << lsb;
    Read(ctx.Arg(0), a0, true);
    as.LI(t0, ~field);
    as.AND(ctx.destination, a0, t0);
    if (insert) {
        Read(ctx.Arg(1), a1, true);
        as.SLLI(a1, a1, lsb);
        as.LI(t0, field);
        as.AND(a1, a1, t0);
        as.OR(ctx.destination, ctx.destination, a1);
    }
    Mask(ctx.destination, ctx.width);
}

#define LA_FIELD(name)                                                                             \
    void Writer::Handle(Op<OpCode::name>, OpContext& ctx) { BitField(ctx); }
LA_FIELD(BitExtract)
LA_FIELD(BitInsert)
LA_FIELD(BitClear)
#undef LA_FIELD

void Writer::Handle(Op<OpCode::ByteSwap>, OpContext& ctx) {
    Read(ctx.Arg(0), a0, true);
    const auto bits = ctx.Arg(1).Payload();
    if (bits != 16 && bits != 32 && bits != 64) Reject(CompileError::UnsupportedOperand);
    if (bits == 16)
        as.revb_2h(ctx.destination, a0);
    else if (bits == 32)
        as.revb_2w(ctx.destination, a0);
    else
        as.revb_d(ctx.destination, a0);
    Mask(ctx.destination, std::min<unsigned>(bits, ctx.width));
}

void Writer::CountBits(OpContext& ctx) {
    Read(ctx.Arg(0), a0, true);
    switch (ctx.operation.opcode) {
        case OpCode::CountLeadingZeros32:
            as.clz_w(ctx.destination, a0);
            break;
        case OpCode::CountLeadingZeros64:
            as.clz_d(ctx.destination, a0);
            break;
        case OpCode::CountTrailingZeros32:
            as.ctz_w(ctx.destination, a0);
            break;
        case OpCode::CountTrailingZeros64:
            as.ctz_d(ctx.destination, a0);
            break;
        default:
            Reject(CompileError::UnsupportedOpcode);
    }
}

#define LA_COUNT(name)                                                                             \
    void Writer::Handle(Op<OpCode::name>, OpContext& ctx) { CountBits(ctx); }
LA_COUNT(CountLeadingZeros32)
LA_COUNT(CountLeadingZeros64)
LA_COUNT(CountTrailingZeros32)
LA_COUNT(CountTrailingZeros64)
#undef LA_COUNT

void Writer::Handle(Op<OpCode::PopCount>, OpContext& ctx) {
    Read(ctx.Arg(0), a0, true);
    as.LI(t0, UINT64_C(0x5555555555555555));
    as.SRLI(t1, a0, 1);
    as.AND(t1, t1, t0);
    as.SUB(a0, a0, t1);
    as.LI(t0, UINT64_C(0x3333333333333333));
    as.AND(t1, a0, t0);
    as.SRLI(a0, a0, 2);
    as.AND(a0, a0, t0);
    as.ADD(a0, a0, t1);
    as.SRLI(t1, a0, 4);
    as.ADD(a0, a0, t1);
    as.LI(t0, UINT64_C(0x0f0f0f0f0f0f0f0f));
    as.AND(a0, a0, t0);
    as.LI(t0, UINT64_C(0x0101010101010101));
    as.MUL(ctx.destination, a0, t0);
    as.SRLI(ctx.destination, ctx.destination, 56);
    Mask(ctx.destination, ctx.width);
}

void Writer::Handle(Op<OpCode::ParallelBitDeposit>, OpContext& ctx) {
    Read(ctx.Arg(0), a0);
    Read(ctx.Arg(1), a1);
    Call(reinterpret_cast<std::uintptr_t>(&::swift::runtime::x86::ParallelBitDeposit),
         HostSymbol::DepositBits);
    as.MV(ctx.destination, a0);
    Mask(ctx.destination, ctx.width);
}

void Writer::Handle(Op<OpCode::ParallelBitExtract>, OpContext& ctx) {
    Read(ctx.Arg(0), a0);
    Read(ctx.Arg(1), a1);
    Call(reinterpret_cast<std::uintptr_t>(&::swift::runtime::x86::ParallelBitExtract),
         HostSymbol::ExtractBits);
    as.MV(ctx.destination, a0);
    Mask(ctx.destination, ctx.width);
}

void Writer::Handle(Op<OpCode::Crc32c>, OpContext& ctx) {
    Read(ctx.Arg(0), a0, true);
    Read(ctx.Arg(1), a1, true);
    Mask(a0, 32);
    for (unsigned byte = 0; byte < ctx.Arg(2).Payload() / 8; ++byte) {
        as.ANDI(t0, a1, 255);
        as.XOR(a0, a0, t0);
        as.SRLI(a1, a1, 8);
        for (unsigned bit = 0; bit < 8; ++bit) {
            as.ANDI(t0, a0, 1);
            as.NEG(t0, t0);
            as.LI(t1, 0x82f63b78);
            as.AND(t0, t0, t1);
            as.SRLI(a0, a0, 1);
            as.XOR(a0, a0, t0);
        }
    }
    as.MV(ctx.destination, a0);
}
}  // namespace swift::runtime::backend::loongarch64::detail
