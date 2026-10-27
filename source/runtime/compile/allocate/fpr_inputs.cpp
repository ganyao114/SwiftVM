#include "fpr_inputs.h"

#include <algorithm>

namespace swift::runtime::compile {

u16 FPRInputs::TieSource(u32 value, bool scalar_insert) const {
    const auto it = std::lower_bound(
            ties.begin(), ties.end(), value, [](const Tie& tie, u32 id) { return tie.value < id; });
    return it != ties.end() && it->value == value && (!it->insert_only || scalar_insert)
                   ? it->source
                   : UINT16_MAX;
}

bool FPRInputs::ReplacesHome(u32 instruction, u16 home) const {
    ASSERT(home < 32);
    const auto it = std::upper_bound(
            writes.begin(), writes.end(), instruction, [](u32 id, const WriteRange& range) {
                return id < range.begin;
            });
    if (it == writes.begin()) return false;
    const auto& range = *std::prev(it);
    return instruction <= range.end && (range.mask & (u32{1} << home));
}

std::span<const FPRInputs::Node> FPRInputs::Nodes(const Publication& publication) const {
    return std::span{nodes}.subspan(publication.first_node, publication.node_count);
}
std::span<const u16> FPRInputs::Inputs(const Node& node) const {
    return std::span{inputs}.subspan(node.first_input, node.input_count);
}
std::span<const FPRInputs::ValueEnd> FPRInputs::Values(const BlockRange& block) const {
    return std::span{values}.subspan(block.first_value, block.value_count);
}
std::span<const FPRInputs::Publication> FPRInputs::Publications(const BlockRange& block) const {
    return std::span{publications}.subspan(block.first_publication, block.publication_count);
}

}  // namespace swift::runtime::compile
