#include "translator.h"
namespace swift::runtime::backend::arm64 {
void JitTranslator::ImportIntegerBranchPlan() {
    const auto* edge = DeadEdgePlan();
    if (!edge) return;
    for (auto* inst : edge->discarded) disable_instructions.set(inst->Id());
    local_conditions.emplace(edge->condition, MapCond(edge->raw_condition));
    if (const auto* narrow = DeadNarrowPlan())
        disable_instructions.set(narrow->immediate_load->Id());
}
bool JitTranslator::IsDeadEdgeIntegerBranchProducer(ir::Inst* inst) const {
    return DeadEdgePlan() && DeadEdgePlan()->producer == inst;
}

std::optional<ir::Cond> JitTranslator::DeadEdgeIntegerBranchCondition(
        ir::Inst* inst) const {
    if (!DeadEdgePlan() || DeadEdgePlan()->condition != inst) {
        return std::nullopt;
    }
    return DeadEdgePlan()->raw_condition;
}

bool JitTranslator::EmitDeadEdgeZeroBranch(ir::Value condition, Label* label,
                                           bool on_true) {
    if (!DeadEdgePlan() ||
        DeadEdgePlan()->condition != condition.Def() ||
        !DeadEdgePlan()->zero_target) {
        return false;
    }
    const auto target = XRegister(*DeadEdgePlan()->zero_target);
    const auto value = DeadEdgePlan()->zero_is_64
            ? Register{target.X()}
            : Register{target.W()};
    const bool zero_on_true =
            DeadEdgePlan()->raw_condition == ir::Cond::EQ;
    if (zero_on_true == on_true) {
        masm.Cbz(value, label);
    } else {
        masm.Cbnz(value, label);
    }
    return true;
}

} // namespace swift::runtime::backend::arm64
