#pragma once

#include "fpr_contracts.h"

namespace swift::runtime::compile {
class AllocationWriter;
}
namespace swift::runtime::backend::arm64 {
// These entries own graph preparation and physical direct-shuffle selection.
// The necessary verifier consumes only selected positions and read-only facts.
FPRContract PrepareFPRContract(const ir::Block*,
                               compile::AllocationWriter&,
                               const FeatureSet&,
                               bool scalar_insert,
                               bool scalar_tie,
                               compile::BlockAnalysisIndex&,
                               compile::CompileWorkspace&);
void SelectPshufdDirect(compile::BlockAnalysisIndex&, compile::AllocationWriter&);
}  // namespace swift::runtime::backend::arm64
