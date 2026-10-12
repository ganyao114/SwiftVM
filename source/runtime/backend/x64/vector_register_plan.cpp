#include "vector_register_plan.h"
#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::x64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
using Set = std::set<u32>;
struct BudgetExceeded {};
class Meter {
public:
    Meter(const VectorRegisterOptions& options, VectorAnalysisStats& stats) : options(options), stats(stats) {}
    void Nodes(u64 amount) { Charge(stats.nodes, amount, options.max_analysis_nodes, VectorAnalysisLimit::Nodes); }
    void Work(u64 amount = 1) { Charge(stats.work, amount, options.max_analysis_work, VectorAnalysisLimit::Work); }
    void Pair() { Charge(stats.pair_work, 1, options.max_pair_work, VectorAnalysisLimit::PairWork); }
    void Storage() { Charge(stats.interference_storage_entries, 2, options.max_interference_storage_entries, VectorAnalysisLimit::InterferenceStorage); }
private:
    void Charge(u64& used, u64 amount, u64 limit, VectorAnalysisLimit kind) {
        // Check before allocation/iteration; subtraction also prevents wrap.
        if (used > limit || amount > limit - used) { stats.exhausted = kind; throw BudgetExceeded{}; }
        used += amount;
    }
    const VectorRegisterOptions& options;
    VectorAnalysisStats& stats;
};
bool Vector(T type) { return type >= T::V8 && type <= T::V128; }
bool ValidBudget(const std::vector<unsigned>& registers) {
    unsigned mask{};
    for (const auto reg : registers) {
        if (reg < 8 || reg > 15 || (mask & (1u << reg))) return false;
        mask |= 1u << reg;
    }
    return true;
}
bool Binary(O op) {
    return op == O::Vec4And || op == O::VecAnd || op == O::Vec4Or || op == O::VecOr ||
           op == O::VecXor || op == O::VecAndNot || op == O::Vec4Add || op == O::VecAdd ||
           op == O::Vec4Sub || op == O::VecSub;
}
bool Cacheable(const ir::Inst* inst) {
    if (!Vector(inst->ReturnType())) return false;
    if (Binary(inst->GetOp())) return true;
    switch (inst->GetOp()) {
        case O::AddPhi: case O::Zero: case O::VecSharedZero: case O::BitCast: case O::GetResult:
            return true;
        case O::LoadUniform:
            return inst->ArgAt(0).GetType() == ir::ArgType::Uniform &&
                   ir::GetValueSizeByte(inst->GetArg<ir::Uniform>(0).GetType()) == 16;
        default: return false;
    }
}
bool AuditedRead(const ir::Inst* inst, u32 physical_slot) {
    if (Binary(inst->GetOp())) return physical_slot < 2;
    switch (inst->GetOp()) {
        case O::BitCast: case O::GetResult: return physical_slot == 0;
        case O::StoreUniform:
            return physical_slot == 1 && inst->ArgAt(1).IsValue() &&
                   ir::GetValueSizeByte(inst->GetArg<ir::Value>(1).Type()) == 16;
        default: return false;
    }
}
template<class Visitor> void Inputs(ir::Inst* inst, const Visitor& visit, Meter& meter) {
    for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
        meter.Work();
        auto& arg = inst->ArgAt(slot);
        if (arg.IsValue()) visit(arg.Get<ir::Value>(), slot);
        else if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue()) visit(arg.Get<ir::Lambda>().GetValue(), slot);
        else if (arg.IsParams())
            for (const auto& param : arg.Get<ir::Params>()) { meter.Work(); if (param.data.IsValue()) visit(param.data.value, slot); }
    }
}
template<class Visitor> void TerminalInputs(const ir::Terminal& term, const Visitor& visit, Meter& meter, unsigned depth = 0) {
    meter.Work();
    if (depth > 128) throw std::runtime_error("vector CFG terminal depth");
    VisitVariant<void>(term, [&](const auto& item) {
        using U = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<U, ir::terminal::If>) {
            visit(item.cond); TerminalInputs(item.then_, visit, meter, depth + 1); TerminalInputs(item.else_, visit, meter, depth + 1);
        } else if constexpr (std::is_same_v<U, ir::terminal::Condition>) {
            TerminalInputs(item.then_, visit, meter, depth + 1); TerminalInputs(item.else_, visit, meter, depth + 1);
        } else if constexpr (std::is_same_v<U, ir::terminal::Switch>) {
            visit(item.value); for (const auto& branch : item.cases) TerminalInputs(branch.then, visit, meter, depth + 1);
        } else if constexpr (std::is_same_v<U, ir::terminal::CheckHalt>) TerminalInputs(item.else_, visit, meter, depth + 1);
    });
}
} // namespace

