#pragma once

#include "runtime/compile/allocate/inputs.h"

namespace swift::runtime::compile {
class AllocationWriter;
class SemanticFacts;
class CompileWorkspace;
struct FlagsDemand;
}  // namespace swift::runtime::compile

namespace swift::runtime::backend::arm64 {

// Appends immutable site requirements and accumulates both pool summaries.
void DescribeScratchInput(compile::AllocationInputs&, const ir::Inst&, compile::FlagsDemand);

// Pure description for an already bound input, also used by physical-conflict
// fixtures that must preserve an existing mapping.
compile::AllocationInputs DescribeAllocationInputs(const ir::Block*,
                                                   const compile::AllocationWriter&,
                                                   const FeatureSet&,
                                                   compile::SemanticFacts&,
                                                   compile::CompileWorkspace&);
compile::AllocationInputs DescribeAllocationInputs(const ir::HIRFunction*,
                                                   const compile::AllocationWriter&,
                                                   const FeatureSet&,
                                                   compile::SemanticFacts&,
                                                   compile::CompileWorkspace&);

// Starts a fresh semantic/physical request before producing its input tables.
compile::AllocationInputs PrepareAllocationInputs(const ir::Block*,
                                                  compile::AllocationWriter&,
                                                  const FeatureSet&);
compile::AllocationInputs PrepareAllocationInputs(const ir::HIRFunction*,
                                                  compile::AllocationWriter&,
                                                  const FeatureSet&);

}  // namespace swift::runtime::backend::arm64
