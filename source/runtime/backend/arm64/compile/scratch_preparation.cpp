#include "scratch_preparation.h"

#include "runtime/common/perf_stats.h"
#include "runtime/compile/analysis/block_index.h"

namespace swift::runtime::backend::arm64 {
ScratchActions PrepareScratch(const ir::Block* block,
                              compile::AllocationView allocation,
                              const FeatureSet& features,
                              const UnitScratchFacts& unit,
                              compile::BlockAnalysisIndex& analysis,
                              compile::CompileWorkspace& workspace) {
    if (analysis.Summary().HasPseudoConsumers()) analysis.PrepareUses();
    auto result = detail::LowerScratch(block, allocation, features, unit, analysis, workspace);
    if (Perf2Enabled()) {
        auto& stats = GetPerfStats2();
        stats.scratch_blocks.fetch_add(1, std::memory_order_relaxed);
        stats.scratch_instructions.fetch_add(analysis.Instructions().size(),
                                             std::memory_order_relaxed);
        stats.scratch_ranges.fetch_add(result.ranges.size(), std::memory_order_relaxed);
        stats.scratch_inputs.fetch_add(result.inputs.size(), std::memory_order_relaxed);
        stats.scratch_temporary_capacity_bytes.fetch_add(
                result.ranges.capacity() * sizeof(ScratchRange) +
                        result.inputs.capacity() * sizeof(SpillInputAction),
                std::memory_order_relaxed);
    }
    return result;
}
}  // namespace swift::runtime::backend::arm64
