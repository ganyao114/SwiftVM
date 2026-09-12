#pragma once

#include "runtime/backend/reg_alloc.h"

namespace swift::runtime::ir {

class HIRFunction;

void RecipespillReloadRegions(HIRFunction* function,
                            backend::RegAlloc* reg_alloc,
                            const FeatureSet& features);
void RecipespillReloadRegions(Block* block, backend::RegAlloc* reg_alloc, const FeatureSet& features);

}  // namespace swift::runtime::ir