VectorRegisterPlan VectorRegisterPlan::Build(const CfgRegisterPlan& graph,
                                            const backend::FunctionPlan* function,
                                            VectorRegisterOptions options) {
    VectorRegisterPlan result;
    if (!graph.Enabled() ||
        (function && function->body && function->body.get() != graph.Body())) {
        result.rejection = VectorPlanRejection::InvalidGraph; return result;
    }
    if (!ValidBudget(options.physical_registers)) {
        result.rejection = VectorPlanRejection::InvalidRegisterBudget; return result;
    }
    Meter meter(options, result.analysis_stats);
    try {
        // Count all sized storage before allocating it, including vector-empty CFGs.
        meter.Nodes(graph.Instructions().size()); meter.Nodes(graph.Blocks().size()); meter.Nodes(graph.Edges().size());
        meter.Work(graph.Instructions().size());
        if (!graph.MatchesBody(graph.Body())) {
            result.rejection = VectorPlanRejection::InvalidGraph; return result;
        }
        result.physical_registers = std::move(options.physical_registers);
        const auto& instructions = graph.Instructions(); const auto& topology = graph.Blocks();
        const u32 count = instructions.size(), nodes = topology.size();
        std::vector<u32> node(count);
        for (u32 i = 0; i < count; ++i) {
            meter.Work();
            auto* inst = instructions[i];
            if (Vector(inst->ReturnType())) {
                result.indices.emplace(inst, result.allocations.size());
                result.allocations.push_back({inst, std::nullopt, 0, Cacheable(inst)});
            }
        }
        result.interference.resize(result.allocations.size());
        std::vector<Set> phi_defs(nodes), defs(nodes), uses(nodes), live_in(nodes), live_out(nodes), terminal_uses(nodes),
            inst_uses(count), edge_uses(graph.Edges().size());
        std::vector<std::optional<u32>> definitions(count);
        for (u32 b = 0; b < nodes; ++b) {
            meter.Work();
            const auto& source = topology[b]; result.blocks.push_back({source.begin, source.end, source.reachable});
            for (u32 i = source.begin; i < source.end; ++i) { meter.Work(); node[i] = b; }
        }
        std::vector<bool> reachable_def(result.allocations.size());
        const auto use = [&](ir::Value value, Set& into, bool audited) {
            meter.Work();
            const auto found = result.indices.find(value.Def());
            if (found == result.indices.end()) return;
            into.insert(found->second);
            auto& reads = result.allocations[found->second].audited_reads;
            if (audited && reads != std::numeric_limits<u32>::max()) ++reads;
        };
        for (u32 i = 0; i < count; ++i) {
            meter.Work();
            auto* inst = instructions[i]; const u32 b = node[i];
            if (auto found = result.indices.find(inst); found != result.indices.end()) {
                definitions[i] = found->second;
                reachable_def[found->second] = topology[b].reachable;
                if (inst->GetOp() == O::AddPhi) { meter.Work(); phi_defs[b].insert(found->second); }
            }
            const bool replaced = function && (function->exits.contains(inst) || function->halt_checks.contains(inst));
            if (!replaced && inst->GetOp() != O::AddPhi && inst->GetOp() != O::BindLabel) {
                Inputs(inst, [&](ir::Value value, u32 slot) { use(value, inst_uses[i], topology[b].reachable && AuditedRead(inst, slot)); }, meter);
                // Match scalar validation's late metadata use contract. Homes
                // remain retained; conservatively keep every producer input live.
                if (inst->GetOp() == O::SaveFlags || inst->GetOp() == O::BranchOnlyFlags || inst->GetOp() == O::GetFlags) {
                    Inputs(inst->GetArg<ir::Value>(0).Def(), [&](ir::Value value, u32) { use(value, inst_uses[i], false); }, meter);
                }
            }
            if (function && function->exits.contains(inst))
                TerminalInputs(function->exits.at(inst), [&](ir::Value value) { use(value, inst_uses[i], false); }, meter);
        }
        // Body terminal executes after the last definition/publication, unlike
        // instruction operands captured before their definition. Keep it separate.
        if (count) TerminalInputs(graph.Body()->GetTerminal(), [&](ir::Value value) { use(value, terminal_uses.back(), false); }, meter);
        for (u32 b = 0; b < nodes; ++b) {
            meter.Work(1 + phi_defs[b].size()); defs[b] = phi_defs[b];
            for (u32 i = topology[b].begin; i < topology[b].end; ++i) {
                meter.Work();
                for (const auto value : inst_uses[i]) { meter.Work(); if (!defs[b].contains(value)) uses[b].insert(value); }
                if (definitions[i]) { meter.Work(); defs[b].insert(*definitions[i]); }
            }
        }
        for (u32 e = 0; e < graph.Edges().size(); ++e) {
            meter.Work();
            const auto& edge = graph.Edges()[e];
            result.edges.push_back({edge.from, edge.to, edge.host_entry, edge.taken, edge.source});
            if (!topology[edge.to].reachable || (!edge.host_entry && !topology[edge.from].reachable)) continue;
            for (const auto& copy : edge.copies) {
                meter.Work(); if (copy.source.IsValue()) use(copy.source.value, edge_uses[e], Vector(copy.destination->ReturnType()));
            }
        }
        result.analysis_stats.work_before_fixed_point = result.analysis_stats.work;
        bool changed;
        do {
            changed = false; meter.Work(); ++result.analysis_stats.fixed_point_passes;
            for (u32 b = nodes; b-- > 0;) {
                meter.Work(); if (!topology[b].reachable) continue;
                meter.Work(terminal_uses[b].size()); Set out = terminal_uses[b];
                for (const auto e : topology[b].outgoing_edges) {
                    meter.Work(); const auto& edge = graph.Edges()[e];
                    for (const auto value : live_in[edge.to]) { meter.Work(); if (!phi_defs[edge.to].contains(value)) out.insert(value); }
                    meter.Work(edge_uses[e].size()); out.insert(edge_uses[e].begin(), edge_uses[e].end());
                }
                meter.Work(uses[b].size()); Set in = uses[b];
                for (const auto value : out) { meter.Work(); if (!defs[b].contains(value)) in.insert(value); }
                // Bound comparisons too, including passes which make no progress.
                meter.Work(in.size() + live_in[b].size() + out.size() + live_out[b].size());
                if (in != live_in[b] || out != live_out[b]) { live_in[b] = std::move(in); live_out[b] = std::move(out); changed = true; }
            }
        } while (changed);
        const auto vector = [&](const Set& values) {
            meter.Work(values.size()); std::vector<ir::Inst*> output; for (const auto value : values) output.push_back(result.allocations[value].producer); return output;
        };
        const auto link = [&](u32 left, u32 right) {
            meter.Pair();
            if (left != right && !result.interference[left].contains(right)) {
                meter.Storage(); // Charge both adjacency entries before either insert.
                result.interference[left].insert(right); result.interference[right].insert(left);
            }
        };
        const auto clique = [&](const Set& values) {
            // Iterate only actual unordered pairs; repeated attempts still count.
            meter.Work(values.size());
            for (auto a = values.begin(); a != values.end(); ++a)
                for (auto b = std::next(a); b != values.end(); ++b) link(*a, *b);
        };
        for (u32 b = 0; b < nodes; ++b) {
            meter.Work();
            result.blocks[b].live_in = vector(live_in[b]); result.blocks[b].live_out = vector(live_out[b]);
            if (!topology[b].reachable) continue;
            meter.Work(live_out[b].size()); Set live = live_out[b]; clique(live);
            for (u32 i = topology[b].end; i-- > topology[b].begin;) {
                meter.Work();
                if (definitions[i]) {
                    for (const auto other : live) link(*definitions[i], other);
                    meter.Work(); live.erase(*definitions[i]);
                }
                meter.Work(inst_uses[i].size()); live.insert(inst_uses[i].begin(), inst_uses[i].end()); clique(live);
            }
        }
        for (u32 e = 0; e < graph.Edges().size(); ++e) {
            meter.Work(); const auto& edge = graph.Edges()[e]; Set live;
            if (topology[edge.to].reachable && (edge.host_entry || topology[edge.from].reachable)) {
                for (const auto value : live_in[edge.to]) { meter.Work(); if (!phi_defs[edge.to].contains(value)) live.insert(value); }
                meter.Work(edge_uses[e].size()); live.insert(edge_uses[e].begin(), edge_uses[e].end()); clique(live);
            }
            result.edges[e].live = vector(live);
        }
        const u32 values = result.allocations.size();
        std::vector<bool> pending(values);
        std::vector<u32> degree(values), stack;
        u32 remaining{};
        if (!result.physical_registers.empty()) for (u32 i = 0; i < values; ++i) {
            meter.Work(); pending[i] = reachable_def[i] && result.allocations[i].cacheable && result.allocations[i].audited_reads;
            remaining += pending[i];
        }
        for (u32 i = 0; i < values; ++i) { meter.Work(); if (pending[i]) for (const auto neighbor : result.interference[i]) { meter.Work(); degree[i] += pending[neighbor]; } }
        while (remaining) {
            std::optional<u32> chosen;
            for (u32 i = 0; i < values; ++i) { meter.Work(); if (pending[i] && degree[i] < result.physical_registers.size()) { chosen = i; break; } }
            if (!chosen) for (u32 i = 0; i < values; ++i) {
                meter.Work(); if (!pending[i]) continue;
                if (!chosen || u64{result.allocations[i].audited_reads} * (u64{degree[*chosen]} + 1) <
                               u64{result.allocations[*chosen].audited_reads} * (u64{degree[i]} + 1)) chosen = i;
            }
            const auto removed = *chosen; pending[removed] = false; --remaining; stack.push_back(removed);
            for (const auto neighbor : result.interference[removed]) { meter.Work(); if (pending[neighbor]) --degree[neighbor]; }
        }
        while (!stack.empty()) {
            meter.Work(); const auto value = stack.back(); stack.pop_back(); Set unavailable;
            for (const auto neighbor : result.interference[value]) { meter.Work(); if (result.allocations[neighbor].slot) unavailable.insert(*result.allocations[neighbor].slot); }
            for (unsigned slot = 0; slot < result.physical_registers.size(); ++slot) {
                meter.Work(); if (!unavailable.contains(slot)) { result.allocations[value].slot = slot; break; }
            }
        }
        Set used;
        for (const auto& allocation : result.allocations) { meter.Work(); if (allocation.slot) used.insert(result.physical_registers[*allocation.slot]); }
        result.used_registers.assign(used.begin(), used.end()); result.enabled = true;
        meter.Work(result.allocations.size() + result.analysis_stats.interference_storage_entries + result.used_registers.size());
        if (!result.Validate()) {
            VectorRegisterPlan rejected; rejected.rejection = VectorPlanRejection::InvalidAnalysis;
            rejected.analysis_stats = result.analysis_stats; return rejected;
        }
        result.rejection = VectorPlanRejection::None;
        return result;
    } catch (const BudgetExceeded&) {
        // Discard every partial color/map/edge. No IR or other plans changed.
        VectorRegisterPlan rejected; rejected.rejection = VectorPlanRejection::AnalysisBudget;
        rejected.analysis_stats = result.analysis_stats; return rejected;
    }
}
std::optional<unsigned> VectorRegisterPlan::Register(const ir::Inst* producer) const noexcept {
    if (!enabled) return std::nullopt;
    const auto found = indices.find(producer);
    if (found == indices.end() || !allocations[found->second].slot) return std::nullopt;
    return physical_registers[*allocations[found->second].slot];
}
bool VectorRegisterPlan::Interferes(const ir::Inst* left, const ir::Inst* right) const noexcept {
    const auto a = indices.find(left), b = indices.find(right);
    return enabled && a != indices.end() && b != indices.end() && interference[a->second].contains(b->second);
}
bool VectorRegisterPlan::Validate() const noexcept {
    if (!enabled || !ValidBudget(physical_registers) || interference.size() != allocations.size() ||
        indices.size() != allocations.size()) return false;
    unsigned used{};
    for (u32 i = 0; i < allocations.size(); ++i) {
        const auto found = indices.find(allocations[i].producer);
        if (!allocations[i].producer || found == indices.end() || found->second != i) return false;
        if (allocations[i].slot) {
            if (!allocations[i].cacheable || !allocations[i].audited_reads || *allocations[i].slot >= physical_registers.size()) return false;
            used |= 1u << physical_registers[*allocations[i].slot];
        }
        for (const auto neighbor : interference[i]) {
            if (neighbor >= allocations.size() || neighbor == i || !interference[neighbor].contains(i)) return false;
            if (allocations[i].slot && allocations[neighbor].slot == allocations[i].slot) return false;
        }
    }
    unsigned listed{};
    for (const auto reg : used_registers) {
        if (reg < 8 || reg > 15 || (listed & (1u << reg))) return false;
        listed |= 1u << reg;
    }
    return used == listed;
}
} // namespace swift::runtime::backend::x64
