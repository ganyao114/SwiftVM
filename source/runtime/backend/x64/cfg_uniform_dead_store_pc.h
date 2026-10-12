#pragma once

#include "cfg_register_plan.h"
#include "runtime/include/config.h"

namespace swift::runtime::backend::x64 {
enum class ExecutionMode : u8;
enum class CfgUniformDeadStorePcRejection {
    Disabled, None, UnsupportedMode, InvalidGraph, InvalidAccess, Budget, InvalidResult,
};
struct CfgUniformDeadStorePcStats {
    bool attempted{}, accepted{};
    CfgUniformDeadStorePcRejection rejection{CfgUniformDeadStorePcRejection::Disabled};
    u32 stores_seen{}, candidates{}, stores_removed{}, removals_crossing_pc{};
    u32 pure_advance_pc_sites{}, fences{}, load_kills{}, overlapping_store_kills{};
};
struct CfgUniformDeadStorePcOptions {
    bool enabled{false};
    u32 max_instructions{100000}, max_candidates{4096};
};

// Ordinary x64 JIT-only, exclusively owned IR. PC markers stay in place.
// This opt-in pass is not a shared interpreter/SingleInstruction optimization.
// Call before all final flags, allocation, frame and emission planning; after
// success discard old plans and retain the body for this ordinary JIT route.
CfgUniformDeadStorePcStats EliminateOverwrittenUniformStoresAcrossPC(
    backend::FunctionPlan&, const Config&, ExecutionMode,
    CfgUniformDeadStorePcOptions = {});
} // namespace swift::runtime::backend::x64
