#include "memory_preparation.h"

#include "runtime/compile/allocate/writer.h"
#include "runtime/compile/analysis/block_index.h"

namespace swift::runtime::backend::arm64 {
MemoryActions PrepareMemory(const ir::Block* block,
                            compile::AllocationWriter& allocation,
                            const FeatureSet& features,
                            const MemorySelections& selections,
                            const GPRResidences& residences,
                            MemoryOptions options,
                            compile::BlockAnalysisIndex& analysis) {
    MemoryActions lowered;
    for (const auto& memory : block->GetInstList()) {
        if (!detail::IsMemoryAccess(memory.GetOp())) continue;
        auto input =
                detail::SelectMemoryCandidates(memory, allocation, selections, options, analysis);
        if (!input) continue;
        if (input->post_index) {
            const auto& candidate = *input->post_index;
            const auto& preservations = allocation.PrepareGPRPreservations();
            // Selection has already picked the first encodable update. A
            // rejected window must not search for a later alternative.
            if (preservations.Conflicts(candidate.base, memory.Id(), candidate.update->Id()))
                input->post_index.reset();
            else
                analysis.PrepareResolvedUses();
        }
        if (input->Empty()) continue;
        if (input->narrow) analysis.PrepareUses();
        detail::LowerMemory(block,
                            *input,
                            allocation,
                            features,
                            selections,
                            residences,
                            options,
                            analysis,
                            lowered);
    }
    return lowered;
}
}  // namespace swift::runtime::backend::arm64
