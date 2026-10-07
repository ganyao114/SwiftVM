#include "translator.h"

#include "runtime/backend/context.h"
#include "runtime/common/sse42str_result.h"

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;
using O = ir::OpCode;

namespace {

constexpr u32 auxiliary_bit = 26;

u64 FlagMask(ir::Flags requested, bool parity = true) {
    u64 mask{};
    if (True(requested & ir::Flags::Negate)) mask |= u64{1} << kNegateBit;
    if (True(requested & ir::Flags::Zero)) mask |= u64{1} << kZeroBit;
    if (True(requested & ir::Flags::Carry)) mask |= u64{1} << kCarryBit;
    if (True(requested & ir::Flags::Overflow)) mask |= u64{1} << kOverflowBit;
    if (True(requested & ir::Flags::AuxiliaryCarry)) mask |= u64{1} << auxiliary_bit;
    if (parity && True(requested & ir::Flags::Parity)) mask |= 255;
    return mask;
}

}  // namespace

bool JitTranslator::EmitFlags(ir::Inst* inst) {
    const auto op = inst->GetOp();
    switch (op) {
        case O::GetFlags: case O::SaveFlags: case O::BranchOnlyFlags:
        case O::TestFlags: case O::TestNotFlags: case O::ClearFlags:
        case O::SetCarry: case O::SetOverflow: case O::InvertCarry:
        case O::PublishFCmpFlags: case O::PublishSse42StrFlags: break;
        default: return false;
    }
    if (ir::IsFloatValueType(inst->ReturnType())) return false;
    auto& as = context.GetMasm();
    const auto result = context.ResultRegister(inst);
    if (op == O::GetFlags) {
        as.MV(result, flags);
        context.Write(inst, result);
        return true;
    }
    // s11 retains the incoming flags, including ADC/SBB carry, until the
    // complete update has been calculated. a6 accumulates replacement bits.
    ASSERT(context.FlagsEnabled());
    as.MV(a6, x0);
    u64 replaced{};
    const auto bit = [&](GPR value, u32 position) {
        as.SLLI(t0, value, position); as.OR(a6, a6, t0);
        replaced |= u64{1} << position;
    };
    if (op == O::SaveFlags || op == O::BranchOnlyFlags) {
        const auto value = inst->GetArg<ir::Value>(0);
        const auto bits = ir::GetValueSizeByte(value.Type()) * 8;
        if (!bits || bits > 64) return true;
        auto* def = value.Def();
        const auto requested = inst->GetArg<ir::Flags>(1);
        const bool carry = True(requested & ir::Flags::Carry);
        const bool overflow = True(requested & ir::Flags::Overflow);
        const bool auxiliary = True(requested & ir::Flags::AuxiliaryCarry);
        const auto producer = def->GetOp();
        context.Read(a0, value);
        if (True(requested & ir::Flags::Negate)) {
            as.SRLI(t1, a0, bits - 1); as.ANDI(t1, t1, 1); bit(t1, kNegateBit);
        }
        if (True(requested & ir::Flags::Zero)) {
            as.SEQZ(t1, a0); bit(t1, kZeroBit);
        }
        const bool add = producer == O::Add || producer == O::Adc;
        const bool sub = producer == O::Sub || producer == O::Sbb;
        const bool negate = producer == O::Neg;
        const bool multiply = producer == O::Mul && bits < 64;
        const bool logical = producer == O::And || producer == O::Or ||
                             producer == O::Xor || producer == O::AndNot;
        if ((carry || overflow || auxiliary) && (add || sub || negate || multiply)) {
            context.Read(a1, def->GetArg<ir::Value>(0)); context.Mask(a1, bits);
            if (!negate) {
                context.Operand(a2, def->GetArg<ir::Operand>(1)); context.Mask(a2, bits);
            }
        }
        if (carry || overflow) {
            bool defined = add || sub || negate || multiply || logical;
            if (add) {
                if (carry) {
                    as.ADD(t1, a1, a2);
                    if (bits == 64) as.SLTU(a3, t1, a1);
                    if (producer == O::Adc) {
                        as.SRLI(t2, flags, kCarryBit); as.ANDI(t2, t2, 1);
                        as.ADD(t3, t1, t2);
                        if (bits == 64) {
                            as.SLTU(t2, t3, t1); as.OR(a3, a3, t2);
                        }
                        as.MV(t1, t3);
                    }
                    if (bits < 64) { as.SRLI(a3, t1, bits); as.ANDI(a3, a3, 1); }
                }
                if (overflow) {
                    as.XOR(t1, a1, a2); as.NOT(t1, t1); as.XOR(t2, a1, a0);
                    as.AND(t1, t1, t2); as.SRLI(a4, t1, bits - 1); as.ANDI(a4, a4, 1);
                }
            } else if (sub) {
                if (carry) {
                    as.SLTU(a3, a1, a2);
                    if (producer == O::Sbb) {
                        as.SRLI(t2, flags, kCarryBit); as.ANDI(t2, t2, 1); as.XORI(t2, t2, 1);
                        as.XOR(t1, a1, a2); as.SEQZ(t1, t1); as.AND(t1, t1, t2); as.OR(a3, a3, t1);
                    }
                    as.XORI(a3, a3, 1);
                }
                if (overflow) {
                    as.XOR(t1, a1, a2); as.XOR(t2, a1, a0); as.AND(t1, t1, t2);
                    as.SRLI(a4, t1, bits - 1); as.ANDI(a4, a4, 1);
                }
            } else if (negate) {
                if (carry) as.SEQZ(a3, a1);
                if (overflow) {
                    as.LI(t1, u64{1} << (bits - 1)); as.XOR(t1, t1, a1); as.SEQZ(a4, t1);
                }
            } else if (multiply) {
                if (ir::IsSignValueType(def->GetArg<ir::Value>(0).Type())) {
                    context.SignExtend(a1, bits); context.SignExtend(a2, bits);
                    as.MUL(t1, a1, a2); as.MV(t2, a0); context.SignExtend(t2, bits);
                    as.XOR(t1, t1, t2); as.SNEZ(a3, t1);
                } else {
                    as.MUL(t1, a1, a2); as.SRLI(t1, t1, bits); as.SNEZ(a3, t1);
                }
                as.MV(a4, a3);
            } else if (logical) {
                as.MV(a3, x0); as.MV(a4, x0);
            }
            if (defined && carry) bit(a3, kCarryBit);
            if (defined && overflow) bit(a4, kOverflowBit);
        }
        if (True(requested & ir::Flags::Parity)) {
            as.ANDI(t1, a0, 255); as.OR(a6, a6, t1); replaced |= 255;
        }
        if (auxiliary && (add || sub || negate)) {
            as.XOR(t1, a1, a0);
            if (!negate) as.XOR(t1, t1, a2);
            as.SRLI(t1, t1, 4); as.ANDI(t1, t1, 1); bit(t1, auxiliary_bit);
        }
    } else if (op == O::TestFlags || op == O::TestNotFlags) {
        const auto requested = inst->GetArg<ir::Flags>(0);
        const auto nzcv = FlagMask(requested, false) & ~(u64{1} << auxiliary_bit);
        const auto predicate = result == t0 ? a4 : result;
        bool first = true;
        if (nzcv) {
            as.LI(t0, nzcv); as.AND(t0, flags, t0); as.SNEZ(predicate, t0); first = false;
        }
        if (True(requested & ir::Flags::Parity)) {
            as.ANDI(t0, flags, 255);
            for (u32 shift : {4u, 2u, 1u}) { as.SRLI(t1, t0, shift); as.XOR(t0, t0, t1); }
            as.ANDI(t0, t0, 1); as.XORI(t0, t0, 1);
            if (first) as.MV(predicate, t0); else as.AND(predicate, predicate, t0);
            first = false;
        }
        if (True(requested & ir::Flags::AuxiliaryCarry)) {
            as.SRLI(t0, flags, auxiliary_bit); as.ANDI(t0, t0, 1);
            if (first) as.MV(predicate, t0); else as.AND(predicate, predicate, t0);
            first = false;
        }
        if (first) as.MV(predicate, x0);
        if (op == O::TestNotFlags) as.XORI(predicate, predicate, 1);
        if (predicate != result) as.MV(result, predicate);
        context.Write(inst, result, true);
        return true;
    } else if (op == O::ClearFlags) {
        const auto requested = inst->GetArg<ir::Flags>(0);
        replaced = FlagMask(requested);
        if (True(requested & ir::Flags::Parity)) as.LI(a6, 1);
    } else if (op == O::SetCarry || op == O::SetOverflow) {
        context.Read(t1, inst->GetArg<ir::Value>(0)); as.ANDI(t1, t1, 1);
        bit(t1, op == O::SetCarry ? kCarryBit : kOverflowBit);
    } else if (op == O::InvertCarry) {
        as.LI(t0, u64{1} << kCarryBit); as.XOR(flags, flags, t0);
        context.MarkFlagsDirty();
        return true;
    } else if (op == O::PublishFCmpFlags) {
        context.Read(a0, inst->GetArg<ir::Value>(0));
        as.ANDI(t1, a0, 1);
        if (inst->GetArg<ir::Imm>(1).Get()) as.XORI(t1, t1, 1);
        bit(t1, kCarryBit);
        as.SRLI(t1, a0, 2); as.ANDI(t1, t1, 1); bit(t1, kZeroBit);
        as.SRLI(t1, a0, 1); as.ANDI(t1, t1, 1); as.XORI(t1, t1, 1); as.OR(a6, a6, t1);
        replaced |= 255 | (u64{1} << kNegateBit) | (u64{1} << kOverflowBit) | (u64{1} << auxiliary_bit);
    } else {
        context.Read(a0, inst->GetArg<ir::Value>(0));
        const auto requested = inst->GetArg<ir::Flags>(1);
        const auto publish = [&](ir::Flags flag, u32 from, u32 to) {
            if (True(requested & flag)) { as.SRLI(t1, a0, from); as.ANDI(t1, t1, 1); bit(t1, to); }
        };
        publish(ir::Flags::Negate, sse42str::kSignBit, kNegateBit);
        publish(ir::Flags::Zero, sse42str::kZeroBit, kZeroBit);
        publish(ir::Flags::Carry, sse42str::kCarryBit, kCarryBit);
        publish(ir::Flags::Overflow, sse42str::kOverflowBit, kOverflowBit);
        if (True(requested & ir::Flags::AuxiliaryCarry)) replaced |= u64{1} << auxiliary_bit;
        if (True(requested & ir::Flags::Parity)) { as.ORI(a6, a6, 1); replaced |= 255; }
    }
    if (replaced) {
        as.LI(t0, ~replaced); as.AND(flags, flags, t0); as.OR(flags, flags, a6);
        context.MarkFlagsDirty();
    }
    return true;
}

}  // namespace swift::runtime::backend::riscv64
