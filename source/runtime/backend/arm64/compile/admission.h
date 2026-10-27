#pragma once

#include <span>
#include <utility>
#include <vector>

#include "runtime/backend/arm64/compile/scratch.h"
#include "runtime/include/config.h"
#include "runtime/ir/function_entry_provenance.h"

namespace swift::runtime::ir {
class HIRBlock;
class HIRFunction;
class Function;
}  // namespace swift::runtime::ir
namespace swift::runtime::backend::arm64 {
class UnitActions;
struct BlockActions;

struct EdgeOptions {
    bool region_edges{}, flags_regs{}, helper_leaf_abi{};
    bool backedge_latch{}, backedge_flags{}, flags_loop_lazy{}, flags_region_branch{};
    bool flags_cfinv{}, flag_m{}, alu_split_audit{};
    bool atomic_lse{}, atomic_memory_base{};
    bool flags_regs_audit{};
    bool operator==(const EdgeOptions&) const = default;
};

struct BlockCertificate {
    const ir::HIRBlock* hir{};
    const ir::Block* block{};
    u64 location{}, end_location{};
    size_t predecessor_begin{}, predecessor_count{}, successor_begin{}, successor_count{};
    const ir::HIRBlock* call_return{};
    bool call_return_target{};
    const BlockActions* emission{};
};

// One owning-unit admission certificate. It is independent of final edge
// actions and is never replicated through per-block shared owners.
struct UnitCertificate {
    bool valid : 1 {true};
    // Intrinsic definition ownership/order/dominance, independent of the
    // optional audit and of dispatcher/external-entry activation state.
    bool ssa_valid : 1 {};
    bool entry_state_valid : 1 {};
    EdgeOptions options;
    ScratchPreparationInputs scratch_inputs;
    const ir::Block* standalone{};
    const ir::HIRFunction* function{};
    const ir::Function* root{};
    u64 root_location{}, root_end{};
    u32 generation{}, allocation_attempt{};
    const UnitActions* action_owner{};
    u32 consumer_revision{};
    std::vector<const ir::HIRBlock*> all_members, listed_members, rpo, external_roots;
    std::vector<const ir::HIRBlock*> predecessors, successors;
    std::vector<std::pair<const ir::HIRBlock*, u64>> external_links;
    std::vector<ir::FunctionEntryProvenance> provenance;
    std::vector<BlockCertificate> blocks;

    [[nodiscard]] bool MatchesUnit(const ir::Block*,
                                   const ir::HIRFunction*,
                                   compile::AllocationView,
                                   const UnitActions&,
                                   const Config&,
                                   const FeatureSet&) const;
    [[nodiscard]] bool MatchesTargetInputs(compile::AllocationView,
                                           const Config&,
                                           const FeatureSet&) const;
    [[nodiscard]] bool MatchesScratchInputs(const GPRSMask&, const FeatureSet&) const;
};

[[nodiscard]] EdgeOptions CaptureEdgeOptions(const Config&, const FeatureSet&);
[[nodiscard]] UnitCertificate CaptureUnitCertificate(const ir::Block*,
                                                     const ir::HIRFunction*,
                                                     compile::AllocationView,
                                                     const UnitActions&,
                                                     const Config&,
                                                     const FeatureSet&);

}  // namespace swift::runtime::backend::arm64
