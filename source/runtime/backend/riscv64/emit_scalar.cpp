#include "emit.h"
#include "runtime/exit.h"

#include <algorithm>
#include "runtime/arch/x86/bit_operations.h"

namespace swift::runtime::backend::riscv64::detail {
namespace {
constexpr std::int64_t kImm12Min = -2048;
constexpr std::int64_t kImm12Max = 2047;
constexpr unsigned kCarryBit = 29;
constexpr u64 kCrc32cPolyReflected = 0x82f63b78ULL;
constexpr u64 kCrc32cPolyStar = 0x105ec76f1ULL;
constexpr u64 kCrc32cMu8 = 0xf1ULL;
constexpr u64 kCrc32cMu16 = 0x13f1ULL;
constexpr u64 kCrc32cMu32 = 0xdea713f1ULL;

bool TrySignedImm12(const Writer& writer, CellView cell, unsigned width, std::int32_t& out) {
    if (cell.Kind() != OperandKind::Immediate) return false;
    const unsigned view_bits = std::min(64u, TypeBits(writer.View(cell)));
    const u64 val = view_bits >= 64 ? cell.Payload() : (cell.Payload() & ((u64{1} << view_bits) - 1));
    const unsigned w = width ? std::min(64u, width) : 64u;
    const std::int64_t simm =
            w >= 64 ? static_cast<std::int64_t>(val)
                    : ((static_cast<std::int64_t>(val) << (64 - w)) >> (64 - w));
    if (simm < kImm12Min || simm > kImm12Max) return false;
    out = static_cast<std::int32_t>(simm);
    return true;
}

void EmitBranchlessClz64(Assembler& as, GPR dst, GPR x) {
    auto step = [&](unsigned shift, unsigned log2_shift, bool first) {
        as.SRLI(t0, x, shift);
        as.SNEZ(t1, t0);
        as.SLLI(t1, t1, log2_shift);
        as.SRL(x, x, t1);
        if (first) {
            as.XORI(dst, t1, shift);
        } else {
            as.XORI(t2, t1, shift);
            as.ADD(dst, dst, t2);
        }
    };
    step(32, 5, true);
    step(16, 4, false);
    step(8, 3, false);
    step(4, 2, false);
    step(2, 1, false);
    as.SLTIU(t0, x, 2);
    as.SEQZ(t1, x);
    as.ADD(t0, t0, t1);
    as.ADD(dst, dst, t0);
}
}  // namespace

void Writer::IntegerArithmetic(OpContext& ctx) {
    const auto op = ctx.operation.opcode;
    const bool subtract = op == OpCode::Sub || op == OpCode::Sbb || op == OpCode::Neg;
    const bool with_carry = op == OpCode::Adc || op == OpCode::Sbb;
    const bool need_aux = allocation.homes[ctx.node.value].auxiliary_offset != UINT32_MAX;
    const bool need_flags = ctx.operation.flag_outputs || need_aux;
    const u64 req = need_aux ? FlagBits(Flag::All) : ctx.operation.flag_outputs;
    const bool need_operand_flags =
            need_flags && (req & (FlagBits(Flag::Carry) | FlagBits(Flag::Overflow) |
                                  FlagBits(Flag::AuxiliaryCarry))) != 0;

    auto at = ctx.operands.begin();
    const auto end = ctx.operands.end();

    if (!with_carry && !need_operand_flags) {
        if (op == OpCode::Neg) {
            const GPR rhs = ReadExprRef(at, end, a1, ctx.width);
            as.NEG(ctx.destination, rhs);
        } else {
            const GPR lhs = ReadRef(*at++, a0, true);
            std::int32_t simm = 0;
            if (at != end && TrySignedImm12(*this, *at, ctx.width, simm) &&
                (!subtract || (simm != -2048))) {
                ++at;
                const std::int32_t add_imm = subtract ? -simm : simm;
                Address(ctx.destination, lhs, add_imm);
            } else {
                const GPR rhs = ReadExprRef(at, end, a1, ctx.width);
                if (subtract)
                    as.SUB(ctx.destination, lhs, rhs);
                else
                    as.ADD(ctx.destination, lhs, rhs);
            }
        }
        if (need_flags) {
            EmitArithmeticFlags(ctx, x0, x0, ctx.destination, x0, subtract);
        } else {
            Mask(ctx.destination, ctx.width);
        }
        return;
    }

    GPR lhs = op == OpCode::Neg ? x0 : ReadWidth(*at++, a0, ctx.width);
    GPR rhs = ReadExprRef(at, end, a1, ctx.width);

    if (with_carry) {
        if (options.zbs) {
            as.BEXTI(a2, s11, kCarryBit);
        } else {
            as.SRLI(a2, s11, kCarryBit);
            as.ANDI(a2, a2, 1);
        }
        if (!need_flags) {
            if (subtract) {
                as.SUB(t0, lhs, rhs);
                as.SUB(ctx.destination, t0, a2);
            } else {
                as.ADD(t0, lhs, rhs);
                as.ADD(ctx.destination, t0, a2);
            }
            Mask(ctx.destination, ctx.width);
            return;
        }
    }

    if (lhs == ctx.destination) {
        as.MV(a0, lhs);
        lhs = a0;
    }
    if (rhs == ctx.destination) {
        as.MV(a1, rhs);
        rhs = a1;
    }

    if (with_carry) {
        if (subtract) {
            as.SUB(t0, lhs, rhs);
            as.SUB(a3, t0, a2);
            if (req & FlagBits(Flag::Carry)) {
                as.SLTU(t1, lhs, rhs);
                as.SLTU(t2, t0, a2);
                as.OR(t1, t1, t2);
            }
            EmitArithmeticFlags(ctx, lhs, rhs, a3, t1, true);
        } else {
            as.ADD(t0, lhs, rhs);
            as.ADD(a3, t0, a2);
            GPR carry = x0;
            if ((req & FlagBits(Flag::Carry)) && ctx.width == 64) {
                as.SLTU(t1, t0, lhs);
                as.SLTU(t2, a3, t0);
                as.OR(t1, t1, t2);
                carry = t1;
            }
            EmitArithmeticFlags(ctx, lhs, rhs, a3, carry, false);
        }
        return;
    }

    if (subtract) {
        if (op == OpCode::Neg)
            as.NEG(t0, rhs);
        else
            as.SUB(t0, lhs, rhs);
        if (req & FlagBits(Flag::Carry)) {
            if (op == OpCode::Neg)
                as.SNEZ(t1, rhs);
            else
                as.SLTU(t1, lhs, rhs);
        }
        EmitArithmeticFlags(ctx, lhs, rhs, t0, t1, true);
    } else {
        as.ADD(t0, lhs, rhs);
        GPR carry = x0;
        if ((req & FlagBits(Flag::Carry)) && ctx.width == 64) {
            as.SLTU(t1, t0, lhs);
            carry = t1;
        }
        EmitArithmeticFlags(ctx, lhs, rhs, t0, carry, false);
    }
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
    const auto op = ctx.operation.opcode;
    auto at = ctx.operands.begin();
    const auto end = ctx.operands.end();
    const GPR lhs = ReadWidth(*at++, a0, ctx.width);
    const bool unary = at == end;
    std::int32_t simm = 0;
    bool masked_already = false;

    if (!unary && (op == OpCode::And || op == OpCode::Or || op == OpCode::Xor) &&
        TrySignedImm12(*this, *at, ctx.width, simm)) {
        ++at;
        const auto imm_bits = static_cast<u32>(simm);
        if (op == OpCode::And) {
            as.ANDI(ctx.destination, lhs, imm_bits);
            masked_already = true;
        } else if (op == OpCode::Or) {
            as.ORI(ctx.destination, lhs, imm_bits);
            masked_already = simm >= 0;
        } else {
            as.XORI(ctx.destination, lhs, imm_bits);
            masked_already = simm >= 0;
        }
    } else {
        const GPR rhs = unary ? x0 : ReadExprRef(at, end, a1, ctx.width);
        switch (op) {
            case OpCode::And:
                as.AND(ctx.destination, lhs, rhs);
                masked_already = true;
                break;
            case OpCode::Or:
                as.OR(ctx.destination, lhs, rhs);
                masked_already = true;
                break;
            case OpCode::Xor:
                as.XOR(ctx.destination, lhs, rhs);
                masked_already = true;
                break;
            case OpCode::AndNot:
                if (options.zbb) {
                    as.ANDN(ctx.destination, lhs, rhs);
                    masked_already = true;
                } else {
                    as.NOT(a1, rhs);
                    as.AND(ctx.destination, lhs, a1);
                    masked_already = true;
                }
                break;
            case OpCode::Not:
                if (unary) {
                    as.SEQZ(ctx.destination, lhs);
                    masked_already = true;
                } else {
                    as.NOT(ctx.destination, rhs);
                }
                break;
            default:
                Reject(CompileError::UnsupportedOpcode);
        }
    }
    if (!masked_already) Mask(ctx.destination, ctx.width);
    if (ctx.operation.flag_outputs ||
        allocation.homes[ctx.node.value].auxiliary_offset != UINT32_MAX) {
        EmitLogicalFlags(ctx);
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
    const GPR lhs = ReadRef(ctx.Arg(0), a0, true);
    const GPR rhs = ReadRef(ctx.Arg(1), a1, true);
    as.MUL(ctx.destination, lhs, rhs);
    Mask(ctx.destination, ctx.width);
}

void Writer::Handle(Op<OpCode::MulSub>, OpContext& ctx) {
    const GPR lhs = ReadRef(ctx.Arg(0), a0, true);
    const GPR rhs = ReadRef(ctx.Arg(1), a1, true);
    const GPR acc = ReadRef(ctx.Arg(2), a2, true);
    as.MUL(t0, lhs, rhs);
    as.SUB(ctx.destination, acc, t0);
    Mask(ctx.destination, ctx.width);
}

void Writer::Handle(Op<OpCode::MulHigh>, OpContext& ctx) {
    const auto bits = TypeBits(View(ctx.Arg(0)));
    const bool is_signed = ctx.Arg(2).Payload() != 0;
    GPR lhs = a0;
    GPR rhs = a1;
    if (is_signed && bits < 64) {
        Read(ctx.Arg(0), a0, true);
        Read(ctx.Arg(1), a1, true);
        SignExtend(a0, bits);
        SignExtend(a1, bits);
    } else {
        lhs = ReadWidth(ctx.Arg(0), a0, bits);
        rhs = ReadWidth(ctx.Arg(1), a1, bits);
    }
    if (bits == 64) {
        if (is_signed)
            as.MULH(ctx.destination, lhs, rhs);
        else
            as.MULHU(ctx.destination, lhs, rhs);
    } else {
        as.MUL(ctx.destination, lhs, rhs);
        as.SRLI(ctx.destination, ctx.destination, bits);
    }
    Mask(ctx.destination, ctx.width);
}

void Writer::Handle(Op<OpCode::Div>, OpContext& ctx) {
    const GPR lhs = ReadWidth(ctx.Arg(0), a0, ctx.width);
    auto at = ctx.operands.begin();
    ++at;
    const GPR rhs = ReadExprRef(at, ctx.operands.end(), a1, ctx.width);
    Label nonzero;
    as.BNE(rhs, x0, &nonzero);
    Exit(ExitValue(NativeExit::DivideFault));
    as.Bind(&nonzero);
    as.DIVU(ctx.destination, lhs, rhs);
}

void Writer::Handle(Op<OpCode::SignedDiv64>, OpContext& ctx) {
    const GPR lhs = ReadRef(ctx.Arg(0), a0);
    const GPR rhs = ReadRef(ctx.Arg(1), a1);
    Label nonzero, valid;
    as.BNE(rhs, x0, &nonzero);
    Exit(ExitValue(NativeExit::DivideFault));
    as.Bind(&nonzero);
    as.LI(t0, u64{1} << 63);
    as.BNE(lhs, t0, &valid);
    as.LI(t0, UINT64_MAX);
    as.BNE(rhs, t0, &valid);
    Exit(ExitValue(NativeExit::DivideFault));
    as.Bind(&valid);
    as.DIV(ctx.destination, lhs, rhs);
}

void Writer::Shift(OpContext& ctx) {
    const auto op = ctx.operation.opcode;
    const bool left = op == OpCode::LslImm || op == OpCode::LslValue;
    const bool right = op == OpCode::LsrImm || op == OpCode::LsrValue;
    const bool arithmetic = op == OpCode::AsrImm || op == OpCode::AsrValue;
    const unsigned src_bits = std::min(ctx.width, TypeBits(View(ctx.Arg(0))));

    if (ctx.Arg(1).Kind() == OperandKind::Immediate) {
        const unsigned amt = static_cast<unsigned>(ctx.Arg(1).Payload()) & (ctx.width - 1);
        if (left) {
            const GPR src = ReadWidth(ctx.Arg(0), ctx.destination, ctx.width);
            if (amt == 0) {
                if (src != ctx.destination) as.MV(ctx.destination, src);
                return;
            }
            as.SLLI(ctx.destination, src, amt);
            Mask(ctx.destination, ctx.width);
            return;
        }
        if (right) {
            const GPR src = ReadWidth(ctx.Arg(0), ctx.destination, ctx.width);
            if (amt == 0) {
                if (src != ctx.destination) as.MV(ctx.destination, src);
            } else {
                as.SRLI(ctx.destination, src, amt);
            }
            return;
        }
        if (arithmetic) {
            if (ctx.width == 64 && src_bits == 64) {
                const GPR src = ReadRef(ctx.Arg(0), ctx.destination);
                if (amt == 0) {
                    if (src != ctx.destination) as.MV(ctx.destination, src);
                } else {
                    as.SRAI(ctx.destination, src, amt);
                }
                return;
            }
            if (ctx.width == 32 && src_bits == 32 && amt != 0) {
                const GPR src = ReadRef(ctx.Arg(0), ctx.destination, true);
                as.SRAIW(ctx.destination, src, amt);
                Mask(ctx.destination, 32);
                return;
            }
            Read(ctx.Arg(0), ctx.destination, true);
            SignExtend(ctx.destination, src_bits);
            if (amt != 0) as.SRAI(ctx.destination, ctx.destination, amt);
            Mask(ctx.destination, ctx.width);
            return;
        }
        const GPR src = ReadWidth(ctx.Arg(0), a0, ctx.width);
        if (amt == 0) {
            if (src != ctx.destination) as.MV(ctx.destination, src);
            return;
        }
        if (options.zbb && ctx.width == 64) {
            as.RORI(ctx.destination, src, amt);
            return;
        }
        if (options.zbb && ctx.width == 32) {
            as.RORIW(ctx.destination, src, amt);
            Mask(ctx.destination, 32);
            return;
        }
        as.SRLI(t0, src, amt);
        as.SLLI(ctx.destination, src, ctx.width - amt);
        as.OR(ctx.destination, ctx.destination, t0);
        Mask(ctx.destination, ctx.width);
        return;
    }

    const GPR amt_raw = ReadRef(ctx.Arg(1), a1, true);
    if (left) {
        const GPR src = ReadWidth(ctx.Arg(0), a0, ctx.width);
        if (ctx.width == 64) {
            as.SLL(ctx.destination, src, amt_raw);
            return;
        }
        if (ctx.width == 32) {
            as.SLLW(ctx.destination, src, amt_raw);
            Mask(ctx.destination, 32);
            return;
        }
        as.ANDI(a1, amt_raw, ctx.width - 1);
        as.SLL(ctx.destination, src, a1);
        Mask(ctx.destination, ctx.width);
        return;
    }
    if (right) {
        const GPR src = ReadWidth(ctx.Arg(0), a0, ctx.width);
        if (ctx.width == 64) {
            as.SRL(ctx.destination, src, amt_raw);
            return;
        }
        as.ANDI(a1, amt_raw, ctx.width - 1);
        as.SRL(ctx.destination, src, a1);
        return;
    }
    if (arithmetic) {
        if (ctx.width == 64 && src_bits == 64) {
            const GPR src = ReadRef(ctx.Arg(0), a0);
            as.SRA(ctx.destination, src, amt_raw);
            return;
        }
        if (ctx.width == 32 && src_bits == 32) {
            const GPR src = ReadRef(ctx.Arg(0), a0, true);
            as.SRAW(ctx.destination, src, amt_raw);
            Mask(ctx.destination, 32);
            return;
        }
        Read(ctx.Arg(0), a0, true);
        SignExtend(a0, src_bits);
        if (ctx.width < 64) {
            as.ANDI(a1, amt_raw, ctx.width - 1);
            as.SRA(ctx.destination, a0, a1);
        } else {
            as.SRA(ctx.destination, a0, amt_raw);
        }
        Mask(ctx.destination, ctx.width);
        return;
    }

    const GPR src = ReadWidth(ctx.Arg(0), a0, ctx.width);
    if (options.zbb && ctx.width == 64) {
        as.ROR(ctx.destination, src, amt_raw);
        return;
    }
    if (options.zbb && ctx.width == 32) {
        as.RORW(ctx.destination, src, amt_raw);
        Mask(ctx.destination, 32);
        return;
    }
    as.ANDI(a1, amt_raw, ctx.width - 1);
    as.NEG(t0, a1);
    as.ANDI(t0, t0, ctx.width - 1);
    as.SRL(t1, src, a1);
    as.SLL(ctx.destination, src, t0);
    as.OR(ctx.destination, ctx.destination, t1);
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
    if (ctx.operation.opcode == OpCode::TestBit) {
        const auto bit = ctx.Arg(1).Payload();
        if (bit >= TypeBits(View(ctx.Arg(0)))) {
            as.MV(ctx.destination, x0);
            return;
        }
        const GPR src = ReadRef(ctx.Arg(0), ctx.destination, true);
        if (options.zbs) {
            as.BEXTI(ctx.destination, src, static_cast<u32>(bit));
        } else {
            as.SRLI(ctx.destination, src, static_cast<u32>(bit));
            as.ANDI(ctx.destination, ctx.destination, 1);
        }
        return;
    }
    const GPR src = ReadRef(ctx.Arg(0), ctx.destination);
    if (ctx.operation.opcode == OpCode::TestZero)
        as.SEQZ(ctx.destination, src);
    else
        as.SNEZ(ctx.destination, src);
}

#define RV_PRED(name)                                                                              \
    void Writer::Handle(Op<OpCode::name>, OpContext& ctx) { Predicate(ctx); }
RV_PRED(TestBit)
RV_PRED(TestZero)
RV_PRED(TestNotZero)
#undef RV_PRED

void Writer::BitField(OpContext& ctx) {
    const GPR src = ReadRef(ctx.Arg(0), a0, true);
    const bool insert = ctx.operation.opcode == OpCode::BitInsert;
    const auto lsb = ctx.Arg(insert ? 2 : 1).Payload();
    const auto length = ctx.Arg(insert ? 3 : 2).Payload();
    if (lsb >= 64 || length > 64 - lsb) Reject(CompileError::UnsupportedOperand);
    const u64 field = !length ? 0 : length == 64 ? UINT64_MAX : ((u64{1} << length) - 1) << lsb;
    if (ctx.operation.opcode == OpCode::BitExtract) {
        if (!length) {
            as.MV(ctx.destination, x0);
        } else {
            if (lsb)
                as.SRLI(ctx.destination, src, static_cast<u32>(lsb));
            else if (src != ctx.destination)
                as.MV(ctx.destination, src);
            Mask(ctx.destination, static_cast<unsigned>(length));
        }
    } else if (!insert && length == 1 && options.zbs) {
        as.BCLRI(ctx.destination, src, static_cast<u32>(lsb));
    } else {
        if (options.zbb) {
            as.LI(t0, field);
            as.ANDN(ctx.destination, src, t0);
        } else {
            as.LI(t0, ~field);
            as.AND(ctx.destination, src, t0);
        }
        if (insert && length) {
            Read(ctx.Arg(1), a1, true);
            if (lsb) as.SLLI(a1, a1, static_cast<u32>(lsb));
            if (options.zbb) {
                as.AND(a1, a1, t0);
            } else {
                as.LI(t0, field);
                as.AND(a1, a1, t0);
            }
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
    const auto bits = ctx.Arg(1).Payload();
    if (bits != 16 && bits != 32 && bits != 64) Reject(CompileError::UnsupportedOperand);
    const GPR src = ReadRef(ctx.Arg(0), a0, true);
    if (options.zbb) {
        as.REV8(ctx.destination, src);
        if (bits < 64) as.SRLI(ctx.destination, ctx.destination, static_cast<u32>(64 - bits));
        if (ctx.width < bits) Mask(ctx.destination, ctx.width);
        return;
    }
    if (bits == 16) {
        as.ANDI(t0, src, 0xff);
        as.SLLI(t0, t0, 8);
        as.SRLI(t1, src, 8);
        as.ANDI(t1, t1, 0xff);
        as.OR(ctx.destination, t0, t1);
    } else if (bits == 32) {
        as.LI(t1, 0x00ff00ffULL);
        as.SRLI(t0, src, 8);
        as.AND(t0, t0, t1);
        as.AND(t2, src, t1);
        as.SLLI(t2, t2, 8);
        as.OR(t0, t0, t2);
        as.SRLI(t1, t0, 16);
        as.SLLI(t0, t0, 48);
        as.SRLI(t0, t0, 32);
        as.OR(ctx.destination, t0, t1);
    } else {
        as.LI(t1, 0x00ff00ff00ff00ffULL);
        as.SRLI(t0, src, 8);
        as.AND(t0, t0, t1);
        as.AND(t2, src, t1);
        as.SLLI(t2, t2, 8);
        as.OR(t0, t0, t2);
        as.LI(t1, 0x0000ffff0000ffffULL);
        as.SRLI(t2, t0, 16);
        as.AND(t2, t2, t1);
        as.AND(t0, t0, t1);
        as.SLLI(t0, t0, 16);
        as.OR(t0, t0, t2);
        as.SRLI(t1, t0, 32);
        as.SLLI(t0, t0, 32);
        as.OR(ctx.destination, t0, t1);
    }
    if (ctx.width < bits) Mask(ctx.destination, ctx.width);
}

void Writer::CountBits(OpContext& ctx) {
    const auto op = ctx.operation.opcode;
    const bool clz = op == OpCode::CountLeadingZeros32 || op == OpCode::CountLeadingZeros64;
    const unsigned bits =
            (op == OpCode::CountLeadingZeros32 || op == OpCode::CountTrailingZeros32) ? 32 : 64;
    if (options.zbb) {
        const GPR src = ReadRef(ctx.Arg(0), a0, true);
        if (clz && bits == 32)
            as.CLZW(ctx.destination, src);
        else if (clz)
            as.CLZ(ctx.destination, src);
        else if (bits == 32)
            as.CTZW(ctx.destination, src);
        else
            as.CTZ(ctx.destination, src);
        return;
    }
    if (clz) {
        if (bits == 32) {
            Read(ctx.Arg(0), a0, true);
            Mask(a0, 32);
            EmitBranchlessClz64(as, ctx.destination, a0);
            as.ADDI(ctx.destination, ctx.destination, -32);
        } else {
            Read(ctx.Arg(0), a0, true);
            EmitBranchlessClz64(as, ctx.destination, a0);
        }
        return;
    }
    Read(ctx.Arg(0), a0, true);
    if (bits == 32) {
        if (options.zbs) {
            as.BSETI(a0, a0, 32);
        } else {
            as.LI(t0, u64{1} << 32);
            as.OR(a0, a0, t0);
        }
    }
    as.ADDI(t0, a0, -1);
    as.NOT(a0, a0);
    as.AND(a0, a0, t0);
    EmitBranchlessClz64(as, ctx.destination, a0);
    as.LI(t0, 64);
    as.SUB(ctx.destination, t0, ctx.destination);
}

#define RV_COUNT(name)                                                                             \
    void Writer::Handle(Op<OpCode::name>, OpContext& ctx) { CountBits(ctx); }
RV_COUNT(CountLeadingZeros32)
RV_COUNT(CountLeadingZeros64)
RV_COUNT(CountTrailingZeros32)
RV_COUNT(CountTrailingZeros64)
#undef RV_COUNT

void Writer::Handle(Op<OpCode::PopCount>, OpContext& ctx) {
    if (options.zbb) {
        const GPR src = ReadRef(ctx.Arg(0), a0, true);
        as.CPOP(ctx.destination, src);
        Mask(ctx.destination, ctx.width);
        return;
    }
    Read(ctx.Arg(0), a0, true);
    as.LI(t1, 0x5555555555555555ULL);
    as.SRLI(t0, a0, 1);
    as.AND(t0, t0, t1);
    as.SUB(a0, a0, t0);
    as.LI(t1, 0x3333333333333333ULL);
    as.SRLI(t0, a0, 2);
    as.AND(t0, t0, t1);
    as.AND(a0, a0, t1);
    as.ADD(a0, a0, t0);
    as.SRLI(t0, a0, 4);
    as.ADD(a0, a0, t0);
    as.LI(t1, 0x0f0f0f0f0f0f0f0fULL);
    as.AND(a0, a0, t1);
    as.LI(t1, 0x0101010101010101ULL);
    as.MUL(a0, a0, t1);
    as.SRLI(ctx.destination, a0, 56);
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
    const auto bits = ctx.Arg(2).Payload();
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64)
        Reject(CompileError::UnsupportedOperand);
    const GPR crc = ReadWidth(ctx.Arg(0), a0, 32);
    const GPR val = ReadWidth(ctx.Arg(1), a1, static_cast<unsigned>(bits));

    if (options.zbc || options.zbkc) {
        as.LI(t3, kCrc32cPolyStar);
        if (bits == 64) {
            as.LI(t2, kCrc32cMu32);
            as.XOR(t0, crc, val);
            Mask(t0, 32);
            as.CLMUL(t0, t0, t2);
            Mask(t0, 32);
            as.CLMUL(t0, t0, t3);
            as.SRLI(t0, t0, 32);
            as.SRLI(t1, val, 32);
            as.XOR(t0, t0, t1);
            as.CLMUL(t0, t0, t2);
            Mask(t0, 32);
            as.CLMUL(ctx.destination, t0, t3);
            as.SRLI(ctx.destination, ctx.destination, 32);
        } else {
            const u64 mu = bits == 8 ? kCrc32cMu8 : (bits == 16 ? kCrc32cMu16 : kCrc32cMu32);
            as.LI(t2, mu);
            as.XOR(t0, crc, val);
            Mask(t0, static_cast<unsigned>(bits));
            as.CLMUL(t0, t0, t2);
            Mask(t0, static_cast<unsigned>(bits));
            as.CLMUL(t0, t0, t3);
            if (bits == 32) {
                as.SRLI(ctx.destination, t0, 32);
            } else {
                as.SRLI(t0, t0, static_cast<u32>(bits));
                as.SRLI(t1, crc, static_cast<u32>(bits));
                as.XOR(ctx.destination, t0, t1);
            }
        }
        if (ctx.width < 32) Mask(ctx.destination, ctx.width);
        return;
    }

    as.XOR(a0, crc, val);
    as.LI(t1, kCrc32cPolyReflected);
    as.LI(t2, bits);
    Label loop;
    as.Bind(&loop);
    as.ANDI(t0, a0, 1);
    as.NEG(t0, t0);
    as.AND(t0, t0, t1);
    as.SRLI(a0, a0, 1);
    as.XOR(a0, a0, t0);
    as.ADDI(t2, t2, -1);
    as.BNE(t2, x0, &loop);
    if (ctx.destination != a0) as.MV(ctx.destination, a0);
    if (ctx.width < 32) Mask(ctx.destination, ctx.width);
}
}  // namespace swift::runtime::backend::riscv64::detail
