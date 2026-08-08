#pragma once

#include "runtime/ir/hir_builder.h"

namespace swift::runtime::ir {

struct UniformInfo;

// Keeps XMM uniform stores as unit-local SSA values until an architectural
// observation point. The pass is deliberately separate from
// UniformEliminationPass so its fault policy can be enabled and audited on its
// own.
class UniformStoreSinkPass {
public:
    static void CaptureLatestSnapshots(Block* block, const UniformInfo& info);
    static void Run(Block* block, const UniformInfo& info, const FeatureSet& features,
                    HIRFunction* hir_function = nullptr);
    static void Run(HIRFunction* hir_function, const UniformInfo& info,
                    const FeatureSet& features);
};

}  // namespace swift::runtime::ir
