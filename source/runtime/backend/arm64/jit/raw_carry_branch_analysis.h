#pragma once

#include <optional>
#include <unordered_map>

#include "runtime/ir/block.h"
#include "runtime/common/svm_config.h"

namespace swift::runtime::backend::arm64 {
class BlockAnalysisIndex;
struct ConsumerPlan;
struct CarryConditionAction {
    ir::Inst* predicate{};
    ir::Inst* combine{};
    ir::Inst* other{};
    ir::Cond packed_condition{};
};

class RawCarryBranchAnalysis final {
public:
    void Analyze(ir::Block* block, const FeatureSet& features,
                 const ConsumerPlan& consumers,
                 const BlockAnalysisIndex* analysis = nullptr);
    [[nodiscard]] const CarryConditionAction* FoldForTest(const ir::Inst* inst) const;

    [[nodiscard]] bool SuppressesInvert(const ir::Inst* inst) const;
    [[nodiscard]] std::optional<ir::Cond> ConditionForTest(
            const ir::Inst* inst) const;
    [[nodiscard]] ir::Inst* InvertForTest(const ir::Inst* inst) const;
    [[nodiscard]] bool Empty() const {
        return folds.empty() && test_inverts.empty() && test_conditions.empty();
    }

private:
    std::unordered_map<const ir::Inst*, CarryConditionAction> folds;
    std::unordered_map<const ir::Inst*, ir::Inst*> test_inverts;
    std::unordered_map<const ir::Inst*, ir::Cond> test_conditions;
};

}  // namespace swift::runtime::backend::arm64
