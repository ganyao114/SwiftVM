#include "translator.h"

#include <unordered_map>

#include "runtime/backend/context.h"

namespace swift::runtime::backend::arm64 {

namespace {

bool IsImmediate(ir::Value value, u64 expected) {
    return value.Def() && value.Def()->GetOp() == ir::OpCode::LoadImm &&
           value.Def()->GetArg<ir::Imm>(0).Get() == expected;
}

bool IsAdjustedBoolean(ir::Value value, u32 depth = 0) {
    if (!value.Def() || depth == 4) {
        return false;
    }
    switch (value.Def()->GetOp()) {
        case ir::OpCode::LoadImm:
            return value.Def()->GetArg<ir::Imm>(0).Get() <= 1;
        case ir::OpCode::TestZero:
        case ir::OpCode::TestNotZero:
        case ir::OpCode::TestFlags:
        case ir::OpCode::TestNotFlags:
        case ir::OpCode::CondSet:
        case ir::OpCode::LocalCondSet:
        case ir::OpCode::LocalParitySet:
        case ir::OpCode::FCmpCondSet:
            return true;
        case ir::OpCode::And:
        case ir::OpCode::Or: {
            u32 inputs = 0;
            for (auto input : value.Def()->GetValues()) {
                ++inputs;
                if (!IsAdjustedBoolean(input, depth + 1)) {
                    return false;
                }
            }
            return inputs == 2;
        }
        default:
            return false;
    }
}

bool IsBooleanDirectSelect(ir::Inst* inst) {
    return inst->GetOp() == ir::OpCode::Select && inst->ReturnType() == ir::ValueType::U8 &&
           IsAdjustedBoolean(inst->GetArg<ir::Value>(0)) &&
           IsImmediate(inst->GetArg<ir::Value>(1), 1) && IsImmediate(inst->GetArg<ir::Value>(2), 0);
}

}  // namespace

void JitTranslator::PrepareBooleanSelects(ir::Block* block) {
    flag_state.boolean_selects.clear();
    flag_state.direct_cond_selects.clear();
    std::unordered_map<ir::Inst*, u32> eligible_constant_uses;
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() == ir::OpCode::Select) {
            auto* condition = inst.GetArg<ir::Value>(0).Def();
            if (condition && condition->GetOp() == ir::OpCode::CondSet &&
                condition->GetUses(false) == 1) {
                flag_state.direct_cond_selects.emplace(&inst, condition->GetArg<ir::Cond>(0));
                disable_instructions.set(condition->Id());
            }
        }
        if (!IsBooleanDirectSelect(&inst)) {
            continue;
        }
        flag_state.boolean_selects.insert(&inst);
        ++eligible_constant_uses[inst.GetArg<ir::Value>(1).Def()];
        ++eligible_constant_uses[inst.GetArg<ir::Value>(2).Def()];
    }
    for (const auto& [constant, uses] : eligible_constant_uses) {
        if (constant->GetUses() == uses) {
            disable_instructions.set(constant->Id());
        }
    }
}

#define __ masm.

void JitTranslator::EmitSelect(ir::Inst* inst) {
    auto cond = inst->GetArg<ir::Value>(0);
    auto true_value = inst->GetArg<ir::Value>(1);
    auto false_value = inst->GetArg<ir::Value>(2);
    auto result = context.R(ir::Value{inst});
    auto resolve = [&](ir::Value value) {
        return ResolvePinnedGPRUse(value, inst).value_or(context.R(value));
    };
    if (auto direct = flag_state.direct_cond_selects.find(inst); direct != flag_state.direct_cond_selects.end()) {
        const bool is_boolean = flag_state.boolean_selects.contains(inst);
        auto emit_direct = [&] {
            if (is_boolean) {
                __ Cset(result.W(), MapCond(direct->second));
            } else {
                __ Csel(result, resolve(true_value), resolve(false_value), MapCond(direct->second));
            }
        };
        if (flag_state.save_in_nzcv && flag_state.nzcv_dirty) {
            emit_direct();
        } else if (!is_boolean || !TryEmitCondSetFromFlags(inst, direct->second)) {
            LoadNZCVFromFlags();
            emit_direct();
        }
        PublishFlagsToken();
        return;
    }
    auto local = LocalConditionFor(cond);
    if (flag_state.boolean_selects.contains(inst)) {
        if (local) {
            __ Cset(result.W(), *local);
        } else {
            MergeNZCV();
            __ Cmp(context.W(cond), 0);
            __ Cset(result.W(), ne);
        }
        return;
    }
    if (local) {
        __ Csel(result, resolve(true_value), resolve(false_value), *local);
        return;
    }
    MergeNZCV();
    __ Cmp(context.W(cond), 0);
    __ Csel(result, resolve(true_value), resolve(false_value), ne);
}

void JitTranslator::EmitSelectZero(ir::Inst* inst) {
    auto test = inst->GetArg<ir::Value>(0);
    auto zero_value = inst->GetArg<ir::Value>(1);
    auto nonzero_value = inst->GetArg<ir::Value>(2);
    auto direct = pinned_gprs.pinned_select_results.find(inst);
    if (direct != pinned_gprs.pinned_select_results.end()) {
        const auto reproved = MatchPinnedSelectPublication(
                direct->second.publication);
        ASSERT_MSG(reproved && *reproved == direct->second,
                   "pinned SelectZero publication proof diverged at IR {}",
                   inst->Id());
    }
    Register result = direct == pinned_gprs.pinned_select_results.end()
            ? context.R(ir::Value{inst})
            : Register{WRegister{direct->second.target}};
    auto resolve = [&](ir::Value value) -> Register {
        return result.Is64Bits() ? Register{context.X(value)}
                                 : Register{context.W(value)};
    };
    MergeNZCV();
    __ Cmp(context.R(test), 0);
    __ Csel(result, resolve(zero_value), resolve(nonzero_value), eq);
}

#undef __

}  // namespace swift::runtime::backend::arm64
