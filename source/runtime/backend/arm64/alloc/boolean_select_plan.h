#pragma once
#include <algorithm>
#include <functional>
#include <optional>
#include <utility>
#include <vector>
#include "runtime/ir/block.h"

namespace swift::runtime::backend::arm64 {
class BlockAnalysisIndex;
struct BooleanSelectAction {
    bool boolean{};
    std::optional<ir::Cond> direct_condition;
};
struct BooleanSelectPlan {
    std::vector<std::pair<ir::Inst*, BooleanSelectAction>> actions;
    std::vector<ir::Inst*> covered;
    const BooleanSelectAction* ActionFor(const ir::Inst* inst) const {
        if (!inst) return nullptr;
        const auto found = std::lower_bound(actions.begin(), actions.end(), inst->Id(),
                [](const auto& action, u32 id) { return action.first->Id() < id; });
        return found != actions.end() && found->first == inst ? &found->second : nullptr;
    }
};
BooleanSelectPlan BuildBooleanSelectPlan(ir::Block* block, const BlockAnalysisIndex& analysis,
        const std::function<bool(const ir::Inst&)>& can_defer_condition);
}
