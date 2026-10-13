#include "translator.h"

#include <bit>
#include "runtime/common/sse42str_result.h"

namespace swift::runtime::backend::loongarch64 {
namespace {

using O = ir::OpCode;
constexpr u32 kAuxiliaryBit = 26;

u64 FlagMask(ir::Flags requested, bool parity = true) {
    u64 mask{};
    if (True(requested & ir::Flags::Negate)) mask |= u64{1} << kNegateBit;
    if (True(requested & ir::Flags::Zero)) mask |= u64{1} << kZeroBit;
    if (True(requested & ir::Flags::Carry)) mask |= u64{1} << kCarryBit;
    if (True(requested & ir::Flags::Overflow)) mask |= u64{1} << kOverflowBit;
    if (True(requested & ir::Flags::AuxiliaryCarry)) mask |= u64{1} << kAuxiliaryBit;
    if (parity && True(requested & ir::Flags::Parity)) mask |= 255;
    return mask;
}

// t6 is the incoming flags snapshot, t5 contains only replacement bits.
// ReadFlags/WriteFlags retain the resident software flags register across
// instructions; state is published at exits and semantic helper boundaries.
void MergeFlags(JitContext& context, u64 replaced) {
    if (!replaced) return;
    auto& as = context.GetMasm();
    as.LI(t7, ~replaced); as.and_(t6, t6, t7); as.or_(t6, t6, t5);
    context.WriteFlags(t6);
}

void TestFlags(JitContext& context, ir::Inst* inst, bool inverted) {
    auto& as = context.GetMasm();
    context.ReadFlags(t6);
    const auto requested = inst->GetArg<ir::Flags>(0);
    const auto nzcv = FlagMask(requested, false) & ~(u64{1} << kAuxiliaryBit);
    bool first = true;
    if (nzcv) {
        as.LI(t1, nzcv); as.and_(t1, t6, t1); as.sltu(t0, zero, t1);
        first = false;
    }
    if (True(requested & ir::Flags::Parity)) {
        as.andi(t1, t6, 255);
        for (u32 shift : {4u, 2u, 1u}) { as.srli_d(t2, t1, shift); as.xor_(t1, t1, t2); }
        as.andi(t1, t1, 1); as.xori(t1, t1, 1);
        if (first) as.MV(t0, t1); else as.and_(t0, t0, t1);
        first = false;
    }
    if (True(requested & ir::Flags::AuxiliaryCarry)) {
        as.bstrpick_d(t1, t6, kAuxiliaryBit, kAuxiliaryBit);
        if (first) as.MV(t0, t1); else as.and_(t0, t0, t1);
        first = false;
    }
    if (first) as.MV(t0, zero);
    if (inverted) as.xori(t0, t0, 1);
    context.Write(inst, t0);
}

}  // namespace

ir::Flags JitTranslator::RequestedFlags(ir::Inst* inst, ir::Flags original) {
    original &= FlagsPlan::Effect(inst).writes;
    const auto requested = context.RequiredFlagWrites(inst, original);
    stats.flag_bits_requested += std::popcount(u64(original & ir::Flags::All));
    stats.flag_bits_computed += std::popcount(u64(requested & ir::Flags::All));
    if (!True(requested & ir::Flags::All)) ++stats.flag_writes_elided;
    return requested;
}

void JitTranslator::EmitCondition(ir::Cond cond, GPR result) {
    auto& as = context.GetMasm();
    if (cond == ir::Cond::AL || cond == ir::Cond::NV) { as.LI(result, 1); return; }
    const auto word = result.getIdx() == t6.getIdx() ? t4 : t6;
    const auto scratch = result.getIdx() == t5.getIdx() ? t3 : t5;
    context.ReadFlags(word);
    const auto read = [&](GPR dst, u32 bit) { as.bstrpick_d(dst, word, bit, bit); };
    switch (cond) {
        case ir::Cond::EQ: read(result, kZeroBit); break;
        case ir::Cond::NE: read(result, kZeroBit); as.xori(result, result, 1); break;
        case ir::Cond::CS: read(result, kCarryBit); break;
        case ir::Cond::CC: read(result, kCarryBit); as.xori(result, result, 1); break;
        case ir::Cond::MI: read(result, kNegateBit); break;
        case ir::Cond::PL: read(result, kNegateBit); as.xori(result, result, 1); break;
        case ir::Cond::VS: read(result, kOverflowBit); break;
        case ir::Cond::VC: read(result, kOverflowBit); as.xori(result, result, 1); break;
        case ir::Cond::HI:
            read(result, kCarryBit); read(scratch, kZeroBit); as.xori(scratch, scratch, 1);
            as.and_(result, result, scratch); break;
        case ir::Cond::LS:
            read(result, kCarryBit); as.xori(result, result, 1); read(scratch, kZeroBit);
            as.or_(result, result, scratch); break;
        case ir::Cond::GE: case ir::Cond::LT:
            read(result, kNegateBit); read(scratch, kOverflowBit); as.xor_(result, result, scratch);
            if (cond == ir::Cond::GE) as.xori(result, result, 1);
            break;
        case ir::Cond::GT: case ir::Cond::LE:
            read(result, kNegateBit); read(scratch, kOverflowBit); as.xor_(result, result, scratch);
            read(scratch, kZeroBit);
            if (cond == ir::Cond::GT) {
                as.xori(result, result, 1); as.xori(scratch, scratch, 1); as.and_(result, result, scratch);
            } else as.or_(result, result, scratch);
            break;
        case ir::Cond::AL: case ir::Cond::NV: break;
    }
}

void JitTranslator::EmitCondSet(ir::Inst* inst) {
    EmitCondition(inst->GetArg<ir::Cond>(0), t0);
    context.Write(inst, t0);
}

void JitTranslator::EmitLocalCondSet(ir::Inst* inst) { EmitCondSet(inst); }

void JitTranslator::EmitGetFlags(ir::Inst* inst) {
    context.ReadFlags(t0);
    context.Write(inst, t0);
}

void JitTranslator::EmitSaveFlags(ir::Inst* inst) {
    const auto value = inst->GetArg<ir::Value>(0);
    auto* def = value.Def();
    const auto bits = ir::GetValueSizeByte(def->ReturnType()) * 8;
    if (!bits || bits > 64) return;
    const auto requested = RequestedFlags(inst, inst->GetArg<ir::Flags>(1));
    if (!True(requested)) return;
    const bool carry = True(requested & ir::Flags::Carry);
    const bool overflow = True(requested & ir::Flags::Overflow);
    const bool auxiliary = True(requested & ir::Flags::AuxiliaryCarry);
    const auto producer = def->GetOp();
    const bool add = producer == O::Add || producer == O::Adc;
    const bool sub = producer == O::Sub || producer == O::Sbb;
    const bool negate = producer == O::Neg;
    const bool multiply = producer == O::Mul && bits < 64;
    const bool logical = producer == O::And || producer == O::Or || producer == O::Xor || producer == O::AndNot;
    auto& as = context.GetMasm();
    context.Read(t0, ir::Value{def}); context.Mask(t0, bits);
    context.ReadFlags(t6);  // Preserve incoming ADC/SBB carry until the merge.
    as.MV(t5, zero);
    u64 replaced{};
    const auto bit = [&](GPR source, u32 position) {
        as.slli_d(t7, source, position); as.or_(t5, t5, t7);
        replaced |= u64{1} << position;
    };
    if (True(requested & ir::Flags::Negate)) {
        as.bstrpick_d(t3, t0, bits - 1, bits - 1); bit(t3, kNegateBit);
    }
    if (True(requested & ir::Flags::Zero)) { as.sltui(t3, t0, 1); bit(t3, kZeroBit); }
    if ((carry || overflow || auxiliary) && (add || sub || negate || multiply)) {
        context.Read(t1, def->GetArg<ir::Value>(0)); context.Mask(t1, bits);
        if (!negate) {
            context.Operand(t2, def->GetArg<ir::Operand>(1)); context.Mask(t2, bits);
        }
    }
    if (carry || overflow) {
        if (add) {
            if (carry) {
                // Reconstruct from the producer's operands and the current
                // incoming carry. SaveFlags can be separated from ADC by
                // an explicit carry update; its named result alone does not
                // determine the carry of that new input state.
                as.add_d(t3, t1, t2);
                if (bits == 64) as.sltu(t4, t3, t1);
                if (producer == O::Adc) {
                    as.bstrpick_d(t7, t6, kCarryBit, kCarryBit);
                    if (bits == 64) {
                        as.add_d(t7, t3, t7); as.sltu(t3, t7, t3);
                        as.or_(t4, t4, t3);
                    } else as.add_d(t3, t3, t7);
                }
                if (bits == 64) bit(t4, kCarryBit);
                else { as.bstrpick_d(t3, t3, bits, bits); bit(t3, kCarryBit); }
            }
            if (overflow) {
                as.xor_(t3, t1, t2); as.nor(t3, t3, zero);
                as.xor_(t4, t1, t0); as.and_(t3, t3, t4);
                as.bstrpick_d(t3, t3, bits - 1, bits - 1); bit(t3, kOverflowBit);
            }
        } else if (sub) {
            if (carry) {
                as.sltu(t3, t1, t2);
                if (producer == O::Sbb) {
                    as.xor_(t4, t1, t2); as.sltui(t4, t4, 1);
                    as.bstrpick_d(t7, t6, kCarryBit, kCarryBit); as.xori(t7, t7, 1);
                    as.and_(t4, t4, t7); as.or_(t3, t3, t4);
                }
                as.xori(t3, t3, 1); bit(t3, kCarryBit);
            }
            if (overflow) {
                as.xor_(t3, t1, t2); as.xor_(t4, t1, t0); as.and_(t3, t3, t4);
                as.bstrpick_d(t3, t3, bits - 1, bits - 1); bit(t3, kOverflowBit);
            }
        } else if (negate) {
            if (carry) { as.sltui(t3, t1, 1); bit(t3, kCarryBit); }
            if (overflow) {
                as.LI(t3, u64{1} << (bits - 1)); as.xor_(t3, t3, t1);
                as.sltui(t3, t3, 1); bit(t3, kOverflowBit);
            }
        } else if (multiply) {
            if (ir::IsSignValueType(def->GetArg<ir::Value>(0).Type())) {
                context.Sign(t1, bits); context.Sign(t2, bits); as.mul_d(t3, t1, t2);
                as.MV(t4, t0); context.Sign(t4, bits); as.xor_(t3, t3, t4);
            } else { as.mul_d(t3, t1, t2); as.srli_d(t3, t3, bits); }
            as.sltu(t3, zero, t3);
            if (carry) bit(t3, kCarryBit);
            if (overflow) bit(t3, kOverflowBit);
        } else if (logical) {
            if (carry) replaced |= u64{1} << kCarryBit;
            if (overflow) replaced |= u64{1} << kOverflowBit;
        }
    }
    if (True(requested & ir::Flags::Parity)) {
        as.andi(t3, t0, 255); as.or_(t5, t5, t3); replaced |= 255;
    }
    if (auxiliary && (add || sub || negate)) {
        as.xor_(t3, t1, t0);
        if (!negate) as.xor_(t3, t3, t2);
        as.bstrpick_d(t3, t3, 4, 4); bit(t3, kAuxiliaryBit);
    }
    MergeFlags(context, replaced);
}

void JitTranslator::EmitBranchOnlyFlags(ir::Inst* inst) { EmitSaveFlags(inst); }

void JitTranslator::EmitTestFlags(ir::Inst* inst) { TestFlags(context, inst, false); }
void JitTranslator::EmitTestNotFlags(ir::Inst* inst) { TestFlags(context, inst, true); }

void JitTranslator::EmitClearFlags(ir::Inst* inst) {
    const auto requested = RequestedFlags(inst, inst->GetArg<ir::Flags>(0));
    if (!True(requested)) return;
    auto& as = context.GetMasm();
    context.ReadFlags(t6);
    as.LI(t5, True(requested & ir::Flags::Parity) ? 1 : 0);
    MergeFlags(context, FlagMask(requested));
}

void JitTranslator::EmitSetCarry(ir::Inst* inst) {
    if (!True(RequestedFlags(inst, ir::Flags::Carry))) return;
    auto& as = context.GetMasm();
    context.Read(t0, inst->GetArg<ir::Value>(0)); as.andi(t0, t0, 1);
    context.ReadFlags(t6); as.slli_d(t5, t0, kCarryBit);
    MergeFlags(context, u64{1} << kCarryBit);
}

void JitTranslator::EmitSetOverflow(ir::Inst* inst) {
    if (!True(RequestedFlags(inst, ir::Flags::Overflow))) return;
    auto& as = context.GetMasm();
    context.Read(t0, inst->GetArg<ir::Value>(0)); as.andi(t0, t0, 1);
    context.ReadFlags(t6); as.slli_d(t5, t0, kOverflowBit);
    MergeFlags(context, u64{1} << kOverflowBit);
}

void JitTranslator::EmitInvertCarry(ir::Inst* inst) {
    if (!True(RequestedFlags(inst, ir::Flags::Carry))) return;
    auto& as = context.GetMasm();
    context.ReadFlags(t0); as.LI(t1, u64{1} << kCarryBit); as.xor_(t0, t0, t1);
    context.WriteFlags(t0);
}

void JitTranslator::EmitPublishFCmpFlags(ir::Inst* inst) {
    const auto requested = RequestedFlags(inst, ir::Flags::All);
    if (!True(requested)) return;
    auto& as = context.GetMasm();
    context.Read(t0, inst->GetArg<ir::Value>(0)); context.ReadFlags(t6);
    as.MV(t5, zero);
    if (True(requested & ir::Flags::Carry)) {
        as.andi(t3, t0, 1);
        if (inst->GetArg<ir::Imm>(1).Get()) as.xori(t3, t3, 1);
        as.slli_d(t3, t3, kCarryBit); as.or_(t5, t5, t3);
    }
    if (True(requested & ir::Flags::Zero)) {
        as.bstrpick_d(t3, t0, 2, 2); as.slli_d(t3, t3, kZeroBit); as.or_(t5, t5, t3);
    }
    if (True(requested & ir::Flags::Parity)) {
        as.bstrpick_d(t3, t0, 1, 1); as.xori(t3, t3, 1); as.or_(t5, t5, t3);
    }
    MergeFlags(context, FlagMask(requested));
}

void JitTranslator::EmitPublishSse42StrFlags(ir::Inst* inst) {
    const auto requested = RequestedFlags(inst, inst->GetArg<ir::Flags>(1));
    if (!True(requested)) return;
    auto& as = context.GetMasm();
    context.Read(t0, inst->GetArg<ir::Value>(0)); context.ReadFlags(t6); as.MV(t5, zero);
    u64 replaced{};
    const auto publish = [&](ir::Flags flag, u32 from, u32 to) {
        if (!True(requested & flag)) return;
        as.bstrpick_d(t3, t0, from, from); as.slli_d(t3, t3, to); as.or_(t5, t5, t3);
        replaced |= u64{1} << to;
    };
    publish(ir::Flags::Negate, sse42str::kSignBit, kNegateBit);
    publish(ir::Flags::Zero, sse42str::kZeroBit, kZeroBit);
    publish(ir::Flags::Carry, sse42str::kCarryBit, kCarryBit);
    publish(ir::Flags::Overflow, sse42str::kOverflowBit, kOverflowBit);
    if (True(requested & ir::Flags::AuxiliaryCarry)) replaced |= u64{1} << kAuxiliaryBit;
    if (True(requested & ir::Flags::Parity)) { as.ori(t5, t5, 1); replaced |= 255; }
    MergeFlags(context, replaced);
}

}  // namespace swift::runtime::backend::loongarch64
