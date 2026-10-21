#include "translator.h"

namespace swift::runtime::backend::arm64 {
void JitTranslator::PrepareScalarFPRPublications(ir::Block*) {
    ASSERT(gpr_plan);
    if (gpr_plan->scalar_fpr)
        for (auto* node : gpr_plan->scalar_fpr->covered) disable_instructions.set(node->Id());
}
}
