#include "emit.h"
#include "runtime/exit.h"

#include "runtime/arch/x86/bit_operations.h"

namespace swift::runtime::backend::riscv64::detail {
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
        as.SRLI(a2, s11, 29);
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

#define RV_ALU(name)                                                                               \
    void Writer::Handle(Op<OpCode::name>, OpContext& ctx) { IntegerArithmetic(ctx); }
RV_ALU(Add)
RV_ALU(Sub)
RV_ALU(Adc)
RV_ALU(Sbb)
RV_ALU(Neg)
#undef RV_ALU

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

#define RV_LOGIC(name)                                                                             \
    void Writer::Handle(Op<OpCode::name>, OpContext& ctx) { LogicalArithmetic(ctx); }
RV_LOGIC(And)
RV_LOGIC(Or)
RV_LOGIC(Xor)
RV_LOGIC(AndNot)
RV_LOGIC(Not)
#undef RV_LOGIC

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
    Read(ctx.Arg(0), a0);
    const auto op = ctx.operation.opcode;
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

#define RV_SHIFT(name)                                                                             \
    void Writer::Handle(Op<OpCode::name>, OpContext& ctx) { Shift(ctx); }
RV_SHIFT(LslImm)
RV_SHIFT(LslValue)
RV_SHIFT(LsrImm)
RV_SHIFT(LsrValue)
RV_SHIFT(AsrImm)
RV_SHIFT(AsrValue)
RV_SHIFT(RorImm)
RV_SHIFT(RorValue)
#undef RV_SHIFT

void Writer::Predicate(OpContext& ctx) {
    Read(ctx.Arg(0), ctx.destination);
    if (ctx.operation.opcode == OpCode::TestBit) {
        const auto bit = ctx.Arg(1).Payload();
        if (bit >= TypeBits(View(ctx.Arg(0))))
            as.MV(ctx.destination, x0);
        else {
            as.SRLI(ctx.destination, ctx.destination, bit);
            as.ANDI(ctx.destination, ctx.destination, 1);
        }
    } else if (ctx.operation.opcode == OpCode::TestZero)
        as.SEQZ(ctx.destination, ctx.destination);
    else
        as.SNEZ(ctx.destination, ctx.destination);
}

#define RV_PRED(name)                                                                              \
    void Writer::Handle(Op<OpCode::name>, OpContext& ctx) { Predicate(ctx); }
RV_PRED(TestBit)
RV_PRED(TestZero)
RV_PRED(TestNotZero)
#undef RV_PRED

void Writer::BitField(OpContext& ctx) {
    Read(ctx.Arg(0), a0, true);
    const bool insert = ctx.operation.opcode == OpCode::BitInsert;
    const auto lsb = ctx.Arg(insert ? 2 : 1).Payload();
    const auto length = ctx.Arg(insert ? 3 : 2).Payload();
    if (lsb >= 64 || length > 64 - lsb) Reject(CompileError::UnsupportedOperand);
    const u64 field = !length ? 0 : length == 64 ? UINT64_MAX : ((u64{1} << length) - 1) << lsb;
    if (ctx.operation.opcode == OpCode::BitExtract) {
        if (!length)
            as.MV(ctx.destination, x0);
        else {
            as.SRLI(ctx.destination, a0, lsb);
            Mask(ctx.destination, length);
        }
    } else {
        as.LI(t0, ~field);
        as.AND(ctx.destination, a0, t0);
        if (insert && length) {
            Read(ctx.Arg(1), a1, true);
            as.SLLI(a1, a1, lsb);
            as.LI(t0, field);
            as.AND(a1, a1, t0);
            as.OR(ctx.destination, ctx.destination, a1);
        }
    }
    Mask(ctx.destination, ctx.width);
}

#define RV_FIELD(name)                                                                             \
    void Writer::Handle(Op<OpCode::name>, OpContext& ctx) { BitField(ctx); }
RV_FIELD(BitExtract)
RV_FIELD(BitInsert)
RV_FIELD(BitClear)
#undef RV_FIELD

void Writer::Handle(Op<OpCode::ByteSwap>, OpContext& ctx) {
    Read(ctx.Arg(0), a0, true);
    const auto bits = ctx.Arg(1).Payload();
    if (bits != 16 && bits != 32 && bits != 64) Reject(CompileError::UnsupportedOperand);
    as.MV(ctx.destination, x0);
    for (unsigned byte = 0; byte < bits / 8; ++byte) {
        as.SLLI(ctx.destination, ctx.destination, 8);
        as.ANDI(t0, a0, 255);
        as.OR(ctx.destination, ctx.destination, t0);
        as.SRLI(a0, a0, 8);
    }
    Mask(ctx.destination, ctx.width);
}

void Writer::CountBits(OpContext& ctx) {
    Read(ctx.Arg(0), a0, true);
    const auto op = ctx.operation.opcode;
    const unsigned bits =
            op == OpCode::CountLeadingZeros32 || op == OpCode::CountTrailingZeros32 ? 32 : 64;
    Mask(a0, bits);
    Label done, repeat;
    as.LI(ctx.destination, bits);
    as.BEQ(a0, x0, &done);
    as.MV(ctx.destination, x0);
    if (op == OpCode::CountLeadingZeros32 || op == OpCode::CountLeadingZeros64) {
        if (bits == 32) as.SLLI(a0, a0, 32);
        as.Bind(&repeat);
        as.BLT(a0, x0, &done);
        as.SLLI(a0, a0, 1);
        as.ADDI(ctx.destination, ctx.destination, 1);
        as.J(&repeat);
    } else {
        as.Bind(&repeat);
        as.ANDI(t0, a0, 1);
        as.BNE(t0, x0, &done);
        as.SRLI(a0, a0, 1);
        as.ADDI(ctx.destination, ctx.destination, 1);
        as.J(&repeat);
    }
    as.Bind(&done);
}

#define RV_COUNT(name)                                                                             \
    void Writer::Handle(Op<OpCode::name>, OpContext& ctx) { CountBits(ctx); }
RV_COUNT(CountLeadingZeros32)
RV_COUNT(CountLeadingZeros64)
RV_COUNT(CountTrailingZeros32)
RV_COUNT(CountTrailingZeros64)
#undef RV_COUNT

void Writer::Handle(Op<OpCode::PopCount>, OpContext& ctx) {
    Read(ctx.Arg(0), a0, true);
    as.MV(ctx.destination, x0);
    Label repeat, done;
    as.Bind(&repeat);
    as.BEQ(a0, x0, &done);
    as.ADDI(t0, a0, -1);
    as.AND(a0, a0, t0);
    as.ADDI(ctx.destination, ctx.destination, 1);
    as.J(&repeat);
    as.Bind(&done);
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
}  // namespace swift::runtime::backend::riscv64::detail
