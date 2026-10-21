#include "translator.h"
namespace swift::runtime::backend::arm64 {
void JitTranslator::ImportIntegerFlagPreparation() {
    ASSERT(!backedge_flags_recipe);
    for (auto* inst : IntegerFlagPreparation().integer.covered)
        disable_instructions.set(inst->Id());
}
}
