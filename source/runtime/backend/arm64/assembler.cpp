#include "assembler.h"

namespace swift::runtime::backend::arm64::detail {
void Assembler::Patch(WordOffset from, WordOffset to) {
    auto* instruction = vixl::aarch64::Instruction::Cast(words.data() + from);
    const auto* target = vixl::aarch64::Instruction::Cast(words.data() + to);
    instruction->SetImmPCOffsetTarget(target);
}
}  // namespace swift::runtime::backend::arm64::detail
