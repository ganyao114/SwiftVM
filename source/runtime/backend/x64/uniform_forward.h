#pragma once

#include "runtime/ir/block.h"

namespace swift::runtime::backend::x64 {

struct UniformForwardStats {
    u32 loads_seen{};
    u32 loads_forwarded{};
    u32 stores_retained{};
    u32 scalar_loads{};
    u32 vector_loads{};
    u32 barriers{};
};

// Target-local straight-line forwarding. Keeps every architectural store at
// its original position; never enables static host mappings or store sinking.
// uniform_size is the actual State uniform-buffer extent. Invalid/unsupported
// views remain untouched for the normal translator to validate.
UniformForwardStats ForwardUniformLoads(ir::Block*, u32 uniform_size);

} // namespace swift::runtime::backend::x64
