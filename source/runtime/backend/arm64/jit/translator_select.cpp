#include "translator.h"

#include "runtime/backend/context.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

void JitTranslator::EmitSelect(ir::Inst* inst) {
    auto cond = inst->GetArg<ir::Value>(0);
    auto true_value = inst->GetArg<ir::Value>(1);
    auto false_value = inst->GetArg<ir::Value>(2);
    auto result = context.R(ir::Value{inst});
    auto resolve = [&](ir::Value value) {
        return context.UseGPR(value, inst);
    };
    const auto* action = gpr_plan->boolean_selects ? gpr_plan->boolean_selects->ActionFor(inst) : nullptr;
    if (action && action->direct_condition) {
        const auto direct = *action->direct_condition;
        const bool is_boolean = action->boolean;
        auto emit_direct = [&] {
            if (is_boolean) {
                __ Cset(result.W(), MapCond(direct));
            } else {
                __ Csel(result, resolve(true_value), resolve(false_value), MapCond(direct));
            }
        };
        if (PendingNZCVCovers(FlagsForCondition(direct))) {
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
    const auto direct = gpr_plan->pinned->ResultHome(inst);
    Register result = direct ? Register{WRegister{*direct}} : context.R(ir::Value{inst});
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
