#pragma once

#include <span>
#include "cfg_register_plan.h"

namespace swift::runtime::backend::x64 {

enum class CfgStatePromoteRejection {
    None, InvalidGraph, InvalidLanes, InvalidRoot, Budget, InvalidResult,
};
struct CfgStatePromoteStats {
    bool accepted{};
    CfgStatePromoteRejection rejection{CfgStatePromoteRejection::InvalidGraph};
    u32 lanes{}, entry_seeds{}, phis_inserted{}, loads_replaced{};
    // Candidate count after no-load culling; rejected lanes require a
    // retained nontrivial Phi in a cyclic SCC that also modifies the lane.
    u32 candidate_lanes{}, rejected_dirty_loop_lanes{};
    // Integration/reporting names; equal to phis_inserted/loads_replaced.
    u32 state_phis{}, loads_seen{}, loads_forwarded{};
    u32 stores_retained{}, barriers{}, partial_kills{}, iterations{};
};
struct CfgStatePromoteOptions {
    // Structural policy, not a frequency or profitability model. Keep the
    // complete semantic pass available to callers that explicitly need it.
    bool reject_dirty_loop_phis{};
};

// Complete architectural U64 lanes, supplied by the frontend. Exact full
// scalar stores and actual U64 loads establish state; partial stores and
// opaque/memory/address-escape instructions invalidate it. Narrow scalar
// reads may extract from known state but never establish an unknown lane.
// Root must start with BindLabel. A demanded real seed executes once in a
// new preheader after the translator's initial poll, before the original
// root; backedges still target that original label. New AddPhi inputs use
// the validated graph's exact incoming-edge occurrence order.
// Every original StoreUniform retains its pointer, operands and position
// relative to original instructions. No state-store elimination is done.
// Budget/verification rejection restores the original body. Discard graph
// after a successful rewrite and validate/rebuild before allocation or emission.
// Accepted with loads_replaced == 0 preserves body membership, arguments and
// IDs exactly; the caller's fresh graph can be reused in that case.
CfgStatePromoteStats PromoteCfgState(backend::FunctionPlan& function,
                                    u32 uniform_size,
                                    std::span<const u32> lane_offsets,
                                    const CfgRegisterPlan& fresh_graph,
                                    CfgStatePromoteOptions options = {});

} // namespace swift::runtime::backend::x64

