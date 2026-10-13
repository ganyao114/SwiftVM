#include "register_plan.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace swift::runtime::backend::loongarch64 {
namespace {
using O = ir::OpCode;
constexpr u32 kNoNode = std::numeric_limits<u32>::max();

void Merge(LiveSet& destination, const LiveSet& source) {
    destination.insert(source.begin(), source.end());
}

bool IsBranch(ir::Inst* inst) { return inst->GetOp() == O::Goto || inst->GetOp() == O::NotGoto; }

bool EarlyResult(ir::Inst* inst) {
    const auto op = inst->GetOp();
    return op == O::VecSatMulQC || op == O::VecSatShiftQC ||
           op == O::Div128Remainder || op == O::CpuidUpper;
}

struct Node {
    u32 first{}, end{};
    LiveSet uses, defs, in, out;
    std::vector<u32> incoming, outgoing;
};

struct Edge {
    u32 from{}, to{};
    bool taken{};
    LiveSet uses;
};

}  // namespace

RegisterPlan RegisterPlan::Build(ir::Block* block, const Targets& targets, const Stops& stops,
                                 std::span<const PhiEdge> phi_edges, const Exits& exits,
                                 const ir::Terminal* terminal) {
    if (!block) throw std::invalid_argument("LoongArch register plan needs an IR block");
    RegisterPlan result;
    std::vector<ir::Inst*> insts;
    std::unordered_map<ir::Inst*, u32> bound;
    for (auto& inst : block->GetInstList()) {
        result.positions.emplace(&inst, insts.size());
        insts.push_back(&inst);
        if (inst.GetOp() == O::BindLabel && !bound.emplace(inst.GetArg<ir::Value>(0).Def(), insts.size() - 1).second)
            throw std::runtime_error("LoongArch register plan has a duplicate label");
    }
    const u32 count = insts.size();
    result.stats.instructions = count;
    if (!count) return result;
    const auto position = [&](ir::Inst* inst) {
        const auto found = result.positions.find(inst);
        if (found == result.positions.end()) throw std::runtime_error("LoongArch register plan has a foreign instruction");
        return found->second;
    };
    const auto mark = [&](LiveSet& set, ir::Value value) {
        (void)position(value.Def());
        set.insert(value.Def());
    };
    const auto operands = [&](ir::Inst* inst, LiveSet& set) {
        for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
            auto& arg = inst->ArgAt(slot);
            if (arg.IsValue()) mark(set, arg.Get<ir::Value>());
            else if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue()) mark(set, arg.Get<ir::Lambda>().GetValue());
            else if (arg.IsParams()) for (const auto& param : arg.Get<ir::Params>())
                if (param.data.IsValue()) mark(set, param.data.value);
        }
    };
    const auto terminal_uses = [&](const auto& recurse, const ir::Terminal& value, LiveSet& set) -> void {
        VisitVariant<void>(value, [&](const auto& term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::If>) {
                mark(set, term.cond); recurse(recurse, term.then_, set); recurse(recurse, term.else_, set);
            } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
                recurse(recurse, term.then_, set); recurse(recurse, term.else_, set);
            } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                mark(set, term.value);
                for (const auto& item : term.cases) recurse(recurse, item.then, set);
            } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) recurse(recurse, term.else_, set);
        });
    };
    const auto stops_here = [&](ir::Inst* inst) { return stops.contains(inst) || exits.contains(inst); };

    std::vector<bool> leaders(count + 1);
    leaders.front() = leaders.back() = true;
    std::unordered_map<ir::Inst*, u32> branch_targets;
    for (u32 i = 0; i < count; ++i) {
        auto* inst = insts[i];
        if (inst->GetOp() == O::BindLabel) leaders[i] = true;
        if (IsBranch(inst) || stops_here(inst)) leaders[i + 1] = true;
        if (IsBranch(inst)) {
            auto* anchor = inst;
            if (auto found = targets.find(inst); found != targets.end()) anchor = found->second;
            auto found = bound.find(anchor);
            const u32 target = found != bound.end() ? found->second :
                anchor && anchor->GetOp() == O::BindLabel ? position(anchor) : kNoNode;
            if (target == kNoNode) throw std::runtime_error("LoongArch register plan has an unbound branch");
            branch_targets.emplace(inst, target);
            leaders[target] = true;
        }
    }
    std::vector<Node> nodes;
    std::vector<u32> node_at(count);
    for (u32 i = 0; i < count; ++i) {
        if (leaders[i]) {
            if (!nodes.empty()) nodes.back().end = i;
            nodes.emplace_back(); nodes.back().first = i;
        }
        node_at[i] = nodes.size() - 1;
    }
    nodes.back().end = count;
    result.stats.nodes = nodes.size();
    std::vector<Edge> edges;
    const auto edge = [&](u32 from, u32 to, bool taken) {
        const u32 index = edges.size();
        edges.emplace_back();
        edges.back().from = from; edges.back().to = to; edges.back().taken = taken;
        nodes[from].outgoing.push_back(index);
        if (to != kNoNode) nodes[to].incoming.push_back(index);
        return index;
    };
    // This source-order traversal matches ConfigurePhis: fallthrough precedes
    // taken when the same predecessor has two edges to one target.
    for (u32 b = 0; b < nodes.size(); ++b) {
        auto* last = insts[nodes[b].end - 1];
        if (!stops_here(last)) {
            const u32 e = edge(b, b + 1 < nodes.size() ? b + 1 : kNoNode, false);
            if (edges[e].to == kNoNode)
                terminal_uses(terminal_uses, terminal ? *terminal : block->GetTerminal(), edges[e].uses);
        }
        if (IsBranch(last)) edge(b, node_at[branch_targets.at(last)], true);
    }

    std::vector<LiveSet> uses(count), defs(count);
    for (u32 i = 0; i < count; ++i) {
        auto* inst = insts[i];
        if (inst->GetOp() != O::BindLabel && inst->GetOp() != O::AddPhi && !EarlyResult(inst)) operands(inst, uses[i]);
        if (inst->GetOp() == O::SaveFlags || inst->GetOp() == O::BranchOnlyFlags || inst->GetOp() == O::GetFlags)
            operands(inst->GetArg<ir::Value>(0).Def(), uses[i]);
        if (auto found = exits.find(inst); found != exits.end()) terminal_uses(terminal_uses, found->second, uses[i]);
        // Pair results and QC values are written by the producer, even when
        // their no-op pseudo marker follows a consumer or a control-flow edge.
        if (EarlyResult(inst)) defs[position(inst->GetArg<ir::Value>(0).Def())].insert(inst);
        else if (inst->GetOp() != O::AddPhi && inst->ReturnType() != ir::ValueType::VOID) defs[i].insert(inst);
    }
    for (u32 b = 0; b < nodes.size(); ++b) {
        auto& node = nodes[b];
        u32 first = node.first;
        if (insts[first]->GetOp() == O::BindLabel) ++first;
        u32 end = first;
        while (end < node.end && insts[end]->GetOp() == O::AddPhi) ++end;
        for (u32 i = end; i < node.end; ++i) if (insts[i]->GetOp() == O::AddPhi)
            throw std::runtime_error("LoongArch register plan phi must be at a CFG entry");
        for (u32 i = first; i < end; ++i) {
            // All phi destinations are assigned together on entry. Their
            // marker sequence must not kill an already produced destination
            // after the first label, including cyclic parallel copies.
            defs[node.first].insert(insts[i]);
            const auto params = insts[i]->GetArg<ir::Params>(0);
            const u32 host_entry = b == 0 ? 1 : 0;
            if (u32(std::distance(params.begin(), params.end())) != node.incoming.size() + host_entry)
                throw std::runtime_error("LoongArch register plan phi input count differs from incoming edges");
            u32 input{};
            for (const auto& param : params) {
                if (!param.data.IsValue() && !param.data.IsImm())
                    throw std::runtime_error("LoongArch register plan has an invalid phi input");
                if (input++ < host_entry) {
                    if (param.data.IsValue()) throw std::runtime_error("LoongArch register plan entry phi needs an immediate");
                } else if (param.data.IsValue()) mark(edges[node.incoming[input - host_entry - 1]].uses, param.data.value);
            }
        }
    }
    for (const auto& phi : phi_edges) {
        if (!phi.from) {
            if (phi.source.Defined()) throw std::runtime_error("LoongArch register plan host phi edge must be constant");
            continue;
        }
        const u32 from = node_at[position(phi.from)];
        auto to = bound.find(phi.to);
        const u32 target = to != bound.end() ? to->second : position(phi.to);
        bool matched{};
        for (const auto e : nodes[from].outgoing) if (edges[e].to == node_at[target]) {
            // Identical from/to coordinates cannot distinguish duplicate
            // branch/fallthrough edges; unioning sources is conservative.
            mark(edges[e].uses, phi.source); matched = true;
        }
        if (!matched || insts[nodes[from].end - 1] != phi.from)
            throw std::runtime_error("LoongArch register plan phi edge is not a CFG edge");
    }
    for (u32 b = 0; b < nodes.size(); ++b) {
        auto& node = nodes[b];
        for (u32 i = node.first; i < node.end; ++i) {
            for (auto* value : uses[i]) if (!node.defs.contains(value)) node.uses.insert(value);
            Merge(node.defs, defs[i]);
            for (auto* value : uses[i]) result.uses[value].push_back(i);
        }
        for (const auto e : node.outgoing)
            for (auto* value : edges[e].uses) result.uses[value].push_back(node.end - 1);
    }

    bool changed;
    do {
        changed = false; ++result.stats.iterations;
        for (u32 b = nodes.size(); b-- > 0;) {
            auto& node = nodes[b];
            LiveSet next_out;
            for (const auto e : node.outgoing) {
                Merge(next_out, edges[e].uses);
                if (edges[e].to != kNoNode) Merge(next_out, nodes[edges[e].to].in);
            }
            LiveSet next_in = node.uses;
            for (auto* value : next_out) if (!node.defs.contains(value)) next_in.insert(value);
            if (next_in != node.in || next_out != node.out) {
                node.in = std::move(next_in); node.out = std::move(next_out); changed = true;
            }
        }
    } while (changed);
    for (const auto& node : nodes) {
        LiveSet live = node.out;
        for (u32 i = node.end; i-- > node.first;) {
            result.after.emplace(insts[i], live);
            result.taken.try_emplace(insts[i]);
            result.fallthrough.emplace(insts[i], i + 1 < node.end ? live : LiveSet{});
            for (auto* value : defs[i]) live.erase(value);
            Merge(live, uses[i]);
            result.before.emplace(insts[i], live);
            result.stats.max_live = std::max<u32>(result.stats.max_live, std::max(live.size(), result.after.at(insts[i]).size()));
        }
        auto* last = insts[node.end - 1];
        for (const auto e : node.outgoing) {
            LiveSet edge_live = edges[e].uses;
            if (edges[e].to != kNoNode) Merge(edge_live, nodes[edges[e].to].in);
            if (edges[e].taken) Merge(result.taken.at(last), edge_live);
            else Merge(result.fallthrough.at(last), edge_live);
        }
    }
    for (auto& [value, positions] : result.uses) {
        (void)value;
        std::sort(positions.begin(), positions.end());
        positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
    }
    return result;
}

u32 RegisterPlan::NextUse(ir::Inst* value, ir::Inst* from) const {
    const auto current = Position(from);
    const auto found = uses.find(value);
    if (found == uses.end()) return kNoNextUse;
    const auto next = std::lower_bound(found->second.begin(), found->second.end(), current);
    return next == found->second.end() ? kNoNextUse : *next;
}

}  // namespace swift::runtime::backend::loongarch64
