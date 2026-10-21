#pragma once

#include <map>
#include <optional>
#include <unordered_set>
#include "runtime/backend/arm64/jit/block_analysis_index.h"
#include "pstate_facts.h"

namespace swift::runtime::backend { class RegAlloc; }

namespace swift::runtime::backend::arm64 {
struct IntegerBranchPlan;
struct StaticFlagDemand {
    ir::Flags set{}, clear{};
    bool branch_only{};
    bool Null() const { return set == ir::Flags::None && clear == ir::Flags::None && !branch_only; }
};
struct NarrowCarryAction {
    ir::Inst* carry_test{};
    ir::Inst* carry_add{};
    ir::Value value{};
    std::vector<ir::Inst*> dead_inputs;
};
struct NarrowCompareAction {
    ir::Value left{};
    std::optional<ir::Value> right{};
    ir::Inst* immediate_load{};
    u32 immediate{};
    u8 width{};
    bool operator==(const NarrowCompareAction&) const = default;
};
struct NarrowIntegerFlagsInputs {
    const BitVector& earlier_coverage;
    std::map<const ir::Inst*, StaticFlagDemand> demands;
    std::map<const ir::Inst*, ir::Value> comparison_inputs;
    std::unordered_set<const ir::Inst*> excluded_comparisons;
    bool canonical_carry{};
    const PstateFacts* pstate{};
};
struct NarrowIntegerFlagsPlan {
    std::map<ir::Inst*, NarrowCarryAction> carries;
    std::map<ir::Inst*, NarrowCompareAction> comparisons;
    std::unordered_set<ir::Inst*> covered;
};
struct NarrowFlagsInput { ir::Value source; ir::Inst* consumer; };
struct IntegerFlagPreparationPlan {
    std::map<ir::Inst*, NarrowFlagsInput> inputs;
    NarrowIntegerFlagsPlan integer;
};
IntegerFlagPreparationPlan BuildIntegerFlagPreparation(
        const BlockAnalysisIndex& analysis, const RegAlloc& allocation,
        const PseudoFlagsPlan& base_flags, const IntegerBranchPlan* selected_branch,
        const BitVector& earlier_coverage, const PstateFacts* pstate,
        bool canonical_carry);
bool HasNarrowIntegerFlagsCandidates(const ir::Block& block);
NarrowIntegerFlagsPlan BuildNarrowIntegerFlagsPlan(const BlockAnalysisIndex& analysis,
        const NarrowIntegerFlagsInputs& inputs);
}
