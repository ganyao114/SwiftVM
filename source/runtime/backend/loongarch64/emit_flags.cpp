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

u32 LbtFlagMask(u64 selected) {
    return ((selected & FlagBits(Flag::Carry)) ? 0x01u : 0u) |
           ((selected & FlagBits(Flag::Parity)) ? 0x02u : 0u) |
           ((selected & FlagBits(Flag::AuxiliaryCarry)) ? 0x04u : 0u) |
           ((selected & FlagBits(Flag::Zero)) ? 0x08u : 0u) |
           ((selected & FlagBits(Flag::Negate)) ? 0x10u : 0u) |
           ((selected & FlagBits(Flag::Overflow)) ? 0x20u : 0u);
}

void Writer::EmitPackedToLbt(GPR src, u32 mask) {
    if (mask & 0x02u) {
        as.SRLI(t8, src, 4);
        as.XOR(t8, t8, src);
        as.SRLI(t7, t8, 2);
        as.XOR(t8, t8, t7);
        as.SRLI(t7, t8, 1);
        as.XOR(t8, t8, t7);
        as.XORI(t8, t8, 1);
        if (mask & 0x01u)
            as.BSTRPICK_D(t7, src, 29, 29);
        else
            as.MV(t7, x0);
        as.BSTRINS_D(t7, t8, 2, 2);
    } else if (mask & 0x01u) {
        as.BSTRPICK_D(t7, src, 29, 29);
    } else {
        as.MV(t7, x0);
    }
    if (mask & 0x04u) {
        as.SRLI(t8, src, 26);
        as.BSTRINS_D(t7, t8, 4, 4);
    }
    if ((mask & 0x18u) == 0x18u) {
        as.SRLI(t8, src, 30);
        as.BSTRINS_D(t7, t8, 7, 6);
    } else if (mask & 0x08u) {
        as.SRLI(t8, src, 30);
        as.BSTRINS_D(t7, t8, 6, 6);
    } else if (mask & 0x10u) {
        as.SRLI(t8, src, 31);
        as.BSTRINS_D(t7, t8, 7, 7);
    }
    if (mask & 0x20u) {
        as.SRLI(t8, src, 28);
        as.BSTRINS_D(t7, t8, 11, 11);
    }
}

void Writer::EmitExportLBTToPacked(GPR dst) {
    if (!lbt_dirty_mask) {
        if (dst != packed_flags) as.MV(dst, packed_flags);
        return;
    }
    as.X86MFFLAG(t7, lbt_dirty_mask);
    if (dst != packed_flags) as.MV(dst, packed_flags);
    if (lbt_dirty_mask & 0x01u) {
        if (carry_xor) {
            as.XORI(t8, t7, 1);
            as.BSTRINS_D(dst, t8, 29, 29);
        } else {
            as.BSTRINS_D(dst, t7, 29, 29);
        }
    }
    if (lbt_dirty_mask & 0x02u) {
        as.BSTRPICK_D(t8, t7, 2, 2);
        as.XORI(t8, t8, 1);
        as.BSTRINS_D(dst, t8, 7, 0);
    }
    if (lbt_dirty_mask & 0x04u) {
        as.SRLI(t8, t7, 4);
        as.BSTRINS_D(dst, t8, 26, 26);
    }
    if ((lbt_dirty_mask & 0x18u) == 0x18u) {
        as.SRLI(t8, t7, 6);
        as.BSTRINS_D(dst, t8, 31, 30);
    } else if (lbt_dirty_mask & 0x08u) {
        as.SRLI(t8, t7, 6);
        as.BSTRINS_D(dst, t8, 30, 30);
    } else if (lbt_dirty_mask & 0x10u) {
        as.SRLI(t8, t7, 7);
        as.BSTRINS_D(dst, t8, 31, 31);
    }
    if (lbt_dirty_mask & 0x20u) {
        as.SRLI(t8, t7, 11);
        as.BSTRINS_D(dst, t8, 28, 28);
    }
}

void Writer::ImportPackedToLBT() {
    if (unit_lbt_used_mask) {
        EmitPackedToLbt(packed_flags, unit_lbt_used_mask);
        as.X86MTFLAG(t7, unit_lbt_used_mask);
    }
    lbt_live = true;
    carry_xor = false;
    lbt_dirty_mask = lbt_canonical_blocks ? unit_lbt_written_mask : 0u;
}

