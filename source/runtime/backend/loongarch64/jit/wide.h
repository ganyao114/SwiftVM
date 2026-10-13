#pragma once

#include "runtime/ir/block.h"
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace swift::runtime::backend::loongarch64 {

// Explicit provenance from one original V256 operation. Only adjacent,
// lane-local halves may be fused; ordinary similar-looking SSA instructions
// never gain LASX semantics by shape matching.
struct WideVectorPair {
    ir::Inst* low{};
    ir::Inst* high{};
    std::array<ir::Inst*, 2> left{}, right{};
};

struct WidePlan {
    IntrusivePtr<ir::Block> body;
    std::unordered_map<ir::Inst*, ir::Inst*> instructions;
    // A split guest access validates all 32 bytes before touching memory and
    // reuses the validated host pointer for its second native half.
    std::unordered_map<ir::Inst*, u32> memory_extents;
    std::unordered_set<ir::Inst*> memory_continuations;
    std::vector<WideVectorPair> native_pairs;
    static bool Needed(ir::Block* block);
    static WidePlan Build(ir::Block* block);
    ir::Terminal RemapTerminal(const ir::Terminal& terminal) const;
};
} // namespace swift::runtime::backend::loongarch64
