#pragma once

#include <optional>
#include <vector>

#include "runtime/compile/analysis/guest_state.h"
#include "runtime/compile/optimized_unit.h"
#include "runtime/compile/passes/loop_hoist.h"
#include "runtime/compile/rules/value_rules.h"
#include "runtime/include/config.h"

namespace swift::runtime::compile {

class CompileWorkspace;

struct CompileOptions {
    const GuestStateLayout* guest_layout{};
    Optimizations enabled{};
    FeatureSet features{};
    BuilderCapabilities target{};
    LoopHoistTarget hoist{};
    bool read_only_guest{};
};

[[nodiscard]] OptimizedUnit OptimizeBlock(CompileUnit unit,
                                          CompileWorkspace& workspace,
                                          const CompileOptions& options);
[[nodiscard]] OptimizedUnit OptimizeFunction(CompileUnit unit,
                                             CompileWorkspace& workspace,
                                             const CompileOptions& options);
// Optimize every region before merging actual parents and sealing the group.
// Returned units retain those parents, pools and metadata independently.
[[nodiscard]] std::optional<std::vector<OptimizedUnit>> OptimizeFunctions(
        std::vector<CompileUnit> units, CompileWorkspace& workspace, const CompileOptions& options);

}  // namespace swift::runtime::compile
