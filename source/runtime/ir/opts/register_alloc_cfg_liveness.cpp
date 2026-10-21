#include "register_alloc_internal.h"
#include "runtime/common/variant_util.h"
#include <unordered_map>
#include <unordered_set>

namespace swift::runtime::ir {
Vector<u32> CollectCFGUseEnds(HIRFunction* function) {
    Vector<u32> ends(function->MaxInstrCount());
    std::vector<HIRBlock*> blocks;
    std::unordered_map<HIRBlock*, size_t> ids;
    for (auto& block : function->GetHIRBlocksRPO()) {
        ids.emplace(&block, blocks.size()); blocks.push_back(&block);
    }
    using Values = std::unordered_set<u32>;
    std::vector<Values> definitions(blocks.size()), uses(blocks.size()), live_in(blocks.size()), live_out(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
        auto* block = blocks[i]->GetBlock();
        const auto add = [&](Value value, u32 point) {
            value = ResolveBitCastSource(value);
            if (!value.Defined() || value.Type() == ValueType::VOID || value.Id() >= ends.size()) return;
            ends[value.Id()] = std::max(ends[value.Id()], point);
            if (!definitions[i].contains(value.Id())) uses[i].insert(value.Id());
        };
        for (auto& inst : block->GetInstList()) {
            if (!inst.IsPseudoOperation()) for (auto value : inst.GetValues()) add(value, inst.Id());
            if (inst.HasValue() && !inst.IsBitCastOperation()) definitions[i].insert(inst.Id());
        }
        const u32 tail = block->IsEmptyBlock() ? 0 : block->GetInstList().back().Id();
        std::function<void(const Terminal&)> terminal;
        terminal = [&](const Terminal& tree) {
            VisitVariant<void>(tree, [&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, terminal::If>) { add(node.cond, tail); terminal(node.then_); terminal(node.else_); }
                else if constexpr (std::is_same_v<T, terminal::Switch>) { add(node.value, tail); for (const auto& item : node.cases) terminal(item.then); }
                else if constexpr (std::is_same_v<T, terminal::Condition>) { terminal(node.then_); terminal(node.else_); }
                else if constexpr (std::is_same_v<T, terminal::CheckHalt>) terminal(node.else_);
            });
        };
        terminal(block->GetTerminal());
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t index = blocks.size(); index-- > 0;) {
            Values outgoing;
            for (auto& edge : blocks[index]->GetOutgoingEdges()) {
                const auto next = ids.find(edge.dest_block);
                if (next != ids.end()) outgoing.insert(live_in[next->second].begin(), live_in[next->second].end());
            }
            Values incoming = uses[index];
            for (auto value : outgoing) if (!definitions[index].contains(value)) incoming.insert(value);
            if (incoming != live_in[index] || outgoing != live_out[index]) {
                live_in[index] = std::move(incoming); live_out[index] = std::move(outgoing); changed = true;
            }
        }
    }
    for (size_t index = 0; index < blocks.size(); ++index) {
        auto* block = blocks[index]->GetBlock();
        const u32 tail = block->IsEmptyBlock() ? 0 : block->GetInstList().back().Id();
        for (auto value : live_out[index]) if (value < ends.size()) ends[value] = std::max(ends[value], tail);
    }
    return ends;
}
}
