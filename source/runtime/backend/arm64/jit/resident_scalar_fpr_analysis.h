#pragma once

#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "runtime/ir/block.h"

namespace swift::runtime::backend::arm64 {

class ResidentScalarFPRAnalysis {
public:
    struct ConversionRecipe {
        u16 target{};
        bool export_result{};
    };

    void Analyze(ir::Block* block);
    [[nodiscard]] const ConversionRecipe* FindConversion(ir::Inst* conversion) const;
    [[nodiscard]] std::optional<u16> FindMemoryStore(ir::Inst* store) const;
    [[nodiscard]] bool IsDiscarded(ir::Inst* inst) const;

private:
    void AnalyzeConversions(ir::Block* block);
    void AnalyzeMemoryStores(ir::Block* block);
    void AnalyzeExtractStores(ir::Block* block);
    bool TryMapConversionStore(ir::Block* block,
                               ir::Inst* conversion,
                               ir::Inst* publication,
                               u16 target);

    std::unordered_map<ir::Inst*, ConversionRecipe> conversions{};
    std::unordered_map<ir::Inst*, u16> memory_stores{};
    std::unordered_set<ir::Inst*> discarded{};
};

}  // namespace swift::runtime::backend::arm64
