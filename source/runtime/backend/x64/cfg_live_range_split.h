#pragma once

#include "cfg_register_plan.h"

namespace swift::runtime::backend::x64 {

struct CfgLiveRangeSplitOptions {
    CfgRegisterOptions allocation_budget{};
    bool scalar_rematerialization{true};
    u32 minimum_reads{3};
    u32 maximum_attempts{16};
};
struct CfgLiveRangeSplitStats {
    bool accepted{}, changed{};
    u32 full_allocation_builds{};
    u32 candidate_regions{}, attempts{}, extra_scalar_copies{}, rewritten_u64_reads{};
    u32 rejected_allocation{}, rejected_home{}, rejected_escape{}, rejected_cost{};
    u64 weighted_home_reads_before{}, weighted_home_reads_after{};
    u64 estimated_region_home_reads_removed{}, estimated_copy_home_reads_added{};
};

// Experimental standalone pass, not integrated with Translator. At most one
// U64 copy is committed. Only an already unassigned, non-rematerialized,
// scalar-only canonical source defined outside every loop is considered.
// Caller owns FunctionPlan exclusively and must discard ALL graph/allocation/
// frame authorities after a successful mutation, then synchronously rebuild
// at this exact final physical budget. No allocation authority escapes here.
// Rejected trials restore argument bookkeeping, membership, IDs and MaxInstrId;
// accepted mutation re-IDs the body once. Phi/terminal/metadata maps are never
// rewritten. This is spill-home reload localization, not arbitrary splitting
// of a previously assigned producer or a new per-edge register protocol.
CfgLiveRangeSplitStats SplitCfgScalarHomeReloads(backend::FunctionPlan& function,
                                                CfgLiveRangeSplitOptions options = {});

} // namespace swift::runtime::backend::x64
