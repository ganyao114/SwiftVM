#include "cfg_register_plan.h"
#include "flags_usage.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <new>
#include <set>
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::x64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
using Set = std::set<u32>;
bool Scalar(T type) { return type >= T::U8 && type <= T::S64; }
bool Container(T type) { return type >= T::U8 && type <= T::V128; }
bool PhaseBinary(O op) {
    return op == O::Add || op == O::Sub || op == O::Adc || op == O::Sbb ||
           op == O::And || op == O::Or || op == O::Xor || op == O::AndNot;
}

bool PhaseUnary(const ir::Inst* inst) {
    if (!Scalar(inst->ReturnType()) || !inst->ArgAt(0).IsValue() ||
        !Scalar(inst->GetArg<ir::Value>(0).Type())) return false;
    // Each helper-free emitter captures input 0 before publishing its output.
    // Not's Operand form and vector/raw-container lowerings are excluded.
    switch (inst->GetOp()) {
        case O::Neg:
        case O::ZeroExtend32: case O::ZeroExtend32To64: case O::ZeroExtend64: case O::SignExtend:
        case O::LslImm: case O::LsrImm: case O::AsrImm: case O::RorImm:
        case O::ByteSwap: case O::BitExtract: case O::BitClear:
        case O::TestBit: case O::TestZero: case O::TestNotZero:
            return true;
        default: return false;
    }
}

bool Cacheable(ir::Inst* inst) {
    if (!Scalar(inst->ReturnType())) return false;
    switch (inst->GetOp()) {
        case O::LoadUniform:
            return inst->ArgAt(0).GetType() == ir::ArgType::Uniform && Scalar(inst->GetArg<ir::Uniform>(0).GetType());
        case O::AddPhi: case O::LoadImm: case O::GetOperand: case O::CallLambda:
        case O::Add: case O::Sub: case O::Adc: case O::Sbb:
        case O::And: case O::Or: case O::Xor: case O::AndNot:
        case O::Neg: case O::Not: case O::Mul: case O::MulSub: case O::MulHigh:
        case O::ZeroExtend32: case O::ZeroExtend32To64: case O::ZeroExtend64: case O::SignExtend:
        case O::LslImm: case O::LslValue: case O::LsrImm: case O::LsrValue:
        case O::AsrImm: case O::AsrValue: case O::RorImm: case O::RorValue:
        case O::ByteSwap: case O::BitExtract: case O::BitInsert: case O::BitClear:
        case O::TestBit: case O::TestZero: case O::TestNotZero:
            return true;
        default: return false;
    }
}

bool AuditedRead(ir::Inst* inst, u32 slot, ir::Value value) {
    if (!Scalar(value.Type())) return false;
    switch (inst->GetOp()) {
        case O::SaveFlags: case O::BranchOnlyFlags: return slot == 0;
        case O::StoreUniform:
            return slot == 1 && inst->ArgAt(0).GetType() == ir::ArgType::Uniform && Scalar(inst->GetArg<ir::Uniform>(0).GetType());
        case O::Goto: case O::NotGoto: return slot == 0;
        case O::CallLambda: return slot >= 1 && slot <= 3;
        default: break;
    }
    if (!Scalar(inst->ReturnType())) return false;
    switch (inst->GetOp()) {
        case O::Add: case O::Sub: case O::Adc: case O::Sbb:
        case O::And: case O::Or: case O::Xor: case O::AndNot: case O::Mul:
            return slot == 0 || slot == 2 || slot == 3;
        case O::GetOperand: return slot == 1 || slot == 2;
        case O::MulSub: return slot <= 2;
        case O::MulHigh: case O::BitInsert:
        case O::LslValue: case O::LsrValue: case O::AsrValue: case O::RorValue:
            return slot <= 1;
        case O::Neg: case O::ZeroExtend32: case O::ZeroExtend32To64: case O::ZeroExtend64: case O::SignExtend:
        case O::LslImm: case O::LsrImm: case O::AsrImm: case O::RorImm:
        case O::ByteSwap: case O::BitExtract: case O::BitClear:
        case O::TestBit: case O::TestZero: case O::TestNotZero:
            return slot == 0;
        default: return false;
    }
}
bool ValidPhysicalBudget(const std::vector<unsigned>& registers) {
    std::set<unsigned> unique;
    for (auto reg : registers)
        if ((reg != 3 && reg != 5 && reg != 15) || !unique.insert(reg).second) return false;
    return true;
}

struct Invalid { CfgPlanRejection reason; };
void Require(bool condition, CfgPlanRejection reason) { if (!condition) throw Invalid{reason}; }
template <typename Visitor>
void TerminalInputs(const ir::Terminal& term, const Visitor& visit, unsigned depth = 0) {
    Require(depth <= 128, CfgPlanRejection::InvalidMetadata);
    VisitVariant<void>(term, [&](const auto& value) {
        using U = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<U, ir::terminal::If>) {
            visit(value.cond);
            TerminalInputs(value.then_, visit, depth + 1); TerminalInputs(value.else_, visit, depth + 1);
        } else if constexpr (std::is_same_v<U, ir::terminal::Condition>) {
            TerminalInputs(value.then_, visit, depth + 1); TerminalInputs(value.else_, visit, depth + 1);
        } else if constexpr (std::is_same_v<U, ir::terminal::Switch>) {
            visit(value.value);
            for (const auto& item : value.cases) TerminalInputs(item.then, visit, depth + 1);
        } else if constexpr (std::is_same_v<U, ir::terminal::CheckHalt>) TerminalInputs(value.else_, visit, depth + 1);
    });
}
}  // namespace

