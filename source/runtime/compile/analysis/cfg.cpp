#include "semantic_facts.h"

#include <algorithm>
#include <functional>

#include "runtime/ir/hir_builder.h"

namespace swift::runtime::compile {

bool FunctionCFGClosed(const ir::HIRFunction& function) {
    const auto members = function.GetHIRBlocks();
    StackVector<const ir::HIRBlock*, 32> owned;
    for (const auto* member : members)
        if (member) owned.push_back(member);
    std::sort(owned.begin(), owned.end(), std::less<>{});
    if (std::adjacent_find(owned.begin(), owned.end()) != owned.end()) return false;
    const auto owns = [&](const ir::HIRBlock* member) {
        return member && std::binary_search(owned.begin(), owned.end(), member, std::less<>{});
    };
    if (!owns(function.GetEntryBlock())) return false;
    for (const auto& member : function.GetHIRBlockList())
        if (!owns(&member)) return false;
    for (const auto& member : function.GetHIRBlocksRPO())
        if (!owns(&member)) return false;
    for (const auto* root : function.GetExternalEntryRoots())
        if (!owns(root)) return false;
    for (const auto& link : function.GetExternalDirectLinks())
        if (!owns(link.source)) return false;
    for (size_t i = 0; i < members.size(); ++i) {
        const auto* member = members[i];
        if (!member) continue;
        if (member->GetOrderId() != i) return false;
        const auto* call_return = member->GetCallReturnBlock();
        if (call_return && !owns(call_return)) return false;
        const auto predecessors = member->GetPredecessors();
        const auto successors = member->GetSuccessors();
        if (predecessors.size() != member->GetIncomingEdges().size() ||
            successors.size() != member->GetOutgoingEdges().size())
            return false;
        size_t index{};
        for (const auto& edge : member->GetIncomingEdges()) {
            const auto* source = edge.GetSourceBlock();
            if (!owns(source) || edge.GetTargetBlock() != member || predecessors[index++] != source)
                return false;
        }
        index = 0;
        for (const auto& edge : member->GetOutgoingEdges()) {
            const auto* target = edge.GetTargetBlock();
            if (!owns(target) || edge.GetSourceBlock() != member || successors[index++] != target)
                return false;
        }
    }
    return true;
}

FunctionCFG::FunctionCFG(const ir::HIRFunction& function) {
    Vector<std::pair<const ir::HIRBlock*, u32>> hir_ids;
    const auto count = function.GetHIRBlocksRPO().size();
    ASSERT(count < MissingBlock);
    blocks.reserve(count);
    block_ids.reserve(count);
    hir_ids.reserve(count);
    size_t edge_count{};
    for (const auto& hir : function.GetHIRBlocksRPO()) {
        const auto id = static_cast<u32>(blocks.size());
        blocks.push_back({.hir = &hir,
                          .function_entry = &hir == function.GetEntryBlock(),
                          .has_incoming_edges = hir.HasIncomingEdges(),
                          .call_return = hir.IsCallReturnBlock()});
        block_ids.emplace_back(hir.GetBlock(), id);
        hir_ids.emplace_back(&hir, id);
        edge_count += hir.GetPredecessors().size() + hir.GetOutgoingEdges().size();
    }
    const auto by_pointer = [](const auto& left, const auto& right) {
        return left.first != right.first ? std::less<>{}(left.first, right.first)
                                         : left.second < right.second;
    };
    std::sort(hir_ids.begin(), hir_ids.end(), by_pointer);
    std::sort(block_ids.begin(), block_ids.end(), by_pointer);
    const auto find_hir = [&](const ir::HIRBlock* hir) {
        const auto found = std::lower_bound(
                hir_ids.begin(), hir_ids.end(), hir, [](const auto& item, const auto* key) {
                    return std::less<>{}(item.first, key);
                });
        return found != hir_ids.end() && found->first == hir ? found->second : MissingBlock;
    };
    for (const auto* root : function.GetExternalEntryRoots()) {
        const auto id = find_hir(root);
        if (id != MissingBlock) blocks[id].external_entry = true;
    }
    ASSERT(edge_count < MissingBlock);
    edges.reserve(edge_count);
    for (auto& block : blocks) {
        block.return_block = find_hir(block.hir->GetCallReturnBlock());
        block.predecessor_begin = static_cast<u32>(edges.size());
        for (const auto* predecessor : block.hir->GetPredecessors()) {
            if (predecessor == function.GetEntryBlock() && find_hir(predecessor) == MissingBlock)
                block.function_entry = true;
            edges.push_back(find_hir(predecessor));
        }
        block.predecessor_count = static_cast<u32>(edges.size()) - block.predecessor_begin;
        block.successor_begin = static_cast<u32>(edges.size());
        for (const auto& edge : block.hir->GetOutgoingEdges())
            edges.push_back(find_hir(edge.GetTargetBlock()));
        block.successor_count = static_cast<u32>(edges.size()) - block.successor_begin;
    }
}

std::optional<u32> FunctionCFG::FindBlock(const ir::Block* block) const {
    const auto found = std::lower_bound(
            block_ids.begin(), block_ids.end(), block, [](const auto& item, const auto* key) {
                return std::less<>{}(item.first, key);
            });
    if (found == block_ids.end() || found->first != block) return std::nullopt;
    return found->second;
}

bool FunctionCFG::Matches(const ir::HIRFunction& function) const {
    if (!FunctionCFGClosed(function) || blocks.size() != function.GetHIRBlocksRPO().size())
        return false;
    const auto id_of = [&](const ir::HIRBlock* hir) {
        if (!hir) return MissingBlock;
        const auto id = FindBlock(hir->GetBlock());
        return id && blocks[*id].hir == hir ? *id : MissingBlock;
    };
    size_t index{};
    for (const auto& hir : function.GetHIRBlocksRPO()) {
        const auto& stored = blocks[index++];
        const auto predecessors = hir.GetPredecessors();
        if (stored.hir != &hir || stored.predecessor_count != predecessors.size() ||
            stored.successor_count != hir.GetOutgoingEdges().size() ||
            stored.return_block != id_of(hir.GetCallReturnBlock()) ||
            stored.has_incoming_edges != hir.HasIncomingEdges() ||
            stored.call_return != hir.IsCallReturnBlock())
            return false;
        bool entry = &hir == function.GetEntryBlock();
        for (size_t i = 0; i < predecessors.size(); ++i) {
            if (predecessors[i] == function.GetEntryBlock() &&
                id_of(predecessors[i]) == MissingBlock)
                entry = true;
            if (Predecessors(stored)[i] != id_of(predecessors[i])) return false;
        }
        const auto roots = function.GetExternalEntryRoots();
        if (stored.function_entry != entry ||
            stored.external_entry != (std::find(roots.begin(), roots.end(), &hir) != roots.end()))
            return false;
        size_t edge_index{};
        for (const auto& edge : hir.GetOutgoingEdges())
            if (Successors(stored)[edge_index++] != id_of(edge.GetTargetBlock())) return false;
    }
    return true;
}

const FunctionCFG& SemanticFacts::PrepareFunctionCFG() {
    ASSERT(function);
    if (!function_cfg) function_cfg.reset(new FunctionCFG{*function});
    return *function_cfg;
}

}  // namespace swift::runtime::compile
