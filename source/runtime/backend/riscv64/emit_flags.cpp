#include "emit.h"

#include <bit>
#include "runtime/arch/x86/flags.h"

namespace swift::runtime::backend::riscv64::detail {
namespace flags = ::swift::runtime::x86::flags;

namespace {
constexpr unsigned kCarryBit = 29;
constexpr unsigned kOverflowBit = 28;
constexpr unsigned kZeroBit = 30;
constexpr unsigned kSignBit = 31;
constexpr unsigned kAuxBitShift = 22;  // bit 4 << 22 == bit 26 (flags::Auxiliary)
}  // namespace

void Writer::Handle(Op<OpCode::ReadFPStatus>, OpContext& ctx) {
    Read(ctx.Arg(0), t2);
    as.FRFLAGS(t0);
    const auto destination = ctx.destination;
    as.SRLI(destination, t0, 4);
    as.ANDI(destination, destination, 1);
    as.ANDI(t1, t0, 8);
    as.SRLI(t1, t1, 1);
    as.OR(destination, destination, t1);
    as.ANDI(t1, t0, 4);
    as.SLLI(t1, t1, 1);
    as.OR(destination, destination, t1);
    as.ANDI(t1, t0, 2);
    as.SLLI(t1, t1, 3);
    as.OR(destination, destination, t1);
    as.ANDI(t1, t0, 1);
    as.SLLI(t1, t1, 5);
    as.OR(destination, destination, t1);
    as.ANDI(t2, t2, 63);
    as.OR(destination, destination, t2);
}

u64 PackedFlagMask(u64 selected) {
    return ((selected & FlagBits(Flag::Carry)) ? flags::Carry : 0) |
           ((selected & FlagBits(Flag::Overflow)) ? flags::Overflow : 0) |
           ((selected & FlagBits(Flag::Zero)) ? flags::Zero : 0) |
           ((selected & FlagBits(Flag::Negate)) ? flags::Sign : 0) |
           ((selected & FlagBits(Flag::Parity)) ? 255 : 0) |
           ((selected & FlagBits(Flag::AuxiliaryCarry)) ? flags::Auxiliary : 0) |
           ((selected & FlagBits(Flag::Direction)) ? flags::Direction : 0);
}

void Writer::MergeFlags(GPR value, u64 selected, bool clean_input) {
    const auto mask = PackedFlagMask(selected);
    if (!mask) return;
    if (options.zbs && std::has_single_bit(mask)) {
        const auto bit = static_cast<u32>(std::countr_zero(mask));
        if (clean_input) {
            as.BCLRI(s11, s11, bit);
            if (value != x0) as.OR(s11, s11, value);
            return;
        }
        if (value == x0) {
            as.BCLRI(s11, s11, bit);
            return;
        }
        as.BEXTI(t5, value, bit);
        as.SLLI(t5, t5, bit);
        as.BCLRI(s11, s11, bit);
        as.OR(s11, s11, t5);
        return;
    }
    if (value == x0) {
        if (options.zbb) {
            as.LI(t6, mask);
            as.ANDN(s11, s11, t6);
        } else {
            as.LI(t6, ~mask);
            as.AND(s11, s11, t6);
        }
        return;
    }
    if (clean_input) {
        if (options.zbb) {
            as.LI(t6, mask);
            as.ANDN(s11, s11, t6);
        } else {
            as.LI(t6, ~mask);
            as.AND(s11, s11, t6);
        }
        as.OR(s11, s11, value);
        return;
    }
    as.LI(t6, mask);
    as.AND(t5, value, t6);
    if (options.zbb) {
        as.ANDN(s11, s11, t6);
    } else {
        as.NOT(t6, t6);
        as.AND(s11, s11, t6);
    }
    as.OR(s11, s11, t5);
}

void Writer::SaveProducerFlags(OpContext& ctx, GPR value, bool clean_input) {
    const auto& home = allocation.homes[ctx.node.value];
    if (home.auxiliary_offset != UINT32_MAX) FrameStore(value, home.auxiliary_offset);
    if (ctx.operation.Metadata().profile == FlagProfile::ArithmeticRaw &&
        (ctx.operation.opcode == OpCode::Sub || ctx.operation.opcode == OpCode::Sbb ||
         ctx.operation.opcode == OpCode::Neg) &&
        (ctx.operation.flag_outputs & FlagBits(Flag::Carry))) {
        if (options.zbs) {
            as.BINVI(value, value, kCarryBit);
        } else {
            as.LI(t6, flags::Carry);
            as.XOR(value, value, t6);
        }
    }
    MergeFlags(value, ctx.operation.flag_outputs, clean_input);
}

void Writer::EmitArithmeticFlags(OpContext& ctx,
                                 GPR lhs,
                                 GPR rhs,
                                 GPR raw_result,
                                 GPR carry,
                                 bool subtract) {
    const bool need_aux = allocation.homes[ctx.node.value].auxiliary_offset != UINT32_MAX;
    const u64 req = need_aux ? FlagBits(Flag::All) : ctx.operation.flag_outputs;
    bool initialized = false;
    const auto accumulate = [&](GPR term) {
        if (!initialized) {
            if (term != t3) as.MV(t3, term);
            initialized = true;
        } else {
            as.OR(t3, t3, term);
        }
    };

    if (req & FlagBits(Flag::Carry)) {
        if (carry == x0 && !subtract && ctx.width < 64) {
            as.SRLI(t1, raw_result, ctx.width);
            as.ANDI(t1, t1, 1);
            carry = t1;
        }
        as.SLLI(carry, carry, kCarryBit);
        accumulate(carry);
    }

    if (raw_result != ctx.destination) as.MV(ctx.destination, raw_result);
    Mask(ctx.destination, ctx.width);

    if (req & FlagBits(Flag::Parity)) {
        as.ANDI(t2, ctx.destination, 255);
        accumulate(t2);
    }
    if (req & FlagBits(Flag::Zero)) {
        as.SEQZ(t2, ctx.destination);
        as.SLLI(t2, t2, kZeroBit);
        accumulate(t2);
    }
    if (req & FlagBits(Flag::Negate)) {
        if (options.zbs) {
            as.BEXTI(t2, ctx.destination, ctx.width - 1);
        } else {
            as.SRLI(t2, ctx.destination, ctx.width - 1);
            if (ctx.width < 64) as.ANDI(t2, t2, 1);
        }
        as.SLLI(t2, t2, kSignBit);
        accumulate(t2);
    }
    if (req & FlagBits(Flag::Overflow)) {
        as.XOR(t0, lhs, ctx.destination);
        if (subtract) {
            as.XOR(t2, lhs, rhs);
            as.AND(t2, t2, t0);
        } else if (options.zbb) {
            as.XOR(t2, lhs, rhs);
            as.ANDN(t2, t0, t2);
        } else {
            as.XOR(t2, lhs, rhs);
            as.NOT(t2, t2);
            as.AND(t2, t2, t0);
        }
        if (options.zbs) {
            as.BEXTI(t2, t2, ctx.width - 1);
        } else {
            as.SRLI(t2, t2, ctx.width - 1);
            if (ctx.width < 64) as.ANDI(t2, t2, 1);
        }
        as.SLLI(t2, t2, kOverflowBit);
        accumulate(t2);
    }
    if (req & FlagBits(Flag::AuxiliaryCarry)) {
        as.XOR(t0, lhs, rhs);
        as.XOR(t0, t0, ctx.destination);
        as.ANDI(t0, t0, 16);
        as.SLLI(t0, t0, kAuxBitShift);
        accumulate(t0);
    }

    if (!initialized) as.MV(t3, x0);
    SaveProducerFlags(ctx, t3, !need_aux);
}

void Writer::EmitLogicalFlags(OpContext& ctx) {
    const bool need_aux = allocation.homes[ctx.node.value].auxiliary_offset != UINT32_MAX;
    const u64 req = need_aux ? FlagBits(Flag::All) : ctx.operation.flag_outputs;
    bool initialized = false;
    const auto accumulate = [&](GPR term) {
        if (!initialized) {
            if (term != t3) as.MV(t3, term);
            initialized = true;
        } else {
            as.OR(t3, t3, term);
        }
    };

    if (req & FlagBits(Flag::Parity)) {
        as.ANDI(t3, ctx.destination, 255);
        initialized = true;
    }
    if (req & FlagBits(Flag::Zero)) {
        as.SEQZ(t0, ctx.destination);
        as.SLLI(t0, t0, kZeroBit);
        accumulate(t0);
    }
    if (req & FlagBits(Flag::Negate)) {
        if (options.zbs) {
            as.BEXTI(t0, ctx.destination, ctx.width - 1);
        } else {
            as.SRLI(t0, ctx.destination, ctx.width - 1);
        }
        as.SLLI(t0, t0, kSignBit);
        accumulate(t0);
    }
    SaveProducerFlags(ctx, initialized ? t3 : x0, !need_aux);
}

void Writer::Condition(GPR destination, ir::Condition condition) {
    const auto bit = [&](GPR reg, unsigned shift) {
        if (options.zbs) {
            as.BEXTI(reg, s11, shift);
        } else {
            as.SRLI(reg, s11, shift);
            as.ANDI(reg, reg, 1);
        }
    };
    const auto zero = [&] { bit(destination, kZeroBit); };
    const auto carry = [&] { bit(destination, kCarryBit); };
    const auto signed_less = [&] {
        bit(destination, kSignBit);
        bit(t4, kOverflowBit);
        as.XOR(destination, destination, t4);
    };
    switch (condition) {
        case ir::Condition::EQ:
            zero();
            return;
        case ir::Condition::NE:
            zero();
            as.XORI(destination, destination, 1);
            return;
        case ir::Condition::CS:
            carry();
            return;
        case ir::Condition::CC:
            carry();
            as.XORI(destination, destination, 1);
            return;
        case ir::Condition::MI:
            bit(destination, kSignBit);
            return;
        case ir::Condition::PL:
            bit(destination, kSignBit);
            as.XORI(destination, destination, 1);
            return;
        case ir::Condition::VS:
            bit(destination, kOverflowBit);
            return;
        case ir::Condition::VC:
            bit(destination, kOverflowBit);
            as.XORI(destination, destination, 1);
            return;
        case ir::Condition::HI:
            carry();
            bit(t4, kZeroBit);
            if (options.zbb) {
                as.ANDN(destination, destination, t4);
            } else {
                as.XORI(t4, t4, 1);
                as.AND(destination, destination, t4);
            }
            return;
        case ir::Condition::LS:
            carry();
            as.XORI(destination, destination, 1);
            bit(t4, kZeroBit);
            as.OR(destination, destination, t4);
            return;
        case ir::Condition::GE:
            signed_less();
            as.XORI(destination, destination, 1);
            return;
        case ir::Condition::LT:
            signed_less();
            return;
        case ir::Condition::GT:
            signed_less();
            bit(t4, kZeroBit);
            as.OR(destination, destination, t4);
            as.XORI(destination, destination, 1);
            return;
        case ir::Condition::LE:
            signed_less();
            bit(t4, kZeroBit);
            as.OR(destination, destination, t4);
            return;
        case ir::Condition::AL:
        case ir::Condition::NV:
            as.LI(destination, 1);
            return;
    }
}

void Writer::Handle(Op<OpCode::CondSet>, OpContext& ctx) {
    Condition(ctx.destination, static_cast<ir::Condition>(ctx.Arg(0).Payload()));
}

void Writer::Handle(Op<OpCode::LocalCondSet>, OpContext& ctx) {
    Handle(Op<OpCode::CondSet>{}, ctx);
}

void Writer::Handle(Op<OpCode::FCmpCondSet>, OpContext& ctx) {
    const GPR src = ReadRef(ctx.Arg(0), a0);
    const auto condition = static_cast<ir::Condition>(ctx.Arg(1).Payload());
    u64 table{};
    for (unsigned packed = 0; packed != 8; ++packed) {
        const bool n = packed == 1, z = packed == 4, c = !n, v = packed == 7;
        bool take{};
        switch (condition) {
            case ir::Condition::EQ:
                take = z;
                break;
            case ir::Condition::NE:
                take = !z;
                break;
            case ir::Condition::CS:
                take = c;
                break;
            case ir::Condition::CC:
                take = !c;
                break;
            case ir::Condition::MI:
                take = n;
                break;
            case ir::Condition::PL:
                take = !n;
                break;
            case ir::Condition::VS:
                take = v;
                break;
            case ir::Condition::VC:
                take = !v;
                break;
            case ir::Condition::HI:
                take = c && !z;
                break;
            case ir::Condition::LS:
                take = !c || z;
                break;
            case ir::Condition::GE:
                take = n == v;
                break;
            case ir::Condition::LT:
                take = n != v;
                break;
            case ir::Condition::GT:
                take = !z && n == v;
                break;
            case ir::Condition::LE:
                take = z || n != v;
                break;
            default:
                take = true;
                break;
        }
        table |= u64{take} << packed;
    }
    as.LI(t0, table);
    as.SRL(ctx.destination, t0, src);
    as.ANDI(ctx.destination, ctx.destination, 1);
}

void Writer::Handle(Op<OpCode::LocalParitySet>, OpContext& ctx) {
    Read(ctx.Arg(0), ctx.destination, true);
    as.SRLI(t0, ctx.destination, 4);
    as.XOR(ctx.destination, ctx.destination, t0);
    as.SRLI(t0, ctx.destination, 2);
    as.XOR(ctx.destination, ctx.destination, t0);
    as.SRLI(t0, ctx.destination, 1);
    as.XOR(ctx.destination, ctx.destination, t0);
    as.XORI(ctx.destination, ctx.destination, ctx.Arg(1).Payload() ? 0 : 1);
    as.ANDI(ctx.destination, ctx.destination, 1);
}

void Writer::Handle(Op<OpCode::GetFlags>, OpContext& ctx) { as.MV(ctx.destination, s11); }

void Writer::Handle(Op<OpCode::RestoreFlags>, OpContext& ctx) {
    const GPR raw = ReadRef(ctx.Arg(0), a0, true);
    MergeFlags(raw, ctx.Arg(1).Payload(), false);
}

void Writer::Handle(Op<OpCode::SaveFlags>, OpContext& ctx) {
    if (!FlagProducer(source.Operations()[ctx.Arg(0).Payload()].opcode))
        Reject(CompileError::UnsupportedFlags);
    const auto offset = allocation.homes[ctx.Arg(0).Payload()].auxiliary_offset;
    if (offset == UINT32_MAX) Reject(CompileError::UnsupportedFlags);
    FrameLoad(a0, offset);
    MergeFlags(a0, ctx.Arg(1).Payload(), false);
}

void Writer::Handle(Op<OpCode::BranchOnlyFlags>, OpContext& ctx) {
    Handle(Op<OpCode::SaveFlags>{}, ctx);
}

void Writer::Handle(Op<OpCode::ClearFlags>, OpContext& ctx) {
    const auto selected = ctx.Arg(0).Payload();
    if (selected & FlagBits(Flag::Parity)) {
        as.LI(a0, 1);
        MergeFlags(a0, selected, true);
    } else {
        MergeFlags(x0, selected, true);
    }
}

void Writer::Handle(Op<OpCode::InvertCarry>, OpContext&) {
    if (options.zbs) {
        as.BINVI(s11, s11, kCarryBit);
    } else {
        as.LI(t0, flags::Carry);
        as.XOR(s11, s11, t0);
    }
}

void Writer::Handle(Op<OpCode::SetCarry>, OpContext& ctx) {
    const GPR src = ReadRef(ctx.Arg(0), a0);
    as.SNEZ(a0, src);
    as.SLLI(a0, a0, kCarryBit);
    MergeFlags(a0, FlagBits(Flag::Carry), true);
}

void Writer::Handle(Op<OpCode::SetOverflow>, OpContext& ctx) {
    const GPR src = ReadRef(ctx.Arg(0), a0);
    as.SNEZ(a0, src);
    as.SLLI(a0, a0, kOverflowBit);
    MergeFlags(a0, FlagBits(Flag::Overflow), true);
}

void Writer::Handle(Op<OpCode::TestFlags>, OpContext& ctx) {
    const auto selected = ctx.Arg(0).Payload();
    const auto mask = PackedFlagMask(selected & ~FlagBits(Flag::Parity));
    if (mask) {
        as.LI(t0, mask);
        as.AND(ctx.destination, s11, t0);
        as.SNEZ(ctx.destination, ctx.destination);
    } else {
        as.MV(ctx.destination, x0);
    }
    if (selected & FlagBits(Flag::Parity)) {
        as.ANDI(t0, s11, 255);
        as.SRLI(t1, t0, 4);
        as.XOR(t0, t0, t1);
        as.SRLI(t1, t0, 2);
        as.XOR(t0, t0, t1);
        as.SRLI(t1, t0, 1);
        as.XOR(t0, t0, t1);
        as.XORI(t0, t0, 1);
        as.ANDI(t0, t0, 1);
        as.OR(ctx.destination, ctx.destination, t0);
    }
}

void Writer::Handle(Op<OpCode::TestNotFlags>, OpContext& ctx) {
    Handle(Op<OpCode::TestFlags>{}, ctx);
    as.XORI(ctx.destination, ctx.destination, 1);
}

void Writer::Handle(Op<OpCode::PublishFCmpFlags>, OpContext& ctx) {
    const GPR src = ReadRef(ctx.Arg(0), a0, true);
    as.ANDI(a1, src, 1);
    as.SLLI(a1, a1, kCarryBit);
    as.ANDI(t0, src, 4);
    as.SLLI(t0, t0, kOverflowBit);
    as.OR(a1, a1, t0);
    as.ANDI(t0, src, 2);
    as.SEQZ(t0, t0);
    as.OR(a1, a1, t0);
    MergeFlags(a1, FlagBits(Flag::All), true);
}

void Writer::Handle(Op<OpCode::PublishSse42StrFlags>, OpContext& ctx) {
    const GPR src = ReadRef(ctx.Arg(0), a0, true);
    as.SRLI(a1, src, 26);
    as.ANDI(a1, a1, 1);
    as.SLLI(a1, a1, kCarryBit);
    as.SRLI(t0, src, 27);
    as.ANDI(t0, t0, 1);
    as.SLLI(t0, t0, kOverflowBit);
    as.OR(a1, a1, t0);
    as.SRLI(t0, src, 25);
    as.ANDI(t0, t0, 1);
    as.SLLI(t0, t0, kZeroBit);
    as.OR(a1, a1, t0);
    as.SRLI(t0, src, 24);
    as.ANDI(t0, t0, 1);
    as.SLLI(t0, t0, kSignBit);
    as.OR(a1, a1, t0);
    as.ORI(a1, a1, 1);
    MergeFlags(a1, ctx.Arg(1).Payload(), false);
}
}  // namespace swift::runtime::backend::riscv64::detail
