#pragma once

#include "lower_memory.h"

namespace swift::runtime::compile {
class AllocationWriter;
}
namespace swift::runtime::backend::arm64 {

// Mutable preparation owns index construction. The per-access selector and
// lowering borrow only physical locations and already prepared const facts.
MemoryActions PrepareMemory(const ir::Block*,
                            compile::AllocationWriter&,
                            const FeatureSet&,
                            const MemorySelections&,
                            const GPRResidences&,
                            MemoryOptions,
                            compile::BlockAnalysisIndex&);

}  // namespace swift::runtime::backend::arm64
