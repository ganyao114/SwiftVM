#include "emit.h"

#include "runtime/arch/x86/flags.h"

namespace swift::runtime::backend::loongarch64::detail {
namespace flags = ::swift::runtime::x86::flags;

void Writer::Handle(Op<OpCode::ReadFPStatus>, OpContext& ctx) {
    Read(ctx.Arg(0), t2);
    as.FRCSR(t0);
    as.SRLI(t0, t0, 16);
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

void Writer::MergeFlags(GPR value, u64 selected) {
    const auto mask = PackedFlagMask(selected);
    if (!mask) return;
    as.LI(t6, mask);
    as.AND(t5, value, t6);
    as.NOT(t6, t6);
    as.AND(packed_flags, packed_flags, t6);
    as.OR(packed_flags, packed_flags, t5);
}

void Writer::SaveProducerFlags(OpContext& ctx, GPR value) {
    const auto& home = allocation.homes[ctx.node.value];
    if (home.auxiliary_offset != UINT32_MAX) FrameStore(value, home.auxiliary_offset);
    if (ctx.operation.Metadata().profile == FlagProfile::ArithmeticRaw &&
        (ctx.operation.opcode == OpCode::Sub || ctx.operation.opcode == OpCode::Sbb ||
         ctx.operation.opcode == OpCode::Neg)) {
        as.LI(t6, flags::Carry);
        as.XOR(value, value, t6);
    }
    MergeFlags(value, ctx.operation.flag_outputs);
}

void Writer::Condition(GPR destination, ir::Condition condition) {
    const auto bit = [&](GPR reg, unsigned shift) {
        as.SRLI(reg, packed_flags, shift);
        as.ANDI(reg, reg, 1);
    };
    const auto zero = [&] { bit(destination, 30); };
    const auto carry = [&] { bit(destination, 29); };
    const auto signed_less = [&] {
        bit(destination, 31);
        bit(t4, 28);
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
            bit(destination, 31);
            return;
        case ir::Condition::PL:
            bit(destination, 31);
            as.XORI(destination, destination, 1);
            return;
        case ir::Condition::VS:
            bit(destination, 28);
            return;
        case ir::Condition::VC:
            bit(destination, 28);
            as.XORI(destination, destination, 1);
            return;
        case ir::Condition::HI:
            carry();
            bit(t4, 30);
            as.XORI(t4, t4, 1);
            as.AND(destination, destination, t4);
            return;
        case ir::Condition::LS:
            carry();
            as.XORI(destination, destination, 1);
            bit(t4, 30);
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
            bit(t4, 30);
            as.OR(destination, destination, t4);
            as.XORI(destination, destination, 1);
            return;
        case ir::Condition::LE:
            signed_less();
            bit(t4, 30);
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
    Read(ctx.Arg(0), a0);
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
    as.LI(ctx.destination, table);
    as.SRL(ctx.destination, ctx.destination, a0);
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

void Writer::Handle(Op<OpCode::GetFlags>, OpContext& ctx) { as.MV(ctx.destination, packed_flags); }

void Writer::Handle(Op<OpCode::RestoreFlags>, OpContext& ctx) {
    Read(ctx.Arg(0), a0, true);
    MergeFlags(a0, ctx.Arg(1).Payload());
}

void Writer::Handle(Op<OpCode::SaveFlags>, OpContext& ctx) {
    if (!FlagProducer(source.Operations()[ctx.Arg(0).Payload()].opcode))
        Reject(CompileError::UnsupportedFlags);
    const auto offset = allocation.homes[ctx.Arg(0).Payload()].auxiliary_offset;
    if (offset == UINT32_MAX) Reject(CompileError::UnsupportedFlags);
    FrameLoad(a0, offset);
    MergeFlags(a0, ctx.Arg(1).Payload());
}

void Writer::Handle(Op<OpCode::BranchOnlyFlags>, OpContext& ctx) {
    Handle(Op<OpCode::SaveFlags>{}, ctx);
}

void Writer::Handle(Op<OpCode::ClearFlags>, OpContext& ctx) {
    as.LI(a0, (ctx.Arg(0).Payload() & FlagBits(Flag::Parity)) ? 1 : 0);
    MergeFlags(a0, ctx.Arg(0).Payload());
}

void Writer::Handle(Op<OpCode::InvertCarry>, OpContext&) {
    as.LI(t0, flags::Carry);
    as.XOR(packed_flags, packed_flags, t0);
}

void Writer::Handle(Op<OpCode::SetCarry>, OpContext& ctx) {
    Read(ctx.Arg(0), a0);
    as.SNEZ(a0, a0);
    as.SLLI(a0, a0, 29);
    MergeFlags(a0, FlagBits(Flag::Carry));
}

void Writer::Handle(Op<OpCode::SetOverflow>, OpContext& ctx) {
    Read(ctx.Arg(0), a0);
    as.SNEZ(a0, a0);
    as.SLLI(a0, a0, 28);
    MergeFlags(a0, FlagBits(Flag::Overflow));
}

void Writer::Handle(Op<OpCode::TestFlags>, OpContext& ctx) {
    const auto selected = ctx.Arg(0).Payload();
    const auto mask = PackedFlagMask(selected & ~FlagBits(Flag::Parity));
    as.LI(t0, mask);
    as.AND(ctx.destination, packed_flags, t0);
    as.SNEZ(ctx.destination, ctx.destination);
    if (selected & FlagBits(Flag::Parity)) {
        as.ANDI(t0, packed_flags, 255);
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
    Read(ctx.Arg(0), a0, true);
    as.ANDI(a1, a0, 1);
    as.SLLI(a1, a1, 29);
    as.ANDI(t0, a0, 4);
    as.SLLI(t0, t0, 28);
    as.OR(a1, a1, t0);
    as.ANDI(t0, a0, 2);
    as.SEQZ(t0, t0);
    as.OR(a1, a1, t0);
    MergeFlags(a1, FlagBits(Flag::All));
}

void Writer::Handle(Op<OpCode::PublishSse42StrFlags>, OpContext& ctx) {
    Read(ctx.Arg(0), a0, true);
    as.SRLI(a1, a0, 26);
    as.ANDI(a1, a1, 1);
    as.SLLI(a1, a1, 29);
    as.SRLI(t0, a0, 27);
    as.ANDI(t0, t0, 1);
    as.SLLI(t0, t0, 28);
    as.OR(a1, a1, t0);
    as.SRLI(t0, a0, 25);
    as.ANDI(t0, t0, 1);
    as.SLLI(t0, t0, 30);
    as.OR(a1, a1, t0);
    as.SRLI(t0, a0, 24);
    as.ANDI(t0, t0, 1);
    as.SLLI(t0, t0, 31);
    as.OR(a1, a1, t0);
    as.ORI(a1, a1, 1);
    MergeFlags(a1, ctx.Arg(1).Payload());
}
}  // namespace swift::runtime::backend::loongarch64::detail
