#pragma once

#include <unordered_set>

#include "runtime/ir/block.h"

namespace swift::runtime::backend::arm64 {

class ScalarCopyAnalysis {
public:
    void Analyze(ir::Block* block);
    [[nodiscard]] bool IsSelfXor(ir::Inst* inst) const;
    [[nodiscard]] bool InputDiscarded(ir::Inst* inst) const;

private:
    std::unordered_set<ir::Inst*> self_xors{};
    std::unordered_set<ir::Inst*> discarded_inputs{};
};

}  // namespace swift::runtime::backend::arm64
