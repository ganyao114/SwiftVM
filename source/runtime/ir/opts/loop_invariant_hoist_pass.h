#pragma once

#include <memory>
#include <vector>

#include "runtime/common/svm_config.h"
#include "runtime/ir/hir_builder.h"
#include "runtime/ir/opts/uniform_elimination_pass.h"

namespace swift::runtime::ir {

class LoopInvariantHoistPlan {
public:
    static std::unique_ptr<LoopInvariantHoistPlan> Analyze(
            HIRFunction* function,
            const UniformInfo& info,
            const FeatureSet& features);

    void Apply();
    void Revert();
    [[nodiscard]] bool Empty() const { return blocks.empty(); }

private:
    struct BlockPlan {
        Block* block{};
        std::vector<Inst*> original_order{};
        std::vector<Inst*> anchors{};
        u16 gpr_count{};
        u16 const_count{};
    };

    std::vector<BlockPlan> blocks{};
    bool applied{};
};

}  // namespace swift::runtime::ir
