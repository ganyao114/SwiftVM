#pragma once

#include "runtime/ir/block.h"
#include <unordered_map>
#include <unordered_set>

namespace swift::runtime::backend::riscv64 {

struct WidePlan {
    IntrusivePtr<ir::Block> body;
    std::unordered_map<ir::Inst*, ir::Inst*> instructions;
    // A split guest access validates all 32 bytes before touching memory and
    // reuses the validated host pointer for its second native half.
    std::unordered_map<ir::Inst*, u32> memory_extents;
    std::unordered_set<ir::Inst*> memory_continuations;
    static bool Needed(ir::Block* block);
    static WidePlan Build(ir::Block* block);
};
} // namespace swift::runtime::backend::riscv64