CfgRegisterPlan CfgRegisterPlan::Build(const ir::Block* block, const backend::FunctionPlan& function,
                                     CfgRegisterOptions options) {
    return BuildImpl(block, function, std::move(options), true);
}
CfgRegisterPlan CfgRegisterPlan::ValidateGraph(const ir::Block* block, const backend::FunctionPlan& function) {
    return BuildImpl(block, function, {.physical_registers = {}}, false);
}
CfgRegisterPlan CfgRegisterPlan::BuildImpl(const ir::Block* block, const backend::FunctionPlan& function,
                                         CfgRegisterOptions options, bool build_interference) {
    CfgRegisterPlan result;
    if (!block) return result;
    auto working = std::make_shared<Analysis>();
    auto& data = *working;
    try {
        Require(!function.body || function.body.get() == block, CfgPlanRejection::InvalidMetadata);
        data.body = block;
        Require(ValidPhysicalBudget(options.physical_registers), CfgPlanRejection::InvalidBudget);
        result.physical_registers = std::move(options.physical_registers);
        data.loop_weighted_costs = build_interference && options.loop_weighted_costs;
        data.scalar_unary_phases = build_interference && options.scalar_unary_phases;
        std::vector<ir::Inst*> insts;
        std::unordered_map<ir::Inst*, u32> positions;
        for (auto& inst : block->GetInstList()) {
            positions.emplace(&inst, insts.size()); insts.push_back(&inst);
        }
        const u32 count = insts.size();
        data.instructions = insts;
        const auto local = [&](ir::Inst* inst) { Require(positions.contains(inst), CfgPlanRejection::InvalidMetadata); };
        const auto nop_marker = [&](ir::Inst* inst) {
            Require(inst->GetOp() == O::Nop && inst->ReturnType() == T::VOID, CfgPlanRejection::InvalidMetadata);
            for (u32 slot = 0; slot < ir::Inst::max_args; ++slot)
                Require(inst->ArgAt(slot).IsVoid(), CfgPlanRejection::InvalidMetadata);
        };
        for (const auto& [branch, target] : function.targets) {
            local(branch); local(target);
            Require((branch->GetOp() == O::Goto || branch->GetOp() == O::NotGoto) && target->GetOp() == O::BindLabel,
                    CfgPlanRejection::InvalidMetadata);
        }
        for (auto* stop : function.stops) {
            local(stop);
            Require(stop->GetOp() == O::Goto || stop->GetOp() == O::NotGoto || function.exits.contains(stop),
                    CfgPlanRejection::InvalidMetadata);
        }
        for (const auto& [marker, term] : function.exits) {
            (void)term; local(marker); Require(function.stops.contains(marker), CfgPlanRejection::InvalidMetadata);
            // Translate replaces this instruction with its terminal. It must
            // neither define an SSA value nor advertise an unexecuted effect.
            // Existing branch exit markers also serve as BindLabel identities.
            Require(marker->ReturnType() == T::VOID &&
                    (marker->GetOp() == O::Nop || marker->GetOp() == O::Goto || marker->GetOp() == O::NotGoto),
                    CfgPlanRejection::InvalidMetadata);
            if (marker->GetOp() == O::Nop) nop_marker(marker);
        }
        for (const auto& [label, location] : function.locations) {
            local(label); Require(label->GetOp() == O::BindLabel && location != ir::Location::INVALID,
                                  CfgPlanRejection::InvalidMetadata);
        }
        for (auto* label : function.synthetic_labels) {
            local(label); Require(label->GetOp() == O::BindLabel && !function.locations.contains(label), CfgPlanRejection::InvalidMetadata);
        }
        for (auto* check : function.halt_checks) {
            local(check); nop_marker(check);
        }
        if (!count) {
            // There is no local SSA definition in an empty body. Walk nested
            // terminal trees too, without dereferencing a foreign selector.
            TerminalInputs(block->GetTerminal(), [](ir::Value) { Require(false, CfgPlanRejection::InvalidSSA); });
            data.has_liveness = data.has_interference = build_interference;
            result.analysis = std::move(working);
            result.rejection = CfgPlanRejection::None; return result;
        }

        std::vector<bool> leaders(count + 1);
        leaders[0] = leaders[count] = true;
        std::unordered_map<ir::Inst*, u32> bound;
        for (u32 i = 0; i < count; ++i) {
            auto* inst = insts[i];
            if (inst->GetOp() == O::Goto || inst->GetOp() == O::NotGoto || function.stops.contains(inst)) leaders[i + 1] = true;
            if (inst->GetOp() == O::BindLabel) {
                Require(inst->ArgAt(0).IsValue(), CfgPlanRejection::InvalidMetadata);
                const auto identity = inst->GetArg<ir::Value>(0).Def();
                Require(positions.contains(identity) && bound.emplace(identity, i).second, CfgPlanRejection::InvalidMetadata);
                leaders[i] = true;
            }
        }
        const auto target = [&](ir::Inst* branch) {
            if (auto found = function.targets.find(branch); found != function.targets.end())
                return found->second->GetArg<ir::Value>(0).Def();
            return branch;
        };
        for (auto* inst : insts) if (inst->GetOp() == O::Goto || inst->GetOp() == O::NotGoto) {
            Require(bound.contains(target(inst)), CfgPlanRejection::InvalidMetadata);
            Require(inst->ArgAt(0).IsValue(), CfgPlanRejection::InvalidMetadata);
            const auto condition = inst->GetArg<ir::Value>(0);
            Require(positions.contains(condition.Def()), CfgPlanRejection::InvalidSSA);
            Require(Scalar(condition.Type()), CfgPlanRejection::InvalidSSA);
        }
        std::vector<u32> node(count);
        for (u32 i = 0; i < count; ++i) {
            if (leaders[i]) data.blocks.push_back({.begin = i});
            node[i] = data.blocks.size() - 1;
        }
        const u32 nodes = data.blocks.size();
        for (u32 b = 0; b < nodes; ++b) data.blocks[b].end = b + 1 < nodes ? data.blocks[b + 1].begin : count;
        const auto edge = [&](u32 from, u32 to, ir::Inst* source, bool taken, bool host_entry) {
            const u32 index = data.edges.size();
            data.edges.push_back({from, to, source, taken, host_entry});
            data.blocks[to].incoming_edges.push_back(index);
            if (!host_entry) data.blocks[from].outgoing_edges.push_back(index);
        };
        edge(0, 0, nullptr, false, true);
        for (u32 b = 0; b < nodes; ++b) {
            auto* last = insts[data.blocks[b].end - 1];
            // Keep live edge occurrence order identical to ConfigurePhis.
            // A terminal exit replaces its marker's opcode completely; an
            // old Goto identity must not invent an executable taken edge.
            if (b + 1 < nodes && !function.stops.contains(last)) edge(b, b + 1, last, false, false);
            if (!function.exits.contains(last) && (last->GetOp() == O::Goto || last->GetOp() == O::NotGoto))
                edge(b, node[bound.at(target(last))], last, true, false);
        }
        std::vector<Set> phi_defs(nodes), edge_uses(data.edges.size());
        for (u32 b = 0; b < nodes; ++b) {
            u32 first = data.blocks[b].begin;
            if (insts[first]->GetOp() == O::BindLabel) ++first;
            u32 end = first;
            while (end < data.blocks[b].end && insts[end]->GetOp() == O::AddPhi) ++end;
            for (u32 i = end; i < data.blocks[b].end; ++i) Require(insts[i]->GetOp() != O::AddPhi, CfgPlanRejection::InvalidPhi);
            for (u32 i = first; i < end; ++i) {
                auto* phi = insts[i];
                Require(Container(phi->ReturnType()) && phi->ArgAt(0).IsParams(), CfgPlanRejection::InvalidPhi);
                for (u32 slot = 1; slot < ir::Inst::max_args; ++slot) Require(phi->ArgAt(slot).IsVoid(), CfgPlanRejection::InvalidPhi);
                std::vector<ir::DataClass> inputs;
                for (const auto& param : phi->GetArg<ir::Params>(0)) inputs.push_back(param.data);
                Require(inputs.size() == data.blocks[b].incoming_edges.size(), CfgPlanRejection::InvalidPhi);
                if (build_interference && Scalar(phi->ReturnType())) phi_defs[b].insert(i);
                for (u32 p = 0; p < inputs.size(); ++p) {
                    auto& incoming = data.edges[data.blocks[b].incoming_edges[p]];
                    const auto input = inputs[p];
                    Require(input.IsImm() || input.IsValue(), CfgPlanRejection::InvalidPhi);
                    if (input.IsValue()) {
                        Require(positions.contains(input.value.Def()), CfgPlanRejection::InvalidSSA);
                        Require(!incoming.host_entry && Container(input.value.Type()) && Container(input.value.Def()->ReturnType()) &&
                                ir::GetValueSizeByte(input.value.Type()) == ir::GetValueSizeByte(phi->ReturnType()), CfgPlanRejection::InvalidPhi);
                    }
                    incoming.copies.push_back({phi, input});
                }
            }
        }
        // Dominators over all reachable lowered bodies, independent of their
        // lexical order. Only host entry is an independent execution root.
        std::vector<u32> work{0}; data.blocks[0].reachable = true;
        for (u32 next = 0; next < work.size(); ++next) for (const auto e : data.blocks[work[next]].outgoing_edges) {
            const auto to = data.edges[e].to;
            if (!data.blocks[to].reachable) { data.blocks[to].reachable = true; work.push_back(to); }
        }
        Set reachable(work.begin(), work.end());
        std::vector<Set> dominators(nodes, reachable);
        dominators[0] = {0};
        bool changed;
        do {
            changed = false;
            for (u32 b = 1; b < nodes; ++b) if (data.blocks[b].reachable) {
                Set intersection = reachable;
                for (auto e : data.blocks[b].incoming_edges) {
                    const auto& incoming = data.edges[e];
                    if (incoming.host_entry || !data.blocks[incoming.from].reachable) continue;
                    Set next; std::set_intersection(intersection.begin(), intersection.end(),
                            dominators[incoming.from].begin(), dominators[incoming.from].end(), std::inserter(next, next.end()));
                    intersection.swap(next);
                }
                intersection.insert(b);
                if (intersection != dominators[b]) { dominators[b].swap(intersection); changed = true; }
            }
        } while (changed);

        data.dominators.resize(nodes);
        for (u32 b = 0; b < nodes; ++b) if (data.blocks[b].reachable)
            data.dominators[b].insert(dominators[b].begin(), dominators[b].end());

        if (data.loop_weighted_costs) {
            // A natural backedge targets a dominator of its source. Merge
            // all latches for one header before walking predecessors: several
            // backedges in one loop must not multiply the nesting estimate.
            std::vector<Set> loops(nodes);
            for (const auto& incoming : data.edges) {
                if (incoming.host_entry || !data.blocks[incoming.from].reachable ||
                    !data.blocks[incoming.to].reachable ||
                    !dominators[incoming.from].contains(incoming.to)) continue;
                loops[incoming.to].insert(incoming.to);
                loops[incoming.to].insert(incoming.from);
            }
            for (u32 header = 0; header < nodes; ++header) {
                auto& members = loops[header];
                if (members.empty()) continue;
                ++data.construction_stats.natural_loops;
                std::vector<u32> pending;
                for (auto latch : members) if (latch != header) pending.push_back(latch);
                while (!pending.empty()) {
                    const auto member = pending.back(); pending.pop_back();
                    for (auto e : data.blocks[member].incoming_edges) {
                        const auto& incoming = data.edges[e];
                        const auto predecessor = incoming.from;
                        if (incoming.host_entry || !data.blocks[predecessor].reachable ||
                            !dominators[predecessor].contains(header)) continue;
                        if (members.insert(predecessor).second && predecessor != header)
                            pending.push_back(predecessor);
                    }
                }
                // A self backedge starts and stops at its header, so its
                // outside predecessors never become loop members.
                for (auto member : members) ++data.blocks[member].loop_depth;
            }
            for (auto& member : data.blocks) {
                data.construction_stats.max_loop_depth =
                    std::max(data.construction_stats.max_loop_depth, member.loop_depth);
                // Preserve the actual depth for diagnosis; bound only the
                // estimate. Irreducible cycles without natural headers stay 1.
                member.loop_weight = u32{1} << (3 * std::min(member.loop_depth, u32{4}));
            }
        }

        std::vector<Set> uses(count), late_uses(count), terminal_uses(nodes);
        std::vector<bool> home(count, false), eligible(count, false);
        std::vector<u32> use_count(count), scalar_reads(count);
        std::vector<u64> weighted_reads(build_interference ? count : 0);
        const auto use = [&](ir::Value value, u32 at, u32 b, Set& destination, bool audited, bool phi_edge,
                             bool counted_read = true, bool runtime_use = true) {
            const auto found = positions.find(value.Def());
            Require(found != positions.end(), CfgPlanRejection::InvalidSSA);
            const u32 producer = found->second;
            Require(Container(value.Def()->ReturnType()) && Container(value.Type()), CfgPlanRejection::InvalidSSA);
            auto* definition = value.Def();
            if (definition->GetOp() == O::CpuidUpper || definition->GetOp() == O::Div128Remainder ||
                definition->GetOp() == O::VecSatMulQC || definition->GetOp() == O::VecSatShiftQC) {
                Require(definition->ArgAt(0).IsValue(), CfgPlanRejection::InvalidSSA);
                definition = definition->GetArg<ir::Value>(0).Def();
                Require(positions.contains(definition), CfgPlanRejection::InvalidSSA);
            }
            if (data.blocks[b].reachable) {
                const auto where = positions.at(definition), from = node[where];
                Require(data.blocks[from].reachable && dominators[b].contains(from) &&
                        (from != b || where < at), CfgPlanRejection::InvalidSSA);
            }
            if (build_interference && runtime_use && Scalar(value.Def()->ReturnType())) {
                destination.insert(producer); ++use_count[producer];
                if (audited && counted_read && Scalar(value.Type()) && data.blocks[b].reachable) {
                    ++scalar_reads[producer];
                    weighted_reads[producer] += data.blocks[b].loop_weight;
                }
                if (!audited || !Scalar(value.Type())) home[producer] = true;
                // Raw/opaque physical occurrences conservatively retain the
                // value through output publication even if another occurrence
                // in this instruction is a normal scalar Read.
                if (!phi_edge && at < count && (!audited || !Scalar(value.Type()))) late_uses[at].insert(producer);
            }
        };
        const auto operands = [&](ir::Inst* inst, u32 at, u32 b, Set& destination, bool indirect) {
            for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
                if (inst->GetOp() == O::BindLabel && slot == 0) continue;
                auto& arg = inst->ArgAt(slot);
                if (arg.IsValue()) {
                    const bool flags_primary = !indirect && slot == 0 &&
                        (inst->GetOp() == O::SaveFlags || inst->GetOp() == O::BranchOnlyFlags || inst->GetOp() == O::GetFlags);
                    if (flags_primary) {
                        // Validate the physical view even when only metadata
                        // is read. Emission reads the canonical producer width.
                        const auto primary = arg.Get<ir::Value>();
                        use(primary, at, b, destination, false, false, false, false);
                        if (inst->GetOp() != O::GetFlags &&
                            ClassifyProducerFlags(primary.Def()->GetOp(), primary.Def()->ReturnType(),
                                                  inst->GetArg<ir::Flags>(1)).needs_result)
                            use(ir::Value{primary.Def()}, at, b, destination, true, false);
                        continue;
                    }
                    use(arg.Get<ir::Value>(), at, b, destination,
                                      !indirect && AuditedRead(inst, slot, arg.Get<ir::Value>()), false,
                                      inst->GetOp() != O::SaveFlags && inst->GetOp() != O::BranchOnlyFlags &&
                                      !((inst->GetOp() == O::Goto || inst->GetOp() == O::NotGoto) && function.stops.contains(inst)),
                                      !indirect);
                }
                else if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue())
                    use(arg.Get<ir::Lambda>().GetValue(), at, b, destination, false, false, true, !indirect);
                else if (arg.IsParams()) for (const auto& param : arg.Get<ir::Params>()) if (param.data.IsValue())
                    use(param.data.value, at, b, destination, false, false, true, !indirect);
            }
        };
        for (u32 i = 0; i < count; ++i) {
            auto* inst = insts[i]; eligible[i] = build_interference && Cacheable(inst) && data.blocks[node[i]].reachable;
            if (inst->GetOp() != O::AddPhi && !function.exits.contains(inst)) operands(inst, i, node[i], uses[i], false);
            if (inst->GetOp() == O::SaveFlags || inst->GetOp() == O::BranchOnlyFlags || inst->GetOp() == O::GetFlags) {
                Require(inst->ArgAt(0).IsValue() && positions.contains(inst->GetArg<ir::Value>(0).Def()), CfgPlanRejection::InvalidSSA);
                // Delayed flags use the producer result and its native flags
                // snapshot. Original operands still need full SSA validation,
                // but are not runtime reads at this marker.
                operands(inst->GetArg<ir::Value>(0).Def(), i, node[i], uses[i], true);
            }
        }
        const auto terminal = [&](const ir::Terminal& term, u32 at, u32 b) {
            TerminalInputs(term, [&](ir::Value value) {
                use(value, at, b, terminal_uses[b], true, false);
                Require(Scalar(value.Type()), CfgPlanRejection::InvalidSSA);
            });
        };
        terminal(block->GetTerminal(), count, nodes - 1);
        for (const auto& [marker, term] : function.exits) terminal(term, positions.at(marker), node[positions.at(marker)]);
        for (u32 e = 0; e < data.edges.size(); ++e) {
            auto& incoming = data.edges[e];
            for (const auto& copy : incoming.copies) if (copy.source.IsValue()) {
                use(copy.source.value, data.blocks[incoming.from].end, incoming.from, edge_uses[e],
                    Scalar(copy.destination->ReturnType()) && Scalar(copy.source.value.Type()), true);
            }
        }
        // Return only after every ordinary/raw, delayed metadata, terminal
        // and Phi-edge input has passed the same validation as allocation.
        if (!build_interference) {
            result.analysis = std::move(working);
            result.rejection = CfgPlanRejection::None;
            return result;
        }
        for (u32 i = 0; i < count; ++i) if (Scalar(insts[i]->ReturnType())) {
            data.allocation_indices.emplace(insts[i], result.allocations.size());
            // Positive static transfer benefit: saved audited reads plus one
            // omitted scalar home store, less one cache publication. Raw-home
            // obligations and flags metadata stay separate and unchanged.
            const bool needs_home = use_count[i] && (home[i] || !eligible[i]);
            const auto savings = use_count[i] ? scalar_reads[i] + (needs_home ? 0u : 1u) : 0u;
            // Widen before summing and subtracting the publication cost;
            // saturate each public counter independently only at the end.
            const u64 definition_weight = data.blocks[node[i]].loop_weight;
            const u64 weighted_savings = use_count[i] ? weighted_reads[i] + (needs_home ? 0 : definition_weight) : 0;
            const u64 weighted_benefit = weighted_savings > definition_weight ? weighted_savings - definition_weight : 0;
            const auto saturate = [](u64 value) { return static_cast<u32>(std::min(value, u64{std::numeric_limits<u32>::max()})); };
            result.allocations.push_back({insts[i], std::nullopt, needs_home, use_count[i],
                                          scalar_reads[i], savings ? savings - 1 : 0, home[i],
                                          saturate(weighted_reads[i]), saturate(weighted_benefit)});
            data.base_canonical_homes.push_back(needs_home);
            data.allocation_eligible.push_back(eligible[i]);
        }
        // Fixed point over complete CFG, with incoming Phi source uses on
        // the matching predecessor occurrence rather than at the join point.
        std::vector<Set> live_in(nodes), live_out(nodes);
        do {
            changed = false;
            for (u32 reverse = nodes; reverse-- > 0;) {
                const u32 b = reverse; if (!data.blocks[b].reachable) continue;
                Set out;
                for (auto e : data.blocks[b].outgoing_edges) {
                    const auto to = data.edges[e].to;
                    out.insert(live_in[to].begin(), live_in[to].end());
                    out.insert(edge_uses[e].begin(), edge_uses[e].end());
                }
                Set in = out; in.insert(terminal_uses[b].begin(), terminal_uses[b].end());
                for (u32 i = data.blocks[b].end; i-- > data.blocks[b].begin;) {
                    if (Scalar(insts[i]->ReturnType())) in.erase(i);
                    in.insert(uses[i].begin(), uses[i].end());
                }
                if (in != live_in[b] || out != live_out[b]) { live_in[b].swap(in); live_out[b].swap(out); changed = true; }
            }
        } while (changed);
        data.has_liveness = true;
        // Accumulate repeated cliques in bounded word storage. Positions are
        // unchanged IR ordinals, including non-cacheable/raw scalar values.
        // The reference sparse builder remains the fallback for any extent.
        const size_t words = size_t(count) / 64 + (count % 64 != 0);
        std::vector<u64> dense;
        size_t cells{};
        if (words && size_t(count) < std::numeric_limits<size_t>::max()) {
            const size_t rows = size_t(count) + 1; // Extra row is the clique mask.
            const u64 budget_words = options.interference_storage_budget_bytes / sizeof(u64);
            if (rows <= budget_words / words && rows <= dense.max_size() / words &&
                rows <= std::numeric_limits<size_t>::max() / sizeof(u64) / words) {
                cells = rows * words;
                try {
                    dense.resize(cells);
                } catch (const std::bad_alloc&) {
                    // Temporary acceleration is optional; do not reject IR
                    // that the original sparse construction could handle.
                }
            }
        }
        data.construction_stats.dense_used = !dense.empty();
        data.construction_stats.sparse_fallback = dense.empty();
        data.construction_stats.dense_bytes = dense.empty() ? 0 : u64(cells) * sizeof(u64);
        const auto link = [&](u32 left, u32 right) {
            if (left == right) return;
            data.interference[insts[left]].insert(insts[right]);
            data.interference[insts[right]].insert(insts[left]);
        };
        const auto clique = [&](const Set& values) {
            ++data.construction_stats.clique_calls;
            if (!dense.empty()) {
                if (values.size() < 2) return;
                auto* mask = dense.data() + size_t(count) * words;
                std::fill_n(mask, words, u64{});
                for (auto value : values) mask[value / 64] |= u64{1} << (value % 64);
                for (auto value : values) {
                    auto* row = dense.data() + size_t(value) * words;
                    for (size_t word = 0; word < words; ++word) row[word] |= mask[word];
                    row[value / 64] &= ~(u64{1} << (value % 64));
                }
                return;
            }
            for (auto left = values.begin(); left != values.end(); ++left)
                for (auto right = std::next(left); right != values.end(); ++right) link(*left, *right);
        };
        const auto vector = [&](const Set& values) {
            std::vector<ir::Inst*> out; for (auto value : values) out.push_back(insts[value]); return out;
        };
        for (u32 b = 0; b < nodes; ++b) {
            data.blocks[b].live_in = vector(live_in[b]); data.blocks[b].live_out = vector(live_out[b]);
            if (!data.blocks[b].reachable) continue;
            Set live = live_out[b]; live.insert(terminal_uses[b].begin(), terminal_uses[b].end()); clique(live);
            for (u32 i = data.blocks[b].end; i-- > data.blocks[b].begin;) {
                const bool phase_binary = PhaseBinary(insts[i]->GetOp()) && Scalar(insts[i]->ReturnType());
                const bool phase_unary = data.scalar_unary_phases && PhaseUnary(insts[i]);
                if (phase_binary || phase_unary) {
                    // Input capture precedes ALU/output publication for these
                    // audited lowerings. Preserve all raw/opaque occurrences
                    // through Write; ordinary dead reads can share its slot.
                    Set read = live; read.erase(i); read.insert(uses[i].begin(), uses[i].end()); clique(read);
                    Set write = live; write.insert(i); write.insert(late_uses[i].begin(), late_uses[i].end()); clique(write);
                    if (insts[i]->ArgAt(0).IsValue()) {
                        const auto lhs = insts[i]->GetArg<ir::Value>(0);
                        const auto source = positions.at(lhs.Def());
                        if (Scalar(lhs.Type()) && uses[i].contains(source) &&
                            !live.contains(source) && !late_uses[i].contains(source))
                            (phase_binary ? data.binary_reuse : data.unary_reuse).emplace(insts[i], lhs.Def());
                    }
                } else {
                    Set coexist = live; coexist.insert(uses[i].begin(), uses[i].end());
                    if (Scalar(insts[i]->ReturnType())) coexist.insert(i);
                    clique(coexist);
                }
                live.erase(i); live.insert(uses[i].begin(), uses[i].end());
            }
            Set phi_live = live_in[b]; phi_live.insert(phi_defs[b].begin(), phi_defs[b].end()); clique(phi_live);
        }
        for (u32 e = 0; e < data.edges.size(); ++e) {
            auto& incoming = data.edges[e];
            Set live = live_in[incoming.to]; live.insert(edge_uses[e].begin(), edge_uses[e].end());
            incoming.live = vector(live); if (!incoming.host_entry && data.blocks[incoming.from].reachable) clique(live);
        }
        if (!dense.empty()) {
            // Materialize each undirected edge once into the existing maps.
            // Coloring still sees exactly the reference builder's neighbors.
            for (u32 left = 0; left < count; ++left) {
                const auto* row = dense.data() + size_t(left) * words;
                for (size_t word = left / 64; word < words; ++word) {
                    u64 bits = row[word];
                    if (word == left / 64) {
                        const auto bit = left % 64;
                        bits &= bit == 63 ? u64{} : ~u64{} << (bit + 1);
                    }
                    while (bits) {
                        const size_t right = word * 64 + std::countr_zero(bits);
                        if (right < count) link(left, u32(right));
                        bits &= bits - 1;
                    }
                }
            }
            std::vector<u64>{}.swap(dense); // Temporary words are not needed by coloring.
        }
        data.has_interference = true;
        // All shared facts are complete. Publish const ownership and release
        // the mutable handle before coloring any private allocation array.
        result.analysis = std::move(working);
        result.rejection = CfgPlanRejection::None;
        result.ColorAllocations();
        Require(result.Validate(), CfgPlanRejection::InvalidSSA);
    } catch (const Invalid& error) {
        result.rejection = error.reason; result.allocations.clear();
        if (working) {
            // Preserve the accepted diagnostic getters after failed Build.
            // Allocation facts remain unusable; only construction still
            // holding mutable ownership may clear these private indices.
            working->allocation_indices.clear();
            working->base_canonical_homes.clear();
            working->allocation_eligible.clear();
            result.analysis = std::move(working);
        }
        // A failure after publication retains its immutable diagnostics.
        // Allocation queries fail closed via Enabled(), without mutating
        // an Analysis which may already be shared by valid sibling plans.
    }
    return result;
}

