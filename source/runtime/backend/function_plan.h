#pragma once

#include <unordered_map>
#include <unordered_set>
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::backend {

// Pure IR lowering shared by host backends. Retain body with executable code;
// raw metadata keys/values refer to its instructions, not transient HIR pools.
struct FunctionPlan {
    IntrusivePtr<ir::Block> body;
    std::unordered_map<ir::Inst*, ir::Inst*> targets;
    std::unordered_map<ir::Inst*, ir::Location> locations;
    std::unordered_set<ir::Inst*> stops, halt_checks, synthetic_labels;
    std::unordered_map<ir::Inst*, ir::Terminal> exits;
    static FunctionPlan Build(ir::HIRFunction* function, std::span<ir::HIRBlock* const> blocks);
};

} // namespace swift::runtime::backend
