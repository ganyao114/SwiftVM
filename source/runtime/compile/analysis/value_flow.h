#pragma once

#include "runtime/common/types.h"

namespace swift::runtime::compile {
class SemanticFacts;
class SSAValidation;

// Cached value dataflow is separate from the structural FunctionCFG. Only
// final endpoints and entry-value bits escape preparation; scratch is local.
class FunctionValueFlow final {
private:
    friend class SemanticFacts;
    friend class SSAValidation;
    Vector<u32> use_ends;
    StackVector<u64, 1> entry_values;
    bool use_ends_ready{};
};
}  // namespace swift::runtime::compile
