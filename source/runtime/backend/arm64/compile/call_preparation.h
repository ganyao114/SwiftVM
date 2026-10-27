#pragma once

#include <vector>
#include "runtime/common/types.h"

namespace swift::runtime::ir {
class HIRFunction;
}
namespace swift::runtime::compile {
class SemanticFacts;
}

namespace swift::runtime::backend::arm64 {
// Function-only sharing, after semantic optimization. Targets remain sorted
// by their complete address; labels and instruction emission belong to JIT.
std::vector<u64> PrepareSharedHostCalls(const ir::HIRFunction&, compile::SemanticFacts&);
}  // namespace swift::runtime::backend::arm64
