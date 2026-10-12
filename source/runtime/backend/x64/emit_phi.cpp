#include "translator.h"

#include <algorithm>
#include <stdexcept>
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::x64 {
using O = ir::OpCode;
using namespace Xbyak;

ir::Inst* Translator::BranchTarget(ir::Inst* branch) const {
    if (function_plan) if (auto found = function_plan->targets.find(branch); found != function_plan->targets.end())
        return found->second->GetArg<ir::Value>(0).Def();
    return branch;
}
bool Translator::FallsThrough(ir::Inst* inst) const {
    return !function_plan || !function_plan->stops.contains(inst);
}
void Translator::CommitGuestEdge(ir::Inst* branch) {
    if (!function_plan) return;
    for (const auto& [label, location] : function_plan->locations) {
        if (label->GetArg<ir::Value>(0).Def() != BranchTarget(branch)) continue;
        const bool dirty = location_dirty;
        PublishLocation();
        // This publication exists only on the taken edge. Lexical emission
        // of an alternative path must still publish its own source PC.
        location_dirty = dirty;
        mov(rax, qword[r12 + state_offset_current_loc]);
        mov(qword[r12 + state_offset_prev_loc], rax);
        mov(rax, location.Value());
        mov(qword[r12 + state_offset_current_loc], rax);
        return;
    }
}

void Translator::ConfigurePhis() {
    const auto valid_phi_type = [](ir::ValueType type) {
        return type >= ir::ValueType::U8 && type <= ir::ValueType::V128;
    };
    struct Edge { ir::Inst* source; bool taken; u32 at; };
    std::vector<ir::Inst*> insts;
    std::unordered_map<ir::Inst*, u32> positions, targets;
    bool has_phi{};
    for (auto& inst : block->GetInstList()) {
        positions.emplace(&inst, insts.size()); insts.push_back(&inst);
        has_phi |= inst.GetOp() == O::AddPhi;
    }
    if (function_plan) {
        if (function_plan->body && function_plan->body.get() != block)
            throw std::runtime_error("x64 flow metadata belongs to another body");
        const auto local = [&](ir::Inst* inst) {
            if (!positions.contains(inst)) throw std::runtime_error("x64 flow metadata references another block");
        };
        for (const auto& [branch, target] : function_plan->targets) {
            local(branch); local(target);
            if ((branch->GetOp() != O::Goto && branch->GetOp() != O::NotGoto) || target->GetOp() != O::BindLabel)
                throw std::runtime_error("x64 flow target requires a branch and BindLabel");
        }
        for (auto* stop : function_plan->stops) {
            local(stop);
            if (stop->GetOp() != O::Goto && stop->GetOp() != O::NotGoto && !function_plan->exits.contains(stop))
                throw std::runtime_error("x64 flow stop must be a branch or terminal exit");
        }
        for (const auto& [exit, terminal] : function_plan->exits) {
            (void)terminal; local(exit);
            if (!function_plan->stops.contains(exit)) throw std::runtime_error("x64 terminal exit must stop fallthrough");
        }
        for (const auto& [label, location] : function_plan->locations) {
            local(label);
            if (label->GetOp() != O::BindLabel || location == ir::Location::INVALID)
                throw std::runtime_error("x64 guest entry requires a valid BindLabel location");
        }
        for (auto* label : function_plan->synthetic_labels) {
            local(label);
            if (label->GetOp() != O::BindLabel || function_plan->locations.contains(label))
                throw std::runtime_error("x64 synthetic label cannot be a guest entry");
        }
        for (auto* check : function_plan->halt_checks) local(check);
    }
    if (insts.empty()) return;
    const u32 count = insts.size();
    std::vector<bool> leaders(count + 1);
    leaders[0] = leaders[count] = true;
    for (u32 i = 0; i < count; ++i) {
        auto* inst = insts[i];
        if (inst->GetOp() == O::Goto || inst->GetOp() == O::NotGoto || !FallsThrough(inst)) leaders[i + 1] = true;
        if (inst->GetOp() == O::BindLabel) {
            if (!inst->ArgAt(0).IsValue() || !inst->GetArg<ir::Value>(0).Defined())
                throw std::runtime_error("x64 BindLabel requires a defined identity");
            const auto identity = inst->GetArg<ir::Value>(0).Def();
            if (!positions.contains(identity) || !targets.emplace(identity, i).second)
                throw std::runtime_error("x64 goto has foreign or multiply bound target");
            labels.try_emplace(identity);
            leaders[i] = true;
        }
    }
    for (auto* inst : insts) if (inst->GetOp() == O::Goto || inst->GetOp() == O::NotGoto) {
        if (!targets.contains(BranchTarget(inst))) throw std::runtime_error("x64 goto has no bound target");
        if (!inst->ArgAt(0).IsValue()) throw std::runtime_error("x64 goto requires a scalar condition");
        const auto condition = inst->GetArg<ir::Value>(0);
        if (!condition.Defined() || !positions.contains(condition.Def()) ||
            !valid_phi_type(condition.Type()) || ir::GetValueSizeByte(condition.Type()) > 8)
            throw std::runtime_error("x64 goto requires a local scalar condition");
    }
    std::vector<u32> starts, node(count);
    for (u32 i = 0; i < count; ++i) {
        if (leaders[i]) starts.push_back(i);
        node[i] = starts.size() - 1;
    }
    const u32 nodes = starts.size(); starts.push_back(count);
    std::vector<std::vector<Edge>> incoming(nodes);
    std::vector<std::vector<u32>> successors(nodes), predecessors(nodes);
    incoming[0].push_back({nullptr, false, 0});
    const auto add_edge = [&](u32 from, u32 to, ir::Inst* last, bool taken, u32 at) {
        successors[from].push_back(to); predecessors[to].push_back(from);
        incoming[to].push_back({last, taken, at});
    };
    // Incoming parameters follow source instruction order, fallthrough before
    // taken if the same predecessor has both edges to the same destination.
    for (u32 b = 0; b < nodes; ++b) {
        auto* last = insts[starts[b + 1] - 1];
        if (b + 1 < nodes && FallsThrough(last)) add_edge(b, b + 1, last, false, starts[b + 1]);
        // Translate replaces exit markers with their terminal, so the old
        // branch identity contributes no executable taken edge.
        if ((!function_plan || !function_plan->exits.contains(last)) &&
            (last->GetOp() == O::Goto || last->GetOp() == O::NotGoto))
            add_edge(b, node[targets.at(BranchTarget(last))], last, true, starts[b + 1]);
    }
    std::vector<std::pair<ir::Value, u32>> edge_uses;
    for (u32 b = 0; b < nodes; ++b) {
        u32 first = starts[b];
        if (insts[first]->GetOp() == O::BindLabel) ++first;
        u32 end = first;
        while (end < starts[b + 1] && insts[end]->GetOp() == O::AddPhi) ++end;
        for (u32 i = end; i < starts[b + 1]; ++i)
            if (insts[i]->GetOp() == O::AddPhi) throw std::runtime_error("x64 phi must precede non-phi instructions at CFG entry");
        max_phi_moves = std::max(max_phi_moves, end - first);
        for (u32 i = first; i < end; ++i) {
            auto* phi = insts[i];
            if (!valid_phi_type(phi->ReturnType())) throw std::runtime_error("invalid x64 phi width or parameters");
            const auto size = ir::GetValueSizeByte(phi->ReturnType());
            if (!size || size > 16 || !phi->ArgAt(0).IsParams()) throw std::runtime_error("invalid x64 phi width or parameters");
            std::vector<ir::DataClass> inputs;
            for (const auto& param : phi->GetArg<ir::Params>(0)) {
                if (param.data.IsValue()) {
                    const auto value = param.data.value;
                    if (!value.Defined() || !positions.contains(value.Def()) ||
                        !valid_phi_type(value.Type()) || !valid_phi_type(value.Def()->ReturnType()) ||
                        ir::GetValueSizeByte(value.Type()) != size)
                        throw std::runtime_error("x64 phi input requires matching width and local defined producer");
                } else if (!param.data.IsImm()) throw std::runtime_error("invalid x64 phi input");
                inputs.push_back(param.data);
            }
            if (inputs.size() != incoming[b].size()) throw std::runtime_error("x64 phi input count differs from incoming edges");
            for (u32 p = 0; p < inputs.size(); ++p) {
                const auto& edge = incoming[b][p];
                PhiMove move{phi, inputs[p]};
                if (!edge.source) {
                    if (inputs[p].IsValue()) throw std::runtime_error("x64 host-entry phi requires an immediate");
                    entry_phis.push_back(move);
                } else {
                    if (inputs[p].IsValue()) edge_uses.emplace_back(inputs[p].value, edge.at);
                    if (edge.taken) taken_phis[edge.source].push_back(move);
                    else fallthrough_phis[insts[starts[b]]].push_back(move);
                }
            }
        }
    }
    // Ordinary blocks retain their existing non-SSA local control behavior.
    // Phi/function CFGs must be valid SSA, not uninitialized stack reads.
    if (!has_phi && !function_plan) return;
    constexpr u32 unknown = UINT32_MAX;
    std::vector<bool> visited(nodes);
    std::vector<u32> rpo, rank(nodes, unknown), idom(nodes, unknown);
    std::vector<std::pair<u32, size_t>> walk{{0, 0}}; visited[0] = true;
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
        for (u32 i = 1; i < rpo.size(); ++i) {
            const auto b = rpo[i]; u32 parent = unknown;
            for (auto from : predecessors[b]) if (idom[from] != unknown) parent = parent == unknown ? from : intersect(parent, from);
            if (idom[b] != parent) { idom[b] = parent; changed = true; }
        }
    } while (changed);
    const auto use = [&](ir::Value value, u32 at, bool edge = false) {
        if (!value.Defined() || !positions.contains(value.Def())) throw std::runtime_error("x64 CFG has foreign SSA input");
        const u32 to = node[edge && at ? at - 1 : std::min(at, count - 1)];
        if (!visited[to]) return;
        auto* definition = value.Def();
        if (definition->GetOp() == O::CpuidUpper || definition->GetOp() == O::Div128Remainder ||
            definition->GetOp() == O::VecSatMulQC || definition->GetOp() == O::VecSatShiftQC)
            definition = definition->GetArg<ir::Value>(0).Def();
        if (!positions.contains(definition)) throw std::runtime_error("x64 CFG pseudo has foreign producer");
        const auto where = positions.at(definition), from = node[where];
        bool dominates = visited[from];
        if (from == to) dominates &= where < at;
        else { auto ancestor = to; while (ancestor != from && ancestor != 0) ancestor = idom[ancestor]; dominates &= ancestor == from; }
        if (!dominates) throw std::runtime_error(edge ? "x64 phi input does not dominate incoming edge" : "x64 SSA input does not dominate its use");
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
        // A replaced branch still passes the structural target/local scalar
        // condition checks above. Only its terminal has executable SSA uses.
        if (insts[i]->GetOp() != O::BindLabel && insts[i]->GetOp() != O::AddPhi &&
            (!function_plan || !function_plan->exits.contains(insts[i]))) operands(insts[i], i);
        if (insts[i]->GetOp() == O::SaveFlags || insts[i]->GetOp() == O::BranchOnlyFlags || insts[i]->GetOp() == O::GetFlags)
            operands(insts[i]->GetArg<ir::Value>(0).Def(), i);
    }
    for (const auto& [value, at] : edge_uses) use(value, at, true);
    const auto terminal = [&](const auto& recurse, const ir::Terminal& term, u32 at) -> void {
        VisitVariant<void>(term, [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, ir::terminal::If>) { use(value.cond, at); recurse(recurse, value.then_, at); recurse(recurse, value.else_, at); }
            else if constexpr (std::is_same_v<T, ir::terminal::Condition>) { recurse(recurse, value.then_, at); recurse(recurse, value.else_, at); }
            else if constexpr (std::is_same_v<T, ir::terminal::Switch>) { use(value.value, at); for (const auto& item : value.cases) recurse(recurse, item.then, at); }
            else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) recurse(recurse, value.else_, at);
        });
    };
    terminal(terminal, block->GetTerminal(), count);
    if (function_plan) for (const auto& [marker, term] : function_plan->exits)
        terminal(terminal, term, positions.at(marker));
}

