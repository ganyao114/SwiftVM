#pragma once

#include <algorithm>
#include <map>
#include <set>
#include <span>
#include <unordered_set>

#include "frozen_gpr_plan.h"
#include "runtime/backend/edge_flags_state.h"
#include "runtime/ir/function_entry_provenance.h"

namespace swift::runtime::ir { class HIRBlock; class Function; }

namespace swift::runtime::backend::arm64 {
struct ConsumerPlan;

enum class UnalignedAtomicFallbackKind : u8 { CompareAndSwap, Exchange, FetchAdd };
struct UnalignedAtomicFallbackKey {
    UnalignedAtomicFallbackKind kind{};
    ir::ValueType type{};
    u8 address{}, result{}, first{}, second{};
    auto operator<=>(const UnalignedAtomicFallbackKey&) const = default;
};
struct UnalignedAtomicFallbackPlan {
    std::map<const ir::Inst*, UnalignedAtomicFallbackKey> keys;
    std::map<UnalignedAtomicFallbackKey, u32> counts;
};

UnalignedAtomicFallbackPlan BuildUnalignedAtomicFallbackPlan(std::span<ir::Block* const> blocks,
        const RegAlloc& allocation, bool lse, bool biased);

struct RegionPreparationOptions {
    bool region_edges{}, flags_regs{}, helper_leaf_abi{};
    bool backedge_latch{}, backedge_flags{}, flags_loop_lazy{}, flags_region_branch{};
    bool flags_cfinv{}, flag_m{};
    bool alu_split_audit{};
    bool atomic_lse{}, atomic_memory_base{};
    bool operator==(const RegionPreparationOptions&) const = default;
};

struct RegionBlockFacts {
    ir::Block* block{};
    EdgeFlagsTargetContract edge_flags;
    u16 accepted_masks{};
    bool transparent{}, token_overwritten{};
};

struct RegionBlockCertificate {
    ir::HIRBlock* hir{};
    ir::Block* block{};
    u64 location{}, end_location{};
    size_t predecessor_begin{}, predecessor_count{}, successor_begin{}, successor_count{};
    ir::HIRBlock* call_return{};
    bool call_return_target{};
    std::shared_ptr<const FrozenGPRPlan> pinned;
    std::shared_ptr<const ConsumerPlan> consumers;
    // Unreachable members have no allocated action plan, but their IR still
    // belongs to the unit certificate. Reuse the same block signature visitor.
};

struct RegionPreparationPlan {
    bool valid{true}, active{};
    UnalignedAtomicFallbackPlan unaligned_atomics;
    std::unordered_set<u64> canonical_terminal_entries;
    std::vector<u64> internal_locations;
    std::map<u64, RegionBlockFacts> blocks;
    std::set<std::pair<u64, u64>> cycle_edges;
    std::vector<std::pair<u64, u64>> backward_terminal_candidates;
    u32 local_cycle_source_count{};
    std::map<const ir::Block*, bool> transparent;

    [[nodiscard]] bool ContainsInternalBlock(u64 location) const {
        return std::binary_search(internal_locations.begin(), internal_locations.end(), location);
    }

    [[nodiscard]] std::optional<bool> BlockTransparency(const ir::Block* block) const;

    [[nodiscard]] bool MatchesUnit(ir::Block* block, ir::HIRFunction* function,
                                   const RegAlloc& allocation, const Config& config,
                                   const FeatureSet& features) const;

    [[nodiscard]] bool MatchesTargetInputs(const RegAlloc& allocation, const Config& config,
                                           const FeatureSet& features) const;
    [[nodiscard]] bool MatchesScratchInputs(const GPRSMask& pool, const FeatureSet& features) const;

    RegionPreparationOptions options;
    ScratchPreparationInputs scratch_inputs;
    ir::Block* standalone{};
    ir::HIRFunction* function{};
    const ir::Function* root{};
    u64 root_location{}, root_end{};
    u32 generation{}, allocation_attempt{};
    std::vector<ir::HIRBlock*> all_members, listed_members, rpo, external_roots;
    std::vector<ir::HIRBlock*> predecessors, successors;
    std::vector<std::pair<ir::HIRBlock*, u64>> external_links;
    std::vector<ir::FunctionEntryProvenance> provenance;
    std::vector<RegionBlockCertificate> certificate;
};

RegionPreparationOptions CaptureRegionPreparationOptions(const Config& config,
                                                         const FeatureSet& features);
RegionPreparationPlan BuildRegionPreparationPlan(ir::Block* block, ir::HIRFunction* function,
        const RegAlloc& allocation, const Config& config, const FeatureSet& features);

void CollectRegionTargets(const ir::Terminal& terminal, std::vector<u64>& targets);
std::optional<ir::Location> RegionLeafTarget(const ir::Terminal& terminal);
inline const RegionPreparationPlan& EmptyRegionPreparationPlan() {
    static const RegionPreparationPlan empty;
    return empty;
}
} // namespace swift::runtime::backend::arm64
