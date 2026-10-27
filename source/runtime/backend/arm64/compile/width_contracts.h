#pragma once

#include <vector>
#include "runtime/compile/allocate/allocation.h"

namespace swift::runtime::compile {
class BlockAnalysisIndex;
}
namespace swift::runtime::backend::arm64 {
struct WidthCertificates {
    std::vector<u32> width, low32, reads;
};
// Explicit construction from the shared semantic index; no owned/fallback graph.
WidthCertificates PrepareWidthCertificates(const ir::Block*,
                                           compile::AllocationView,
                                           const FeatureSet&,
                                           const GPRSMask&,
                                           compile::BlockAnalysisIndex&);
[[nodiscard]] bool CheckWidthContracts(const ir::Block*,
                                       compile::AllocationView,
                                       const FeatureSet&,
                                       const GPRSMask&,
                                       const compile::BlockAnalysisIndex&);
}  // namespace swift::runtime::backend::arm64