void Translator::EmitPhiMoves(const std::vector<PhiMove>& moves) {
    CountPhiCopies(moves);
    cached_vector = nullptr;
    std::vector<bool> direct(moves.size());
    if (cfg_register_plan.Enabled() && sparse_frame_plan.Enabled()) {
        std::unordered_set<ir::Inst*> source_definitions;
        for (const auto& move : moves)
            if (move.source.IsValue()) source_definitions.insert(move.source.value.Def());
        for (u32 i = 0; i < moves.size(); ++i) {
            auto* destination = moves[i].destination;
            const auto type = destination->ReturnType();
            direct[i] = type >= ir::ValueType::U8 && type <= ir::ValueType::S64 &&
                !cfg_register_plan.Register(destination) &&
                cfg_register_plan.RequiresCanonicalHome(destination) &&
                sparse_frame_plan.Home(destination).has_value() &&
                !source_definitions.contains(destination);
        }
    }
    // Capture every remaining source before direct copies or snapshot
    // publication can overwrite it. Keep original scratch ordinals.
    for (u32 i = 0; i < moves.size(); ++i) {
        if (direct[i]) continue;
        const auto& move = moves[i];
        const auto size = ir::GetValueSizeByte(move.destination->ReturnType());
        if (move.source.IsImm()) {
            mov(rax, move.source.imm.Get());
            if (size < 8) Mask(rax, size * 8);
            mov(qword[r13 + phi_scratch + i * 16], rax);
            mov(qword[r13 + phi_scratch + i * 16 + 8], 0);
        } else if (move.destination->ReturnType() <= ir::ValueType::S64) {
            Read(rax, move.source.value);
            mov(qword[r13 + phi_scratch + i * 16], rax);
            mov(qword[r13 + phi_scratch + i * 16 + 8], 0);
        } else {
            // Vector annotations and input views select access width, not
            // container truncation. Preserve every raw byte on vector phis.
            ReadVector(xmm0, move.source.value);
            movdqu(ptr[r13 + phi_scratch + i * 16], xmm0);
        }
    }
    // A direct destination has a unique whole-body home, no assigned
    // register, and is absent from every value source (including raw views).
    // Its Write cannot change any source needed by another direct copy.
    for (u32 i = 0; i < moves.size(); ++i) {
        if (!direct[i]) continue;
        const auto& move = moves[i];
        if (move.source.IsImm()) mov(rax, move.source.imm.Get());
        else Read(rax, move.source.value);
        Write(move.destination, rax);
        ++statistics.direct_phi_copy_sites;
    }
    for (u32 i = 0; i < moves.size(); ++i) {
        if (direct[i]) continue;
        auto* destination = moves[i].destination;
        if (destination->ReturnType() <= ir::ValueType::S64 && cfg_register_plan.Enabled()) {
            mov(rax, qword[r13 + phi_scratch + i * 16]);
            Write(destination, rax);
        } else {
            movdqu(xmm0, ptr[r13 + phi_scratch + i * 16]);
            if (vector_register_plan.Enabled() && destination->ReturnType() >= ir::ValueType::V8 &&
                destination->ReturnType() <= ir::ValueType::V128) {
                WriteVector(destination, xmm0);
                if (vector_register_plan.Register(destination)) ++statistics.vector_cfg_phi_publication_sites;
            } else movdqu(ptr[r13 + Home(destination)], xmm0);
        }
    }
}
} // namespace swift::runtime::backend::x64
