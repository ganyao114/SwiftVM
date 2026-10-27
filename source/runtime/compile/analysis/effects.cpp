#include "effects.h"

namespace swift::runtime::compile {

std::optional<ir::Flags> ConditionFlags(ir::Cond condition) {
    using F = ir::Flags;
    switch (condition) {
        case ir::Cond::EQ:
        case ir::Cond::NE:
            return F::Zero;
        case ir::Cond::CS:
        case ir::Cond::CC:
            return F::Carry;
        case ir::Cond::MI:
        case ir::Cond::PL:
            return F::Negate;
        case ir::Cond::VS:
        case ir::Cond::VC:
            return F::Overflow;
        case ir::Cond::HI:
        case ir::Cond::LS:
            return F::Carry | F::Zero;
        case ir::Cond::GE:
        case ir::Cond::LT:
            return F::Negate | F::Overflow;
        case ir::Cond::GT:
        case ir::Cond::LE:
            return F::Negate | F::Overflow | F::Zero;
        case ir::Cond::AL:
        case ir::Cond::NV:
            return F::None;
    }
    return std::nullopt;
}

FlagAccess InstructionFlagAccess(const ir::Inst& inst) {
    using O = ir::OpCode;
    using F = ir::Flags;
    switch (inst.GetOp()) {
        case O::SaveFlags:
        case O::BranchOnlyFlags:
            return {F::None, inst.GetArg<F>(1)};
        case O::ClearFlags:
            return {F::None, inst.GetArg<F>(0)};
        case O::SetCarry:
            return {F::None, F::Carry};
        case O::SetOverflow:
            return {F::None, F::Overflow};
        case O::InvertCarry:
            return {F::Carry, F::Carry};
        case O::PublishFCmpFlags:
            return {F::None, F::All};
        case O::PublishSse42StrFlags:
            return {F::None, inst.GetArg<F>(1)};
        case O::TestFlags:
        case O::TestNotFlags:
            return {inst.GetArg<F>(0), F::None};
        case O::GetFlags:
            // Existing backends return the whole packed flags word, even if
            // the legacy argument names only a subset.
            return {F::All, F::None};
        case O::Adc:
        case O::Sbb:
            return {F::Carry, F::None};
        case O::CondSelect:
        case O::CondSet:
        case O::LocalCondSet:
            return {ConditionFlags(inst.GetArg<ir::Cond>(0)).value_or(F::NZCV), F::None};
        case O::FCmpCondSet:
            // Reads its named floating-point relation, not guest flags.
            return {};
        default:
            // GetEffects already resolves audited direct helper traits.
            // A synchronous fault observes the incoming flags even if the
            // normal path overwrites them before an explicit flag reader.
            const auto effects = inst.GetEffects();
            if (True(effects & (ir::IREffect::ReadFlags | ir::IREffect::WriteFlags |
                                ir::IREffect::MayFault | ir::IREffect::Unknown)) ||
                (True(effects & ir::IREffect::Call) &&
                 True(effects & (ir::IREffect::ReadGuestState | ir::IREffect::WriteGuestState |
                                 ir::IREffect::MayFault))))
                return {F::All, F::None};
            return {};
    }
}

const ir::UniformEffectSet* HelperUniformWrites(const ir::Inst& inst,
                                               u32 uniform_size,
                                               UniformWritePolicy policy) {
    switch (inst.GetOp()) {
        case ir::OpCode::CallLambda:
        case ir::OpCode::CallDynamic:
        case ir::OpCode::CallLocation: {
            const auto helper = ir::HelperStateEffects::Resolve(inst.GetArg<ir::Lambda>(0));
            if (policy == UniformWritePolicy::NoReentry) {
                if (!helper.direct || helper.MayReenter()) return nullptr;
                if (!helper.WritesGuestState()) return &ir::kNoUniformEffects;
            }
            const auto* effects = ir::LookupUniformEffectSet(helper.uniform);
            if (!effects || (effects->count && !effects->ranges)) return nullptr;
            for (u8 i = 0; i < effects->count; ++i) {
                const auto& range = effects->ranges[i];
                if (range.offset > uniform_size || range.size > uniform_size - range.offset)
                    return nullptr;
            }
            return effects;
        }
        default:
            return nullptr;
    }
}

}  // namespace swift::runtime::compile
