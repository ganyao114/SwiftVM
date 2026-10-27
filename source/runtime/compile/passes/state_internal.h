#pragma once

#include "passes.h"

namespace swift::runtime::compile {

void EliminateStateDeadStores(RewriteContext& rewrite, ir::Block* block,
                             const GuestStateLayout& info, const FeatureSet& features,
                             bool xmm_only = false);

}  // namespace swift::runtime::compile
