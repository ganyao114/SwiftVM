#include "semantic_facts.h"
#include "uses.h"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

#include "runtime/ir/hir_builder.h"

namespace swift::runtime::compile {

FunctionValueFlow& SemanticFacts::PrepareFunctionValues() {
    ASSERT(function);
    if (!function_values) function_values.reset(new FunctionValueFlow);
    return *function_values;
}

bool SemanticFacts::RequiresIncomingValues(const ir::Block* current) {
    ASSERT(Contains(current));
    const bool valid = PrepareSSAValidation();
    ASSERT(valid);
    if (!valid) return true;
    if (!function_values || function_values->entry_values.empty()) return false;
    const auto id = function_cfg->FindBlock(current);
    ASSERT(id);
    if (!id) return true;
    return (function_values->entry_values[*id / 64] & (u64{1} << (*id % 64))) != 0;
}

const Vector<u32>& SemanticFacts::PrepareCFGUseEnds() {
    ASSERT(HasInput());
    if (!function) {
        static const Vector<u32> empty;
        return empty;
    }
    auto& flow = PrepareFunctionValues();
    auto& ends = flow.use_ends;
    if (flow.use_ends_ready) return ends;
    // Both domains are 16-bit. While preparing, the upper half stores the
    // defining RPO block plus one (zero means no definition); the lower half
    // stores the endpoint. Clear the owner before publishing the result.
    static_assert(std::is_same_v<decltype(std::declval<const ir::Inst&>().Id()), u16>);
    static_assert(std::is_same_v<decltype(function->MaxBlockCount()), u16>);
    constexpr unsigned owner_shift = std::numeric_limits<u16>::digits;
    constexpr u32 use_mask = std::numeric_limits<u16>::max();
    ASSERT(function->GetHIRBlocksRPO().size() <= use_mask);
    // A failed preparation can leave owner tags in this private buffer.
    // Every attempt starts fresh, even when capacity is already available.
    ends.assign(instruction_count, 0);
    const auto definition = [&](u32 id) { return (ends[id] >> owner_shift) - u32{1}; };
    const auto record_end = [&](u32 id, u32 point) {
        auto& entry = ends[id];
        entry = (entry & ~use_mask) | std::max(entry & use_mask, point);
    };
    Vector<std::pair<u32, u32>> needs;
    u32 index{};
    for (const auto& hir : function->GetHIRBlocksRPO()) {
        const auto* block = hir.GetBlock();
        const auto add = [&](ir::Value value, u32 point) {
            value = ir::ResolveBitCastSource(value);
            if (!value.Defined() || value.Type() == ir::ValueType::VOID ||
                value.Id() >= ends.size())
                return;
            const auto id = value.Id();
            record_end(id, point);
            if (definition(id) != index) needs.emplace_back(id, index);
        };
        for (const auto& instruction : block->GetInstList()) {
            if (!instruction.IsPseudoOperation())
                instruction.ForEachValue([&](ir::Value value) { add(value, instruction.Id()); });
            if (instruction.HasValue() && !instruction.IsBitCastOperation() &&
                instruction.Id() < instruction_count)
                ends[instruction.Id()] =
                        (ends[instruction.Id()] & use_mask) | ((index + 1) << owner_shift);
        }
        const auto& list = block->GetInstList();
        VisitImplicitUses(*block, list.empty() ? 0 : list.back().Id(), add);
        ++index;
    }
    std::sort(needs.begin(), needs.end());
    needs.erase(std::unique(needs.begin(), needs.end()), needs.end());
    if (!needs.empty()) {
        const auto& graph = PrepareFunctionCFG();
        const auto blocks = graph.Blocks();
        Vector<std::pair<u32, u32>> returns;
        for (u32 i = 0; i < blocks.size(); ++i)
            if (blocks[i].return_block != FunctionCFG::MissingBlock)
                returns.emplace_back(blocks[i].return_block, i);
        std::sort(returns.begin(), returns.end());
        Vector<u32> reached(blocks.size());
        StackVector<u32, 32> work;
        u32 generation{};
        for (size_t first = 0; first < needs.size();) {
            const auto value = needs[first].first;
            size_t end = first + 1;
            while (end < needs.size() && needs[end].first == value) ++end;
            if (!++generation) {
                std::fill(reached.begin(), reached.end(), 0);
                generation = 1;
            }
            const auto enqueue = [&](u32 block) {
                if (reached[block] == generation) return;
                reached[block] = generation;
                work.push_back(block);
            };
            const auto previous = [&](u32 source) {
                if (source == FunctionCFG::MissingBlock) return;
                const auto& list = blocks[source].hir->GetBlock()->GetInstList();
                if (!list.empty()) record_end(value, list.back().Id());
                // A definition kills live-in, but still needs its value live
                // through this outgoing edge. Logical call returns follow
                // the same liveness relation, independently of their ABI.
                if (definition(value) != source) enqueue(source);
            };
            for (size_t use = first; use < end; ++use) enqueue(needs[use].second);
            while (!work.empty()) {
                const auto current = work.back();
                work.pop_back();
                for (const auto source : graph.Predecessors(blocks[current])) previous(source);
                auto returned = std::lower_bound(
                        returns.begin(), returns.end(), std::pair{current, u32{0}});
                for (; returned != returns.end() && returned->first == current; ++returned)
                    previous(returned->second);
            }
            first = end;
        }
    }
    for (auto& end : ends) end &= use_mask;
    flow.use_ends_ready = true;
    return ends;
}
}  // namespace swift::runtime::compile
