#include "inputs.h"

#include <algorithm>
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::compile {

AllocationInputs::AllocationInputs(const ir::Block* block,
                                   const FeatureSet& features,
                                   backend::GPRSMask gprs,
                                   backend::FPRSMask fprs,
                                   AllocationBudget budget)
        : block(block)
        , function(nullptr)
        , features(features)
        , instruction_count(block ? block->MaxInstrId() : 0)
        , fprs(fprs)
        , pools{{{gprs, budget, false}, {gprs, budget, false}}} {
    ASSERT(block);
}

AllocationInputs::AllocationInputs(const ir::HIRFunction* function,
                                   const FeatureSet& features,
                                   backend::GPRSMask gprs,
                                   backend::FPRSMask fprs,
                                   AllocationBudget budget)
        : block(nullptr)
        , function(function)
        , features(features)
        , instruction_count(function ? function->MaxInstrCount() : 0)
        , fprs(fprs)
        , pools{{{gprs, budget, false}, {gprs, budget, false}}} {
    ASSERT(function);
}

const AllocationInputs::PoolPolicy& AllocationInputs::PolicyFor(backend::GPRSMask pool) const {
    if (pool == pools[0].pool) return pools[0];
    ASSERT(pool == pools[1].pool);
    return pools[1];
}

bool AllocationInputs::UsesDeclaredPool(backend::GPRSMask pool) const {
    (void)PolicyFor(pool);
    return pool != pools[0].pool;
}

std::span<const AllocationInputs::Tie> AllocationInputs::TiesFor(u32 value) const {
    const auto first = std::lower_bound(
            ties.begin(), ties.end(), value, [](const Tie& tie, u32 id) { return tie.value < id; });
    const auto last = std::upper_bound(
            first, ties.end(), value, [](u32 id, const Tie& tie) { return id < tie.value; });
    return {first, last};
}

u16 AllocationInputs::PreferredGPR(u32 value) const {
    const auto found = std::lower_bound(
            preferences.begin(), preferences.end(), value, [](const Preference& item, u32 id) {
                return item.value < id;
            });
    return found != preferences.end() && found->value == value ? found->home : UINT16_MAX;
}

bool AllocationInputs::DirectlyFeedsMemory(u32 value) const {
    return std::binary_search(memory_feeders.begin(), memory_feeders.end(), value);
}

bool AllocationInputs::Publishes(u32 instruction, u32 value, u16 home) const {
    const auto found =
            std::lower_bound(publications.begin(),
                             publications.end(),
                             instruction,
                             [](const Publication& item, u32 id) { return item.instruction < id; });
    return found != publications.end() && found->instruction == instruction &&
           found->source == value && found->home == home;
}

std::span<const AllocationInputs::Publication> AllocationInputs::PublicationsFor(
        const ir::Block* block) const {
    if (block->GetInstList().empty()) return {};
    const auto first_id = block->GetInstList().front().Id();
    const auto last_id = block->GetInstList().back().Id();
    const auto first = std::lower_bound(
            publications.begin(), publications.end(), first_id, [](const Publication& row, u32 id) {
                return row.instruction < id;
            });
    const auto last = std::upper_bound(
            first, publications.end(), last_id, [](u32 id, const Publication& row) {
                return id < row.instruction;
            });
    return {first, last};
}

ScratchRequirements AllocationInputs::ScratchFor(u32 instruction, backend::GPRSMask pool) const {
    ASSERT(instruction < instruction_count);
    const auto pool_index = static_cast<u32>(&PolicyFor(pool) - pools.data());
    const auto next = std::upper_bound(
            scratch.begin(), scratch.end(), instruction, [](u32 id, const ScratchSite& site) {
                return id < site.instruction;
            });
    ASSERT(next != scratch.begin());
    const auto& profile = std::prev(next)->profile;
    return {profile.need.gpr,
            profile.need.fpr,
            profile.outside_value_pool[pool_index],
            profile.reload_headroom,
            (profile.overlap_pools & (1u << pool_index)) != 0,
            profile.spill_outside_value_pool[pool_index]};
}

void AllocationInputs::Validate(const ir::Block* input_block,
                                const ir::HIRFunction* input_function,
                                backend::GPRSMask gprs,
                                backend::FPRSMask input_fprs) const {
    ASSERT(block == input_block && function == input_function);
    ASSERT(instruction_count == (function ? function->MaxInstrCount() : block->MaxInstrId()));
    (void)PolicyFor(gprs);
    ASSERT(fprs == input_fprs);
}

}  // namespace swift::runtime::compile
