#include "translator.h"

#include "runtime/backend/context.h"
#include "runtime/compile/analysis/effects.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

void JitTranslator::EmitSelect(const ir::Inst* inst) {
    auto cond = inst->GetArg<ir::Value>(0);
    auto true_value = inst->GetArg<ir::Value>(1);
    auto false_value = inst->GetArg<ir::Value>(2);
    auto result = context.R(ir::Value{inst});
    auto resolve = [&](ir::Value value) {
        return context.UseGPR(value, inst);
    };
    const auto* action = context.GetActions().Select(inst->Id());
    if (action && action->DirectCondition()) {
        const auto direct = *action->DirectCondition();
        const auto required = compile::ConditionFlags(direct).value_or(ir::Flags::NZCV);
        PrepareFlagRead(required);
        const bool is_boolean = action->boolean;
        auto emit_direct = [&] {
            if (is_boolean) {
                __ Cset(result.W(), MapCond(direct));
            } else {
                __ Csel(result, resolve(true_value), resolve(false_value), MapCond(direct));
            }
        };
        if (PendingNZCVCovers(required)) {
            emit_direct();
        } else {
            if (flag_state.save_in_nzcv && flag_state.nzcv_dirty) {
                MergeNZCV(FlagsRegsAuditMergeCause::ClearOrPartialWrite,
                          flags_audit_block_edge);
            }
            if (!is_boolean || !TryEmitCondSetFromFlags(inst, direct)) {
                LoadNZCVFromFlags();
                emit_direct();
            }
        }
        PublishFlagsToken();
        return;
    }
    auto local = LocalConditionFor(cond);
    if (action && action->boolean) {
        if (local) {
            __ Cset(result.W(), *local);
        } else {
            MergeNZCV();
            CompareIntegerZero(cond);
            __ Cset(result.W(), ne);
        }
        return;
    }
    if (local) {
        __ Csel(result, resolve(true_value), resolve(false_value), *local);
        return;
    }
    MergeNZCV();
    CompareIntegerZero(cond);
    __ Csel(result, resolve(true_value), resolve(false_value), ne);
}

void JitTranslator::EmitSelectZero(const ir::Inst* inst) {
    auto test = inst->GetArg<ir::Value>(0);
    auto zero_value = inst->GetArg<ir::Value>(1);
    auto nonzero_value = inst->GetArg<ir::Value>(2);
    const auto* residence = ActiveGPRResidence(inst);
    const auto direct = residence ? residence->ResultHome() : std::nullopt;
    Register result = direct ? Register{WRegister{*direct}} : context.R(ir::Value{inst});
    auto resolve = [&](ir::Value value) -> Register {
        return result.Is64Bits() ? Register{context.X(value)}
                                 : Register{context.W(value)};
    };
    MergeNZCV();
    CompareIntegerZero(test);
    __ Csel(result, resolve(zero_value), resolve(nonzero_value), eq);
}

#undef __

}  // namespace swift::runtime::backend::arm64