void Writer::EnsureLBT() {
    if (!lbt_live) ImportPackedToLBT();
}

void Writer::NormalizeLBT() {
    if (!options.lbt_x86) return;
    EnsureLBT();
    if (carry_xor) {
        as.X86MFFLAG(t7, 0x01);
        as.XORI(t7, t7, 1);
        as.X86MTFLAG(t7, 0x01);
        carry_xor = false;
    }
}

void Writer::MergeFlags(GPR value, u64 selected) {
    if (options.lbt_x86) {
        if (selected & FlagBits(Flag::Direction)) {
            as.SRLI(t7, value, 10);
            as.BSTRINS_D(packed_flags, t7, 10, 10);
        }
        const u32 lbt_mask = LbtFlagMask(selected);
        if (!lbt_mask) return;
        if (lbt_mask != 0x3f) EnsureLBT();
        EmitPackedToLbt(value, lbt_mask);
        as.X86MTFLAG(t7, lbt_mask);
        lbt_live = true;
        lbt_dirty_mask |= lbt_mask;
        if (lbt_mask & 0x01) carry_xor = false;
        return;
    }
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
    if (options.lbt_x86) {
        if (condition == ir::Condition::AL || condition == ir::Condition::NV) {
            as.LI(destination, 1);
            return;
        }
        EnsureLBT();
        switch (condition) {
            case ir::Condition::EQ:
                as.SETX86J(destination, 4);
                return;
            case ir::Condition::NE:
                as.SETX86J(destination, 5);
                return;
            case ir::Condition::CS:
                as.SETX86J(destination, carry_xor ? 1 : 2);
                return;
            case ir::Condition::CC:
                as.SETX86J(destination, carry_xor ? 2 : 1);
                return;
            case ir::Condition::MI:
                as.SETX86J(destination, 10);
                return;
            case ir::Condition::PL:
                as.SETX86J(destination, 11);
                return;
            case ir::Condition::VS:
                as.SETX86J(destination, 12);
                return;
            case ir::Condition::VC:
                as.SETX86J(destination, 13);
                return;
            case ir::Condition::HI:
                if (carry_xor) {
                    as.SETX86J(destination, 0);
                } else {
                    as.SETX86J(destination, 2);
                    as.SETX86J(t4, 5);
                    as.AND(destination, destination, t4);
                }
                return;
            case ir::Condition::LS:
                if (carry_xor) {
                    as.SETX86J(destination, 3);
                } else {
                    as.SETX86J(destination, 1);
                    as.SETX86J(t4, 4);
                    as.OR(destination, destination, t4);
                }
                return;
            case ir::Condition::GE:
                as.SETX86J(destination, 7);
                return;
            case ir::Condition::LT:
                as.SETX86J(destination, 8);
                return;
            case ir::Condition::GT:
                as.SETX86J(destination, 6);
                return;
            case ir::Condition::LE:
                as.SETX86J(destination, 9);
                return;
            case ir::Condition::AL:
            case ir::Condition::NV:
                as.LI(destination, 1);
                return;
        }
    }
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

void Writer::Handle(Op<OpCode::GetFlags>, OpContext& ctx) {
    if (lbt_live) {
        EmitExportLBTToPacked(packed_flags);
    }
    as.MV(ctx.destination, packed_flags);
}

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
    const auto selected = ctx.Arg(0).Payload();
    if (options.lbt_x86) {
        if (selected & FlagBits(Flag::Direction)) as.BSTRINS_D(packed_flags, x0, 10, 10);
        const u32 lbt_mask = LbtFlagMask(selected);
        if (!lbt_mask) return;
        if (lbt_mask != 0x3f) EnsureLBT();
        as.X86MTFLAG(x0, lbt_mask);
        lbt_live = true;
        lbt_dirty_mask |= lbt_mask;
        if (lbt_mask & 0x01) carry_xor = false;
        return;
    }
    as.LI(a0, (selected & FlagBits(Flag::Parity)) ? 1 : 0);
    MergeFlags(a0, selected);
}

void Writer::Handle(Op<OpCode::InvertCarry>, OpContext&) {
    if (options.lbt_x86 && lbt_live) {
        carry_xor = !carry_xor;
        lbt_dirty_mask |= 0x01u;
        return;
    }
    as.LI(t0, flags::Carry);
    as.XOR(packed_flags, packed_flags, t0);
}

