#include "gpr_inputs.h"

#include <algorithm>
#include "runtime/ir/block.h"

namespace swift::runtime::compile {
const GPRInputs::BlockRange* GPRInputs::BlockFor(const ir::Block* block) const {
    if (block->GetInstList().empty()) return nullptr;
    const auto first = block->GetInstList().front().Id();
    const auto found = std::lower_bound(
            blocks.begin(), blocks.end(), first, [](const BlockRange& range, u32 id) {
                return range.block->GetInstList().front().Id() < id;
            });
    return found != blocks.end() && found->block == block ? &*found : nullptr;
}
std::span<const GPRInputs::ReadReuse> GPRInputs::Reads(const BlockRange& range) const {
    return std::span{reads}.subspan(range.first_read, range.read_count);
}
std::span<const GPRInputs::ValueView> GPRInputs::Views(const BlockRange& range) const {
    return std::span{views}.subspan(range.first_view, range.view_count);
}
u8 GPRInputs::ConsumerFlags(u32 instruction) const {
    const auto found = std::lower_bound(
            consumer_rules.begin(),
            consumer_rules.end(),
            instruction,
            [](const ConsumerRule& rule, u32 id) { return rule.instruction < id; });
    return found != consumer_rules.end() && found->instruction == instruction ? found->flags : 0;
}
bool GPRInputs::CrossesClobber(const BlockRange& range, u32 begin, u32 end, u16 home) const {
    for (const auto& clobber :
         std::span{clobbers}.subspan(range.first_clobber, range.clobber_count)) {
        if (clobber.instruction < begin) continue;
        if (clobber.instruction > end) break;
        if (clobber.mask & (1u << home)) return true;
    }
    return false;
}
}  // namespace swift::runtime::compile
