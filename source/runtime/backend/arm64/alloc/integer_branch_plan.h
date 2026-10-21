#pragma once
#include <optional>
#include <unordered_set>
#include "runtime/backend/reg_alloc.h"

namespace swift::runtime::backend::arm64 {
class BlockAnalysisIndex;
class GuestStateMap;
struct DeadEdgeIntegerBranch {
    ir::Inst* producer{};
    ir::Inst* condition{};
    ir::Flags required{};
    ir::Cond raw_condition{};
    std::optional<u16> zero_target{};
    bool zero_is_64{};
    std::unordered_set<ir::Inst*> discarded{};
};
struct DeadNarrowImmediateBranch {
    ir::Inst* producer{};
    ir::Inst* immediate_load{};
    u64 immediate{};
    ir::Flags required{};
    u8 width{};
};
struct IntegerBranchPlan {
    DeadEdgeIntegerBranch edge;
    std::optional<DeadNarrowImmediateBranch> narrow;
};
std::optional<IntegerBranchPlan> BuildIntegerBranchPlan(
        ir::Block* block, const BlockAnalysisIndex& analysis,
        const RegAlloc& allocation, const FeatureSet& features,
        const GuestStateMap& guest_state);
}
