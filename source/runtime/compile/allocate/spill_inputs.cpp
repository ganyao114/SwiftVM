#include "spill_inputs.h"

#include <algorithm>
#include "runtime/ir/block.h"

namespace swift::runtime::compile {
const SpillInputs::BlockRange* SpillInputs::BlockFor(const ir::Block* block) const {
    if (block->GetInstList().empty()) return nullptr;
    const auto first = block->GetInstList().front().Id();
    const auto found = std::lower_bound(
            blocks.begin(), blocks.end(), first, [](const BlockRange& range, u32 id) {
                return range.block->GetInstList().front().Id() < id;
            });
    return found != blocks.end() && found->block == block ? &*found : nullptr;
}
u8 SpillInputs::FlagsFor(const BlockRange& range, u32 instruction) const {
    const auto rows = std::span{sites}.subspan(range.first_site, range.site_count);
    const auto found =
            std::lower_bound(rows.begin(), rows.end(), instruction, [](const Site& site, u32 id) {
                return site.instruction < id;
            });
    return found != rows.end() && found->instruction == instruction ? found->flags : 0;
}
std::span<const SpillInputs::Segment> SpillInputs::Segments(const BlockRange& range) const {
    return std::span{segments}.subspan(range.first_segment, range.segment_count);
}
}  // namespace swift::runtime::compile