const CfgRegisterPlan::Analysis& CfgRegisterPlan::Data() const noexcept {
    static const Analysis empty;
    return analysis ? *analysis : empty;
}

const std::unordered_set<const ir::Inst*>& CfgRegisterPlan::Neighbors(const ir::Inst* producer) const noexcept {
    static const std::unordered_set<const ir::Inst*> empty;
    const auto& interference = Data().interference;
    const auto found = interference.find(producer);
    return found != interference.end() ? found->second : empty;
}

void CfgRegisterPlan::ColorAllocations() {
    const auto& insts = Data().instructions;
    const u32 count = insts.size();
    std::unordered_map<ir::Inst*, u32> positions;
    for (u32 i = 0; i < count; ++i) positions.emplace(insts[i], i);
    Require(Data().base_canonical_homes.size() == allocations.size() &&
            Data().allocation_eligible.size() == allocations.size(), CfgPlanRejection::InvalidSSA);
    for (u32 i = 0; i < allocations.size(); ++i) {
        allocations[i].slot.reset();
        allocations[i].requires_canonical_home = Data().base_canonical_homes[i];
    }
    // Deterministic optimistic simplification and reverse coloring.
    // Spilled producers remain
    // home-only everywhere; never replace a runtime slot at a CFG edge.
    std::unordered_map<const ir::Inst*, std::vector<const ir::Inst*>> affinities;
    const auto add_affinities = [&](const auto& reuse) {
        for (const auto& [output, source] : reuse) if (!Interferes(output, source)) {
            affinities[output].push_back(source); affinities[source].push_back(output);
        }
    };
    add_affinities(Data().binary_reuse);
    add_affinities(Data().unary_reuse);
    std::vector<bool> pending(count);
    std::vector<u32> degree(count), stack;
    const auto benefit = [&](u32 i) {
        const auto& allocation = allocations[Data().allocation_indices.at(insts[i])];
        return Data().loop_weighted_costs ? allocation.weighted_benefit : allocation.estimated_benefit;
    };
    if (!physical_registers.empty())
        for (u32 i = 0; i < count; ++i) {
            const auto found = Data().allocation_indices.find(insts[i]);
            if (found != Data().allocation_indices.end()) {
                const auto index = found->second;
                if (Data().allocation_eligible[index] && allocations[index].uses && benefit(i)) pending[i] = true;
            }
        }
    u32 remaining{};
    for (u32 i = 0; i < count; ++i) if (pending[i]) {
        ++remaining;
        for (auto* other : Neighbors(insts[i])) degree[i] += pending[positions.at(const_cast<ir::Inst*>(other))];
    }
    while (remaining) {
        std::optional<u32> chosen;
        // Lexical tie order is stable. Low-degree values are guaranteed a
        // color when restored; do not eagerly spill high-degree values.
        for (u32 i = 0; i < count; ++i) if (pending[i] && degree[i] < physical_registers.size()) { chosen = i; break; }
        if (!chosen) for (u32 i = 0; i < count; ++i) if (pending[i]) {
            // Select the cheapest potential spill per remaining conflict.
            // Integer cross-products preserve deterministic ratio ties.
            if (!chosen || u64(benefit(i)) * (u64{degree[*chosen]} + 1) <
                           u64(benefit(*chosen)) * (u64{degree[i]} + 1)) chosen = i;
        }
        const auto removed = *chosen;
        pending[removed] = false; --remaining; stack.push_back(removed);
        for (auto* other : Neighbors(insts[removed])) {
            const auto neighbor = positions.at(const_cast<ir::Inst*>(other));
            if (pending[neighbor]) --degree[neighbor];
        }
    }
    while (!stack.empty()) {
        const u32 chosen = stack.back(); stack.pop_back();
        std::set<unsigned> unavailable;
        for (auto* other : Neighbors(insts[chosen])) if (auto color = Slot(other)) unavailable.insert(*color);
        auto& allocation = allocations[Data().allocation_indices.at(insts[chosen])];
        // Prefer the audited lhs/output affinity in either coloring order.
        // Vote by color, then lowest slot: unordered map iteration cannot
        // change the stable result or override an interference constraint.
        std::vector<u32> votes(physical_registers.size());
        for (const auto* other : affinities[insts[chosen]])
            if (auto color = Slot(other); color && !unavailable.contains(*color)) ++votes[*color];
        for (unsigned slot = 0; slot < votes.size(); ++slot)
            if (votes[slot] && (!allocation.slot || votes[slot] > votes[*allocation.slot])) allocation.slot = slot;
        if (!allocation.slot)
            for (unsigned slot = 0; slot < physical_registers.size(); ++slot) if (!unavailable.contains(slot)) { allocation.slot = slot; break; }
        if (!allocation.slot) allocation.requires_canonical_home = true;
    }
    for (auto& allocation : allocations)
        if (!allocation.slot && allocation.uses) allocation.requires_canonical_home = true;
}