void Writer::Handle(Op<OpCode::SetCarry>, OpContext& ctx) {
    Read(ctx.Arg(0), a0);
    as.SNEZ(a0, a0);
    if (options.lbt_x86) {
        EnsureLBT();
        as.X86MTFLAG(a0, 0x01);
        lbt_dirty_mask |= 0x01u;
        carry_xor = false;
        return;
    }
    as.SLLI(a0, a0, 29);
    MergeFlags(a0, FlagBits(Flag::Carry));
}

void Writer::Handle(Op<OpCode::SetOverflow>, OpContext& ctx) {
    Read(ctx.Arg(0), a0);
    as.SNEZ(a0, a0);
    if (options.lbt_x86) {
        EnsureLBT();
        as.SLLI(a0, a0, 11);
        as.X86MTFLAG(a0, 0x20);
        lbt_dirty_mask |= 0x20u;
        return;
    }
    as.SLLI(a0, a0, 28);
    MergeFlags(a0, FlagBits(Flag::Overflow));
}

void Writer::EmitFlagTest(GPR destination, u64 selected, bool negate) {
    if (!selected) {
        as.LI(destination, negate ? 1 : 0);
        return;
    }
    if (options.lbt_x86) {
        EnsureLBT();
        if (!(selected & FlagBits(Flag::Direction))) {
            if (selected == FlagBits(Flag::Zero)) {
                as.SETX86J(destination, negate ? 5 : 4);
                return;
            }
            if (selected == FlagBits(Flag::Carry)) {
                const bool want_lbt_cf = (!negate) != carry_xor;
                as.SETX86J(destination, want_lbt_cf ? 2 : 1);
                return;
            }
            if (selected == (FlagBits(Flag::Carry) | FlagBits(Flag::Zero)) && !carry_xor) {
                as.SETX86J(destination, negate ? 0 : 3);
                return;
            }
            if (selected == FlagBits(Flag::Negate)) {
                as.SETX86J(destination, negate ? 11 : 10);
                return;
            }
            if (selected == FlagBits(Flag::Overflow)) {
                as.SETX86J(destination, negate ? 13 : 12);
                return;
            }
            if (selected == FlagBits(Flag::Parity)) {
                as.SETX86J(destination, negate ? 15 : 14);
                return;
            }
        }
        const u32 lbt_mask = LbtFlagMask(selected);
        if ((lbt_mask & 0x01) && carry_xor) NormalizeLBT();
        if (lbt_mask) {
            as.X86MFFLAG(destination, lbt_mask);
            as.SNEZ(destination, destination);
        } else {
            as.MV(destination, x0);
        }
        if (selected & FlagBits(Flag::Direction)) {
            as.BSTRPICK_D(t4, packed_flags, 10, 10);
            as.OR(destination, destination, t4);
        }
        if (negate) as.XORI(destination, destination, 1);
        return;
    }
    const auto mask = PackedFlagMask(selected & ~FlagBits(Flag::Parity));
    as.LI(t0, mask);
    as.AND(destination, packed_flags, t0);
    as.SNEZ(destination, destination);
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
        as.OR(destination, destination, t0);
    }
    if (negate) as.XORI(destination, destination, 1);
}

void Writer::Handle(Op<OpCode::TestFlags>, OpContext& ctx) {
    EmitFlagTest(ctx.destination, ctx.Arg(0).Payload(), false);
}

void Writer::Handle(Op<OpCode::TestNotFlags>, OpContext& ctx) {
    EmitFlagTest(ctx.destination, ctx.Arg(0).Payload(), true);
}

void Writer::Handle(Op<OpCode::PublishFCmpFlags>, OpContext& ctx) {
    Read(ctx.Arg(0), a0, true);
    if (options.lbt_x86) {
        as.ANDI(t0, a0, 1);
        as.SRLI(t1, a0, 1);
        as.BSTRINS_D(t0, t1, 2, 2);
        as.SRLI(t1, a0, 2);
        as.BSTRINS_D(t0, t1, 6, 6);
        as.X86MTFLAG(t0, 0x3f);
        lbt_live = true;
        lbt_dirty_mask |= 0x3fu;
        carry_xor = false;
        return;
    }
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
