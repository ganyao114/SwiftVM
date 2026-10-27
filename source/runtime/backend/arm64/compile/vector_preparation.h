#pragma once

#include "lower_vector.h"

namespace swift::runtime::compile {
class CompileWorkspace;
}
namespace swift::runtime::backend::arm64 {
struct VectorActions final {
    ScalarFPRActions publications;
    ScalarFPRActions resident;
};

// Shares one candidate projection. Callers consume the two outputs at their
// original coverage points; temporary positions never escape this call.
VectorActions PrepareVector(const ir::Block*,
                            compile::AllocationView,
                            compile::BlockAnalysisIndex&,
                            compile::CompileWorkspace&);
}  // namespace swift::runtime::backend::arm64
