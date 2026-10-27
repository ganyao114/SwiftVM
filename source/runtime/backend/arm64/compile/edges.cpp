#include "edges.h"

#include <algorithm>
#include <type_traits>
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::arm64 {

void CollectEdgeTargets(const ir::Terminal& terminal, std::vector<u64>& targets) {
    VisitVariant<void>(terminal, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                      std::is_same_v<T, ir::terminal::LinkBlockFast> ||
                      std::is_same_v<T, ir::terminal::ExternalLinkBlock>)
            targets.push_back(term.next.Value());
        else if constexpr (std::is_same_v<T, ir::terminal::If> ||
                           std::is_same_v<T, ir::terminal::Condition>) {
            CollectEdgeTargets(term.then_, targets);
            CollectEdgeTargets(term.else_, targets);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            for (const auto& item : term.cases) CollectEdgeTargets(item.then, targets);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>)
            CollectEdgeTargets(term.else_, targets);
    });
}

std::optional<ir::Location> EdgeLeafTarget(const ir::Terminal& terminal) {
    std::optional<ir::Location> result;
    VisitVariant<void>(terminal, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                      std::is_same_v<T, ir::terminal::LinkBlockFast>)
            result = term.next;
    });
    return result;
}

static_assert(sizeof(EdgeBlockAction) == 32);
static_assert(sizeof(PFAFOperand) == 16);
static_assert(sizeof(PFAFAction) == 40);
static_assert(sizeof(BackedgeAction) == 32);
static_assert(sizeof(AtomicAction) == 4);

const EdgeBlockAction* EdgeActions::Block(u64 location) const {
    const auto found =
            std::lower_bound(blocks.begin(), blocks.end(), location, [](const auto& row, u64 key) {
                return row.location < key;
            });
    return found != blocks.end() && found->location == location ? &*found : nullptr;
}

const EdgeBlockAction* EdgeActions::ForBlock(const ir::Block* block) const {
    if (!block) return nullptr;
    const auto* first = Block(block->GetStartLocation().Value());
    if (!first) return nullptr;
    if (active) return first->block == block ? first : nullptr;
    for (auto* row = first;
         row != blocks.data() + blocks.size() && row->location == first->location;
         ++row)
        if (row->block == block) return row;
    return nullptr;
}

std::optional<bool> EdgeActions::BlockTransparency(const ir::Block* block) const {
    const auto* row = ForBlock(block);
    return row ? std::optional{bool(row->transparent)} : std::nullopt;
}

bool EdgeActions::IsCycleEdge(u64 source, u64 target) const {
    return std::binary_search(cycle_edges.begin(), cycle_edges.end(), std::pair{source, target});
}

bool EdgeActions::CarriesUnitState(u64 source, u64 target) const {
    return std::binary_search(
            unit_state_edges.begin(), unit_state_edges.end(), std::pair{source, target});
}

bool EdgeActions::HasCycleSource(u64 source) const {
    const auto found = std::lower_bound(
            cycle_edges.begin(), cycle_edges.end(), source, [](const auto& row, u64 key) {
                return row.first < key;
            });
    return found != cycle_edges.end() && found->first == source;
}

bool EdgeActions::IsCanonicalEntry(u64 location) const {
    return std::binary_search(
            canonical_terminal_entries.begin(), canonical_terminal_entries.end(), location);
}

const AtomicCohort* AtomicActions::For(u32 instruction) const {
    const auto found = std::lower_bound(
            instructions.begin(), instructions.end(), instruction, [](const auto& row, u32 key) {
                return row.instruction < key;
            });
    return found != instructions.end() && found->instruction == instruction
                   ? &cohorts[found->cohort]
                   : nullptr;
}

}  // namespace swift::runtime::backend::arm64
