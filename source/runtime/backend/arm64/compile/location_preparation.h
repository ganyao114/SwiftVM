#pragma once

#include <vector>
#include "runtime/compile/allocate/allocation.h"

namespace swift::runtime {
struct FeatureSet;
namespace ir {
class HIRFunction;
}
namespace compile {
class SemanticFacts;
}
}  // namespace swift::runtime

namespace swift::runtime::backend::arm64 {
// Function-only terminal publication decisions. Physical queries use the
// actual terminal consumer point, after trailing call/return metadata. No
// operand materialization or labels belong here.
std::vector<u16> PrepareDeferredLocations(const ir::HIRFunction&,
                                          compile::SemanticFacts&,
                                          compile::AllocationView,
                                          const FeatureSet&);
}  // namespace swift::runtime::backend::arm64
