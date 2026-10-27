#pragma once

#include <memory>

#include "runtime/backend/register_mask.h"
#include "runtime/compile/allocate/fpr_inputs.h"
#include "runtime/compile/analysis/block_index.h"

namespace swift::runtime {
struct FeatureSet;
}
namespace swift::runtime::compile {
class SemanticFacts;
}
namespace swift::runtime::backend::arm64 {

// One ordered FPR projection collects ties and publications together. Only
// semantic records are built; reverse windows and value ends are prepared
// when their candidates exist. Physical homes remain the allocator's choice.
void DescribeFPRInputs(const ir::Block*,
                       compile::SemanticFacts&,
                       u32 instruction_count,
                       FPRSMask,
                       const FeatureSet&,
                       compile::BlockAnalysisIndex&,
                       std::unique_ptr<compile::FPRInputs>&);

}  // namespace swift::runtime::backend::arm64
