#pragma once

#include "scratch.h"

namespace swift::runtime::backend::arm64 {
// Required pseudo-use analysis is explicit. The const lowerer emits compact
// scratch ranges and exact local MEM transport without building another graph.
[[nodiscard]] ScratchActions PrepareScratch(const ir::Block*,
                                            compile::AllocationView,
                                            const FeatureSet&,
                                            const UnitScratchFacts&,
                                            compile::BlockAnalysisIndex&,
                                            compile::CompileWorkspace&);
}  // namespace swift::runtime::backend::arm64
