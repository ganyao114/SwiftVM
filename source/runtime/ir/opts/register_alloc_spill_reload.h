#pragma once

#include <span>
#include "runtime/backend/reg_alloc.h"

namespace swift::runtime::ir {

class HIRFunction;

void RecipespillReloadRegions(HIRFunction* function,
                            backend::RegAlloc* reg_alloc,
                            const FeatureSet& features,
                            std::span<const backend::FlagsDemand> flags_demands);
void RecipespillReloadRegions(Block* block, backend::RegAlloc* reg_alloc, const FeatureSet& features,
                            std::span<const backend::FlagsDemand> flags_demands);

}  // namespace swift::runtime::ir
