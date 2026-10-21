#pragma once

#include <optional>

#include "runtime/ir/block.h"

namespace swift::runtime::backend::arm64 {

class BlockAnalysisIndex;
class FrozenGPRPlan;

class Div128NarrowingAnalysis final {
public:
    explicit Div128NarrowingAnalysis(ir::Block* block) : block(block) {}

    [[nodiscard]] bool CanLowerNatively(const ir::Inst* inst) const;

private:
    friend class FrozenGPRPlan;
    // Only final unit planning may borrow an index known to belong to this block.
    Div128NarrowingAnalysis(ir::Block* block, const BlockAnalysisIndex* analysis)
            : block(block), analysis(analysis) {}

    [[nodiscard]] ir::Value Resolve(ir::Value value, u8 depth = 0) const;
    [[nodiscard]] std::optional<ir::Value> ResolveUniformLoad(
            const ir::Inst* load) const;

    ir::Block* block{};
    const BlockAnalysisIndex* analysis{};
};

}  // namespace swift::runtime::backend::arm64
