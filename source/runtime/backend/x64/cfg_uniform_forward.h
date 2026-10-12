#pragma once

#include "cfg_register_plan.h"

namespace swift::runtime::backend::x64 {

struct CfgUniformForwardStats {
    bool accepted{};
    u32 loads_seen{}, loads_forwarded{}, scalar_loads{}, vector_loads{};
    u32 stores_retained{}, barriers{}, iterations{};
    u32 candidate_loads{}, scalar_candidates{}, rejected_scalar_sources{}, allocation_rounds{};
    bool allocation_policy_enabled{}, allocation_limit_reached{}, final_scalar_sources_allocated{};
};
struct CfgUniformForwardOptions {
    // A provisional allocation policy, not a latency or profitability model.
    // Vector sources keep their original rules; only scalar candidates are
    // required to have an assigned register in the final policy allocation.
    bool require_scalar_source_register{};
    CfgRegisterOptions allocation_budget{};
};

// Read forwarding only: every architectural store retains its identity,
// position and operands. The graph must be freshly validated for this exact
// body. After rewriting loads, discard it and validate/rebuild allocation.
// Translator's synchronous preparation may privately adopt the final full
// allocation instead; this standalone API never exposes a reusable plan.
// Guest-edge polls may exit but never mutate uniforms on their continuation;
// opaque calls, guest memory operations and address escape kill all facts.
CfgUniformForwardStats ForwardCfgUniformLoads(backend::FunctionPlan& function,
                                             u32 uniform_size,
                                             const CfgRegisterPlan& graph,
                                             CfgUniformForwardOptions options = {});

} // namespace swift::runtime::backend::x64
