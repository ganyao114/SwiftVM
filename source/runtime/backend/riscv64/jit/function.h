#pragma once

#include "translator.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::backend::riscv64 {

// Owns the flattened, remapped IR through the executable allocation's QSBR
// lifetime. Original HIR blocks retain their canonical instructions.
struct FunctionPlan {
    IntrusivePtr<ir::Block> body;
    std::unordered_map<ir::Inst*, ir::Inst*> targets;
    std::unordered_map<ir::Inst*, ir::Location> locations;
    std::unordered_set<ir::Inst*> stops, halt_checks, synthetic_labels;
    std::unordered_map<ir::Inst*, ir::Terminal> exits;

    static FunctionPlan Build(ir::HIRFunction* function, std::span<ir::HIRBlock* const> blocks);
};

} // namespace swift::runtime::backend::riscv64
