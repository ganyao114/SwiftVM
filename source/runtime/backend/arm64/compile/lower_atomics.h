#pragma once

#include "edges.h"
#include "runtime/compile/allocate/allocation.h"

namespace swift::runtime::backend::arm64 {
[[nodiscard]] AtomicActions LowerAtomicFallbacks(std::span<const ir::Block* const>,
                                                 compile::AllocationView,
                                                 bool lse,
                                                 bool biased);
}
