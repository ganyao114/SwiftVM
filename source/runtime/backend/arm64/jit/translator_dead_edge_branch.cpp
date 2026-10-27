#include "translator.h"
namespace swift::runtime::backend::arm64 {
void JitTranslator::ImportIntegerBranchCondition() {
    const auto* edge = DeadEdgePlan();
    if (!edge) return;
    local_conditions.emplace(edge->condition, MapCond(edge->raw_condition));
}
bool JitTranslator::IsDeadEdgeIntegerBranchProducer(const ir::Inst* inst) const {
    return DeadEdgePlan() && DeadEdgePlan()->producer == inst->Id();
}

std::optional<ir::Cond> JitTranslator::DeadEdgeIntegerBranchCondition(const ir::Inst* inst) const {
    if (!DeadEdgePlan() || DeadEdgePlan()->condition != inst->Id()) {
        return std::nullopt;
    }
    return DeadEdgePlan()->raw_condition;
}

bool JitTranslator::EmitDeadEdgeZeroBranch(ir::Value condition, Label* label,
                                           bool on_true) {
    if (!DeadEdgePlan() || DeadEdgePlan()->condition != condition.Id() ||
        !DeadEdgePlan()->ZeroTarget()) {
        return false;
    }
    const auto target = XRegister(*DeadEdgePlan()->ZeroTarget());
    const auto value = DeadEdgePlan()->width == 8 ? Register{target.X()} : Register{target.W()};
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
