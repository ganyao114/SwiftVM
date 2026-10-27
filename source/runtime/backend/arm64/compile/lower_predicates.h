#pragma once

#include <functional>
#include "actions.h"

namespace swift::runtime::compile {
class BlockAnalysisIndex;
class AllocationView;
class GPRHomeIndex;
class CompileWorkspace;
}
namespace swift::runtime::ir {
class Inst;
}
namespace swift::runtime::backend::arm64 {

struct IntegerBranchActions final {
    IntegerBranchAction action;
    std::vector<u16> covered;
};
std::optional<IntegerBranchActions> LowerIntegerBranch(const ir::Block*,
                                                       const compile::BlockAnalysisIndex&,
                                                       compile::CompileWorkspace&,
                                                       compile::AllocationView,
                                                       const FeatureSet&,
                                                       const compile::GPRHomeIndex&);

struct SelectActions final {
    std::vector<SelectAction> actions;
    std::vector<u16> covered;
};

// Pseudo-use preparation is deferred until a selected condition window needs it.
SelectActions LowerSelects(compile::BlockAnalysisIndex&,
                           compile::CompileWorkspace&,
                           const std::function<bool(const ir::Inst&)>& can_defer_condition);
}  // namespace swift::runtime::backend::arm64