CfgRegisterPlan CfgRegisterPlan::Recolor(std::vector<unsigned> registers) const {
    CfgRegisterPlan rejected;
    if (!HasLiveness() || !HasInterference() || !Validate()) {
        rejected.rejection = Enabled() ? CfgPlanRejection::InvalidSSA : rejection;
        return rejected;
    }
    if (!ValidPhysicalBudget(registers)) {
        rejected.rejection = CfgPlanRejection::InvalidBudget;
        return rejected;
    }
    // Share only published immutable facts. Allocations and physical budgets
    // remain independently owned, so resetting colors and restoring base home
    // obligations cannot change the original plan or any sibling recolor.
    CfgRegisterPlan result = *this;
    result.physical_registers = std::move(registers);
    try {
        result.ColorAllocations();
        Require(result.Validate(), CfgPlanRejection::InvalidSSA);
    } catch (const Invalid& error) {
        rejected.rejection = error.reason;
        return rejected;
    }
    return result;
}

std::optional<unsigned> CfgRegisterPlan::Slot(const ir::Inst* producer) const noexcept {
    if (!Enabled()) return std::nullopt;
    const auto found = Data().allocation_indices.find(producer);
    return found == Data().allocation_indices.end() ? std::nullopt : allocations[found->second].slot;
}
std::optional<unsigned> CfgRegisterPlan::Register(const ir::Inst* producer) const noexcept {
    const auto slot = Slot(producer); return slot ? std::optional<unsigned>{physical_registers[*slot]} : std::nullopt;
}
bool CfgRegisterPlan::RequiresCanonicalHome(const ir::Inst* producer) const noexcept {
    if (!Enabled()) return true;
    const auto found = Data().allocation_indices.find(producer);
    return found == Data().allocation_indices.end() || allocations[found->second].requires_canonical_home;
}
bool CfgRegisterPlan::RequiresFullContainer(const ir::Inst* producer) const noexcept {
    if (!HasInterference()) return true;
    const auto found = Data().allocation_indices.find(producer);
    return found == Data().allocation_indices.end() || allocations[found->second].requires_full_container;
}
bool CfgRegisterPlan::Interferes(const ir::Inst* left, const ir::Inst* right) const noexcept {
    const auto found = Data().interference.find(left); return found != Data().interference.end() && found->second.contains(right);
}
bool CfgRegisterPlan::CanReuseBinaryInput(const ir::Inst* output, const ir::Inst* lhs) const noexcept {
    if (!Enabled()) return false;
    const auto found = Data().binary_reuse.find(output);
    return found != Data().binary_reuse.end() && found->second == lhs && !Interferes(output, lhs);
}
bool CfgRegisterPlan::CanReuseUnaryInput(const ir::Inst* output, const ir::Inst* source) const noexcept {
    if (!ScalarUnaryPhasesEnabled()) return false;
    const auto found = Data().unary_reuse.find(output);
    return found != Data().unary_reuse.end() && found->second == source && !Interferes(output, source);
}
bool CfgRegisterPlan::Validate() const noexcept {
    if (!HasLiveness() || !HasInterference()) return false;
    for (const auto& allocation : allocations) if (allocation.slot) {
        if (*allocation.slot >= physical_registers.size()) return false;
        const auto found = Data().interference.find(allocation.producer);
        if (found == Data().interference.end()) continue;
        for (auto* other : found->second) if (Slot(other) == allocation.slot) return false;
    }
    return true;
}

bool CfgRegisterPlan::MatchesBody(const ir::Block* candidate) const noexcept {
    if (!Enabled() || candidate != Data().body || !candidate) return false;
    size_t index{};
    for (const auto& inst : candidate->GetInstList()) {
        if (index >= Data().instructions.size() || Data().instructions[index++] != &inst) return false;
    }
    return index == Data().instructions.size();
}
bool CfgRegisterPlan::DominatesBlock(u32 from, u32 to) const noexcept {
    return Enabled() && to < Data().dominators.size() && Data().dominators[to].contains(from);
}
}  // namespace swift::runtime::backend::x64
