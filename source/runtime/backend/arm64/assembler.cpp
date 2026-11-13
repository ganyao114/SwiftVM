#include "assembler.h"

namespace swift::runtime::backend::arm64::detail {
void Assembler::Patch(WordOffset from, WordOffset to) {
    if (to > from && to <= words.size()) last_branch_target = std::max(last_branch_target, to);
    auto* instruction = vixl::aarch64::Instruction::Cast(words.data() + from);
    const auto* target = vixl::aarch64::Instruction::Cast(words.data() + to);
    instruction->SetImmPCOffsetTarget(target);
}
}  // namespace swift::runtime::backend::arm64::detail
