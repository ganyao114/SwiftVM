#pragma once

#include <optional>
#include <unordered_set>
#include "region_pfaf_plan.h"
#include "runtime/backend/reg_alloc.h"

namespace swift::runtime::backend::arm64 {
struct BackedgeFlagsInputs {
    const FeatureSet& features;
    bool self_enabled{}, region_enabled{}, canonical_carry{};
    std::unordered_set<u64> flags_dead_targets;
};
struct BackedgeFlagsPlan {
    bool dead_successor{}, canonical_carry{}, self_is_then{};
    ir::Location self_target{}, cold_target{};
    u8 carry_inverted{};
    ir::Flags requested{};
    ir::Inst* polarity_load{};
    ir::Inst* polarity_store{};
    ir::Inst* final_save{};
    ir::Inst* final_advance{};
    std::optional<RegionPFAFPlan> pfaf;
};
std::optional<BackedgeFlagsPlan> BuildBackedgeFlagsPlan(ir::Block& block,
        const BackedgeFlagsInputs& inputs);
}
