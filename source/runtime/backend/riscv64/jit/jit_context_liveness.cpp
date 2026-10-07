#include "jit_context.h"

#include <algorithm>
#include <unordered_set>
#include "runtime/common/variant_util.h"
#include "runtime/ir/block.h"
#include "runtime/ir/instr.h"

namespace swift::runtime::backend::riscv64 {

void JitContext::ConfigureLiveness(ir::Block* block) {
    using O = ir::OpCode;
    using Set = std::unordered_set<ir::Inst*>;
    last_uses.clear(); live_entries.clear(); branch_live_entries.clear();
    bool cfg{};
    for (const auto& inst : block->GetInstList())
        cfg |= inst.GetOp() == O::Goto || inst.GetOp() == O::NotGoto || inst.GetOp() == O::BindLabel || !FallsThrough(const_cast<ir::Inst*>(&inst));
    if (!cfg) {
        // Most decoded blocks are straight-line. Avoid CFG sets and their
        // allocations on this hot compilation path.
        u32 position{};
        const auto mark = [&](ir::Value value, u32 at) { last_uses[value.Def()] = at; };
        const auto operands = [&](ir::Inst* inst, u32 at) {
            for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
                auto& arg = inst->ArgAt(slot);
                if (arg.IsValue()) mark(arg.Get<ir::Value>(), at);
                else if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue()) mark(arg.Get<ir::Lambda>().GetValue(), at);
                else if (arg.IsParams()) for (const auto& param : arg.Get<ir::Params>()) if (param.data.IsValue()) mark(param.data.value, at);
            }
        };
        for (auto& inst : block->GetInstList()) {
            last_uses.try_emplace(&inst, position);
            if (inst.GetOp() != O::AddPhi) operands(&inst, position);
            if (inst.GetOp() == O::SaveFlags || inst.GetOp() == O::BranchOnlyFlags || inst.GetOp() == O::GetFlags)
                operands(inst.GetArg<ir::Value>(0).Def(), position);
            ++position;
        }
        const auto terminal = [&](const auto& recurse, const ir::Terminal& value) -> void {
            VisitVariant<void>(value, [&](const auto& term) {
                using T = std::decay_t<decltype(term)>;
                if constexpr (std::is_same_v<T, ir::terminal::If>) { mark(term.cond, position); recurse(recurse, term.then_); recurse(recurse, term.else_); }
                else if constexpr (std::is_same_v<T, ir::terminal::Condition>) { recurse(recurse, term.then_); recurse(recurse, term.else_); }
                else if constexpr (std::is_same_v<T, ir::terminal::Switch>) { mark(term.value, position); for (const auto& item : term.cases) recurse(recurse, item.then); }
                else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) recurse(recurse, term.else_);
            });
        };
        terminal(terminal, block->GetTerminal());
        return;
    }
    std::vector<ir::Inst*> insts;
    std::unordered_map<ir::Inst*, u32> targets;
    for (auto& inst : block->GetInstList()) {
        last_uses[&inst] = insts.size(); insts.push_back(&inst);
        if (inst.GetOp() == O::BindLabel) targets[inst.GetArg<ir::Value>(0).Def()] = insts.size() - 1;
    }
    for (auto* inst : insts) if (inst->GetOp() == O::Goto || inst->GetOp() == O::NotGoto)
        if (auto found = targets.find(BranchTarget(inst)); found != targets.end()) targets[inst] = found->second;
    if (insts.empty()) return;
    const u32 count = insts.size();
    std::vector<bool> leaders(count + 1); leaders[0] = leaders[count] = true;
    for (u32 i = 0; i < count; ++i) {
        const auto op = insts[i]->GetOp();
        if (op == O::Goto || op == O::NotGoto || !FallsThrough(insts[i])) leaders[i + 1] = true;
        else if (op == O::BindLabel) leaders[i] = true;
    }
    std::vector<u32> starts, node(count);
    for (u32 i = 0; i < count; ++i) { if (leaders[i]) starts.push_back(i); node[i] = starts.size() - 1; }
    const u32 nodes = starts.size(); starts.push_back(count);
    std::vector<Set> uses(nodes), defs(nodes), edge_uses(nodes), in(nodes), out(nodes);
    std::vector<std::vector<u32>> successors(nodes);
    for (u32 b = 0; b < nodes; ++b) {
        auto* last = insts[starts[b + 1] - 1];
        if (b + 1 < nodes && FallsThrough(last)) successors[b].push_back(b + 1);
        if ((last->GetOp() == O::Goto || last->GetOp() == O::NotGoto) && targets.contains(last))
            successors[b].push_back(node[targets.at(last)]);
    }
    const auto mark = [&](ir::Value value, u32 at, bool edge = false) {
        const u32 b = node[edge && at ? at - 1 : std::min(at, count - 1)];
        auto* definition = value.Def();
        if (edge) edge_uses[b].insert(definition);
        else if (!defs[b].contains(definition)) uses[b].insert(definition);
        last_uses[definition] = std::max(last_uses[definition], at);
    };
    const auto operands = [&](ir::Inst* inst, u32 at) {
        for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
            auto& arg = inst->ArgAt(slot);
            if (arg.IsValue()) mark(arg.Get<ir::Value>(), at);
            else if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue()) mark(arg.Get<ir::Lambda>().GetValue(), at);
            else if (arg.IsParams()) for (const auto& param : arg.Get<ir::Params>()) if (param.data.IsValue()) mark(param.data.value, at);
        }
    };
    for (u32 i = 0; i < count; ++i) {
        auto* inst = insts[i];
        if (inst->GetOp() != O::BindLabel && inst->GetOp() != O::AddPhi) operands(inst, i);
        if (inst->GetOp() == O::SaveFlags || inst->GetOp() == O::BranchOnlyFlags || inst->GetOp() == O::GetFlags)
            operands(inst->GetArg<ir::Value>(0).Def(), i);
        if (inst->ReturnType() != ir::ValueType::VOID) defs[node[i]].insert(inst);
    }
    for (const auto& [value, at] : phi_edge_uses) mark(value, at, true);
    const auto terminal = [&](const auto& recurse, const ir::Terminal& value) -> void {
        VisitVariant<void>(value, [&](const auto& term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::If>) { mark(term.cond, count); recurse(recurse, term.then_); recurse(recurse, term.else_); }
            else if constexpr (std::is_same_v<T, ir::terminal::Condition>) { recurse(recurse, term.then_); recurse(recurse, term.else_); }
            else if constexpr (std::is_same_v<T, ir::terminal::Switch>) { mark(term.value, count); for (const auto& item : term.cases) recurse(recurse, item.then); }
            else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) recurse(recurse, term.else_);
        });
    };
    terminal(terminal, block->GetTerminal());
    bool changed;
    do {
        changed = false;
        for (u32 b = nodes; b-- > 0;) {
            Set next_out = edge_uses[b];
            for (const auto successor : successors[b]) next_out.insert(in[successor].begin(), in[successor].end());
            Set next_in = uses[b];
            for (auto* value : next_out) if (!defs[b].contains(value)) next_in.insert(value);
            if (next_out != out[b] || next_in != in[b]) { out[b] = std::move(next_out); in[b] = std::move(next_in); changed = true; }
        }
    } while (changed);
    for (u32 b = 0; b < nodes; ++b) {
        for (auto* value : out[b]) last_uses[value] = std::max(last_uses[value], starts[b + 1]);
        auto& live = live_entries[insts[starts[b]]]; live.assign(in[b].begin(), in[b].end());
    }
    for (auto* branch : insts) if (branch->GetOp() == O::Goto || branch->GetOp() == O::NotGoto)
        if (targets.contains(branch)) branch_live_entries[branch] = live_entries.at(insts[targets.at(branch)]);
}

void JitContext::FlushValuesTo(const std::vector<ir::Inst*>& live) {
    const auto wanted = [&](ir::Inst* inst) { return std::find(live.begin(), live.end(), inst) != live.end(); };
    for (size_t i = 0; i < cached_values.size(); ++i) if (auto* inst = cached_values[i]; inst && wanted(inst)) {
        Store(scalar_registers[i], values, s64(inst->Id()) * kValueStride + cached_parts[i] * 8); ++value_stats.stores;
    }
    for (size_t i = 0; i < cached_vectors.size(); ++i) if (cached_vectors[i] && wanted(cached_vectors[i])) SpillVector(i);
    DiscardValues();
}

}  // namespace swift::runtime::backend::riscv64
