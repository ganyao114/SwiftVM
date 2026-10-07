#include "jit_context.h"

#include <algorithm>
#include <unordered_set>
#include "runtime/common/variant_util.h"
#include "runtime/ir/block.h"
#include "runtime/ir/instr.h"

namespace swift::runtime::backend::riscv64 {

void JitContext::ConfigureInitialization(ir::Block* block) {
    initial_values.clear();
    std::vector<ir::Inst*> insts;
    std::unordered_map<ir::Inst*, u32> positions, targets;
    for (auto& inst : block->GetInstList()) { positions[&inst] = insts.size(); insts.push_back(&inst); }
    if (insts.empty()) return;
    const u32 count = insts.size();
    std::vector<bool> leaders(count + 1);
    leaders[0] = leaders[count] = true;
    for (u32 i = 0; i < count; ++i) {
        const auto op = insts[i]->GetOp();
        if (op == ir::OpCode::Goto || op == ir::OpCode::NotGoto) leaders[i + 1] = true;
        if (op == ir::OpCode::BindLabel) { leaders[i] = true; targets[insts[i]->GetArg<ir::Value>(0).Def()] = i; }
    }
    std::vector<u32> starts, node(count);
    for (u32 i = 0; i < count; ++i) {
        if (leaders[i]) starts.push_back(i);
        node[i] = starts.size() - 1;
    }
    const u32 nodes = starts.size();
    starts.push_back(count);
    std::vector<std::vector<u32>> successors(nodes), predecessors(nodes);
    const auto edge = [&](u32 from, u32 to) { successors[from].push_back(to); predecessors[to].push_back(from); };
    for (u32 b = 0; b < nodes; ++b) {
        if (b + 1 < nodes) edge(b, b + 1);
        auto* last = insts[starts[b + 1] - 1];
        if ((last->GetOp() == ir::OpCode::Goto || last->GetOp() == ir::OpCode::NotGoto) && targets.contains(last))
            edge(b, node[targets.at(last)]);
    }
    // Cooper's immediate-dominator algorithm stores O(nodes) metadata,
    // rather than a quadratic matrix of SSA/CFG dominance sets.
    constexpr u32 unknown = UINT32_MAX;
    std::vector<u32> rpo, rank(nodes, unknown), idom(nodes, unknown);
    std::vector<bool> visited(nodes);
    std::vector<std::pair<u32, size_t>> walk{{0, 0}};
    visited[0] = true;
    while (!walk.empty()) {
        auto& [b, next] = walk.back();
        if (next == successors[b].size()) { rpo.push_back(b); walk.pop_back(); }
        else { const auto child = successors[b][next++]; if (!visited[child]) { visited[child] = true; walk.emplace_back(child, 0); } }
    }
    std::reverse(rpo.begin(), rpo.end());
    for (u32 i = 0; i < rpo.size(); ++i) rank[rpo[i]] = i;
    idom[0] = 0;
    const auto intersect = [&](u32 left, u32 right) {
        while (left != right) {
            while (rank[left] > rank[right]) left = idom[left];
            while (rank[right] > rank[left]) right = idom[right];
        }
        return left;
    };
    bool changed;
    do {
        changed = false;
        for (u32 index = 1; index < rpo.size(); ++index) {
            const auto b = rpo[index]; u32 parent = unknown;
            for (const auto predecessor : predecessors[b]) if (idom[predecessor] != unknown)
                parent = parent == unknown ? predecessor : intersect(parent, predecessor);
            if (idom[b] != parent) { idom[b] = parent; changed = true; }
        }
    } while (changed);
    std::vector<std::vector<u32>> children(nodes);
    for (const auto b : rpo) if (b) children[idom[b]].push_back(b);
    std::vector<u32> enter(nodes), leave(nodes); u32 time{};
    walk.emplace_back(0, 0); enter[0] = time++;
    while (!walk.empty()) {
        auto& [b, next] = walk.back();
        if (next == children[b].size()) { leave[b] = time++; walk.pop_back(); }
        else { const auto child = children[b][next++]; enter[child] = time++; walk.emplace_back(child, 0); }
    }
    std::unordered_set<ir::Inst*> needed;
    const auto use = [&](ir::Value value, u32 at) {
        if (!visited[node[std::min(at, count - 1)]]) return;
        auto* definition = value.Def();
        // Pair-result pseudos are written by their producer, before the
        // pseudo marker appears in the instruction stream.
        if (definition->GetOp() == ir::OpCode::CpuidUpper || definition->GetOp() == ir::OpCode::Div128Remainder)
            definition = definition->GetArg<ir::Value>(0).Def();
        if (!positions.contains(definition)) return; // compile.cpp rejects foreign SSA.
        const u32 where = positions.at(definition), from = node[where], to = node[std::min(at, count - 1)];
        const bool dominates = visited[from] && (from == to ? where < at : enter[from] <= enter[to] && leave[to] <= leave[from]);
        if (!dominates) needed.insert(value.Def());
    };
    const auto operands = [&](ir::Inst* inst, u32 at) {
        for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
            auto& arg = inst->ArgAt(slot);
            if (arg.IsValue()) use(arg.Get<ir::Value>(), at);
            else if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue()) use(arg.Get<ir::Lambda>().GetValue(), at);
            else if (arg.IsParams()) for (const auto& param : arg.Get<ir::Params>()) if (param.data.IsValue()) use(param.data.value, at);
        }
    };
    for (u32 i = 0; i < count; ++i) {
        auto* inst = insts[i];
        if (inst->GetOp() == ir::OpCode::BindLabel) continue;
        operands(inst, i);
        if (inst->GetOp() == ir::OpCode::SaveFlags || inst->GetOp() == ir::OpCode::BranchOnlyFlags || inst->GetOp() == ir::OpCode::GetFlags)
            operands(inst->GetArg<ir::Value>(0).Def(), i);
    }
    const auto terminal = [&](const auto& recurse, const ir::Terminal& value) -> void {
        VisitVariant<void>(value, [&](const auto& term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::If>) { use(term.cond, count); recurse(recurse, term.then_); recurse(recurse, term.else_); }
            else if constexpr (std::is_same_v<T, ir::terminal::Condition>) { recurse(recurse, term.then_); recurse(recurse, term.else_); }
            else if constexpr (std::is_same_v<T, ir::terminal::Switch>) { use(term.value, count); for (const auto& item : term.cases) recurse(recurse, item.then); }
            else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) recurse(recurse, term.else_);
        });
    };
    terminal(terminal, block->GetTerminal());
    for (auto* inst : insts) if (needed.contains(inst)) initial_values.push_back(inst);
}

void JitContext::InitializeValues() {
    for (auto* inst : initial_values) {
        Store(biscuit::x0, values, s64(inst->Id()) * kValueStride); ++value_stats.stores; ++value_stats.initializations;
        if (!ZeroHigh(inst)) { Store(biscuit::x0, values, s64(inst->Id()) * kValueStride + 8); ++value_stats.stores; ++value_stats.initializations; }
    }
}

}  // namespace swift::runtime::backend::riscv64
