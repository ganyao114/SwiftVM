#pragma once

#include "cfg_register_plan.h"

namespace swift::runtime::backend::x64 {

enum class FlagsResidencyReason : u8 {
    Disabled, Forced, NoFlags, Unsupported, InvalidPlan, SaturatedEstimate, Unprofitable, Profitable
};

struct FlagsResidencyDecision {
    bool selected{};
    FlagsResidencyReason reason{FlagsResidencyReason::Disabled};
    u32 allocation_builds{}, allocation_recolors{};
    u64 weighted_flag_reads{}, weighted_flag_writes{}, weighted_carry_reads{};
    // Potential terminal exit sites, not exits executed per invocation. All
    // normal and early exits share exactly one final flags flush.
    u64 weighted_helper_calls{}, weighted_exit_boundaries{};
    u64 resident_overhead{}, scalar_lost_benefit{}, net_savings{};
};

// Static natural-loop estimate, not a dynamic spill or execution count.
// Compare both budgets from the same immutable, already-promoted IR before
// forwarding. Recoloring deep-copies topology and sparse interference after
// one full analysis; disable it to compare two independent full builds.
// Neither allocation escapes this public call or authorizes later reuse.
// The caller freezes the selected policy for forwarding and final allocation.
// Nested terminal branches credit the minimum flags savings and charge maximum
// helper overhead; Switch's default path credits zero case-body flags savings.
// Unknown reachable lowerings fail closed; exceptional recovery cost is outside
// this ordinary-return model. The emitter must deactivate residency after its
// epilogue flush, before EndMemory, for the single-exit overhead used here.
FlagsResidencyDecision SelectFlagsResidency(ir::Block* body,
    const backend::FunctionPlan& function, CfgRegisterOptions options = {},
    bool reuse_interference = true);

const char* FlagsResidencyReasonName(FlagsResidencyReason reason) noexcept;

} // namespace swift::runtime::backend::x64
