#include "scalar_allocation_plan.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace swift::runtime::backend::loongarch64 {
namespace {
using O = ir::OpCode;

bool EarlyResult(ir::Inst* inst) {
    const auto op = inst->GetOp();
    return op == O::VecSatMulQC || op == O::VecSatShiftQC ||
           op == O::Div128Remainder || op == O::CpuidUpper;
}

void ScalarOperands(ir::Inst* inst, LiveSet& inputs) {
    const auto add = [&](ir::Value value) {
        if (ScalarAllocationPlan::Eligible(value.Def())) inputs.insert(value.Def());
    };
    for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
        auto& arg = inst->ArgAt(slot);
        if (arg.IsValue()) add(arg.Get<ir::Value>());
        else if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue()) add(arg.Get<ir::Lambda>().GetValue());
        else if (arg.IsParams()) for (const auto& parameter : arg.Get<ir::Params>())
            if (parameter.data.IsValue()) add(parameter.data.value);
    }
}

}  // namespace

bool ScalarAllocationPlan::Eligible(ir::Inst* value) {
    return value && value->ReturnType() >= ir::ValueType::U8 && value->ReturnType() <= ir::ValueType::S64;
}

std::optional<u32> ScalarAllocationPlan::Register(ir::Inst* value) const {
    const auto found = registers.find(value);
    return found == registers.end() ? std::nullopt : std::optional{found->second};
}

std::optional<u32> ScalarAllocationPlan::RegisterAt(ir::Inst* value, ir::Inst* at) const {
    if (!splitting || !at || point_registers.empty()) return Register(value);
    const auto& placement = point_registers.at(at);
    const auto found = placement.find(value);
    return found == placement.end() ? std::nullopt : std::optional{found->second};
}

std::span<const ScalarSplitMove> ScalarAllocationPlan::MovesBefore(ir::Inst* at) const {
    const auto found = split_moves.find(at);
    return found == split_moves.end() ? std::span<const ScalarSplitMove>{} : found->second;
}

std::span<const ScalarLiveFragment> ScalarAllocationPlan::Fragments(ir::Inst* value) const {
    const auto found = fragments.find(value);
    return found == fragments.end() ? std::span<const ScalarLiveFragment>{} : found->second;
}

std::optional<u64> ScalarAllocationPlan::Immediate(ir::Inst* value) const {
    const auto found = immediates.find(value);
    return found == immediates.end() ? std::nullopt : std::optional{found->second};
}

bool ScalarAllocationPlan::Interferes(ir::Inst* left, ir::Inst* right) const {
    const auto found = interference.find(left);
    return found != interference.end() && found->second.contains(right);
}

ScalarAllocationPlan ScalarAllocationPlan::Build(ir::Block* block, const RegisterPlan& live,
                                                const RegisterPlan::Targets& targets,
                                                u32 register_count, bool rematerialize_constants,
                                                bool split_live_ranges, const RegisterPlan::Stops& stops,
                                                const RegisterPlan::Exits& exits) {
    if (!block || register_count > kRegisterCount)
        throw std::invalid_argument("invalid LoongArch scalar allocation input");
    ScalarAllocationPlan result;
    std::vector<ir::Inst*> instructions, values;
    std::unordered_map<ir::Inst*, u32> positions, indices, labels;
    for (auto& inst : block->GetInstList()) {
        positions.emplace(&inst, instructions.size()); instructions.push_back(&inst);
        result.inputs.try_emplace(&inst); result.definitions.try_emplace(&inst);
        if (inst.GetOp() == O::BindLabel) labels.emplace(inst.GetArg<ir::Value>(0).Def(), instructions.size() - 1);
        if (Eligible(&inst)) {
            ++result.stats.values;
            // Only immutable, scalar LoadImm results are reconstructed. Raw
            // aliases, phi values and helper outputs retain their full homes.
            if (rematerialize_constants && inst.GetOp() == O::LoadImm && inst.ArgAt(0).IsImm() &&
                std::all_of(&inst.ArgAt(1), &inst.ArgAt(ir::Inst::max_args - 1) + 1,
                            [](const ir::Arg& arg) { return arg.IsVoid(); })) {
                const u32 bits = ir::GetValueSizeByte(inst.ReturnType()) * 8;
                const auto mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
                result.immediates.emplace(&inst, inst.GetArg<ir::Imm>(0).Get() & mask);
                continue;
            }
            indices.emplace(&inst, values.size()); values.push_back(&inst);
            result.interference.try_emplace(&inst);
        }
    }
    result.stats.rematerialized_values = result.immediates.size();
    if (instructions.empty()) {
        result.splitting = split_live_ranges;
        return result;
    }

    std::vector<std::vector<u32>> preferences(values.size());
    for (u32 i = 0; i < instructions.size(); ++i) {
        auto* inst = instructions[i];
        if (inst->GetOp() != O::BindLabel && inst->GetOp() != O::AddPhi && !EarlyResult(inst))
            ScalarOperands(inst, result.inputs.at(inst));
        if (inst->GetOp() == O::SaveFlags || inst->GetOp() == O::GetFlags || inst->GetOp() == O::BranchOnlyFlags)
            ScalarOperands(inst->GetArg<ir::Value>(0).Def(), result.inputs.at(inst));
        if (!Eligible(inst)) continue;
        if (EarlyResult(inst)) {
            auto* producer = inst->GetArg<ir::Value>(0).Def();
            if (!positions.contains(producer)) throw std::runtime_error("foreign LoongArch scalar pseudo producer");
            result.definitions.at(producer).insert(inst);
        } else if (inst->GetOp() == O::AddPhi) {
            u32 entry = i;
            while (entry && instructions[entry - 1]->GetOp() == O::AddPhi) --entry;
            if (entry && instructions[entry - 1]->GetOp() == O::BindLabel) --entry;
            result.definitions.at(instructions[entry]).insert(inst);
            for (const auto& parameter : inst->GetArg<ir::Params>(0)) if (parameter.data.IsValue()) {
                auto* source = parameter.data.value.Def();
                if (indices.contains(source)) {
                    const auto source_index = indices.at(source), destination_index = indices.at(inst);
                    preferences[destination_index].push_back(source_index);
                    preferences[source_index].push_back(destination_index);
                }
            }
        } else result.definitions.at(inst).insert(inst);
    }

    // Loop intervals affect spill preference only; liveness/interference is
    // taken from the full CFG fixed point, including irreducible backedges.
    std::vector<u32> weights(instructions.size(), 1);
    for (auto* inst : instructions) if (inst->GetOp() == O::Goto || inst->GetOp() == O::NotGoto) {
        auto* target = targets.contains(inst) ? targets.at(inst) : inst;
        const auto found = labels.find(target);
        const auto first = found != labels.end() ? found->second : positions.at(target);
        const auto last = positions.at(inst);
        if (first <= last) for (u32 at = first; at <= last; ++at) weights[at] = std::min(weights[at] * 8, u32{4096});
    }
    std::vector<u64> costs(values.size(), 1);
    for (u32 i = 0; i < instructions.size(); ++i) {
        auto* inst = instructions[i];
        LiveSet together;
        for (auto* value : live.At(inst)) if (Eligible(value) && !result.Rematerialized(value)) together.insert(value);
        for (auto* value : live.After(inst)) if (Eligible(value) && !result.Rematerialized(value)) together.insert(value);
        for (auto* value : result.definitions.at(inst)) if (!result.Rematerialized(value)) together.insert(value);
        result.stats.max_pressure = std::max(result.stats.max_pressure, u32(together.size()));
        // Before/after are deliberately one clique. Emitters may write a
        // result before rereading an input to compute flags or another half.
        for (auto* left : together) for (auto* right : together) if (left != right) {
            if (!indices.contains(left) || !indices.contains(right))
                throw std::runtime_error("foreign value in LoongArch scalar liveness");
            result.interference.at(left).insert(right);
        }
        for (auto* value : result.inputs.at(inst)) {
            ++result.stats.input_sites;
            if (result.Rematerialized(value)) ++result.stats.rematerialized_input_sites;
            else costs[indices.at(value)] += weights[i] * 4;
        }
        for (auto* value : result.definitions.at(inst)) {
            ++result.stats.definition_sites;
            if (!result.Rematerialized(value)) costs[indices.at(value)] += weights[i];
        }
        // Phi edge reads and terminal uses are already in the liveness sets.
        // Give them cost too, without confusing a lexical hint with deadness.
        for (auto* value : live.ForBranch(inst)) if (indices.contains(value)) costs[indices.at(value)] += weights[i];
        if (i + 1 == instructions.size()) for (auto* value : live.ForFallthrough(inst))
            if (indices.contains(value)) costs[indices.at(value)] += weights[i] * 4;
    }
    std::vector<std::vector<u32>> graph(values.size());
    for (u32 i = 0; i < values.size(); ++i) {
        for (auto* neighbor : result.interference.at(values[i])) graph[i].push_back(indices.at(neighbor));
        result.stats.interference_edges += graph[i].size();
    }
    result.stats.interference_edges /= 2;

    // Optimistic simplify/select. A high-degree value is only a spill
    // candidate; selection can still color it after neighbors spill.
    std::vector<u32> degrees(values.size()), stack;
    std::vector<bool> remaining(values.size(), true);
    for (u32 i = 0; i < values.size(); ++i) degrees[i] = graph[i].size();
    for (u32 removed = 0; removed < values.size(); ++removed) {
        u32 selected = UINT32_MAX;
        for (u32 i = 0; i < values.size(); ++i) if (remaining[i] && degrees[i] < register_count) {
            selected = i; break;
        }
        if (selected == UINT32_MAX) for (u32 i = 0; i < values.size(); ++i) if (remaining[i]) {
            if (selected == UINT32_MAX || costs[i] * (degrees[selected] + 1) < costs[selected] * (degrees[i] + 1))
                selected = i;
        }
        remaining[selected] = false; stack.push_back(selected);
        for (auto neighbor : graph[selected]) if (remaining[neighbor]) --degrees[neighbor];
    }
    std::vector<u32> colors(values.size(), UINT32_MAX);
    for (auto iterator = stack.rbegin(); iterator != stack.rend(); ++iterator) {
        const auto value = *iterator;
        u32 unavailable{};
        for (auto neighbor : graph[value]) if (colors[neighbor] != UINT32_MAX) unavailable |= u32{1} << colors[neighbor];
        u32 chosen = UINT32_MAX;
        for (auto preferred : preferences[value]) if (colors[preferred] != UINT32_MAX &&
                !(unavailable & (u32{1} << colors[preferred]))) { chosen = colors[preferred]; break; }
        if (chosen == UINT32_MAX) for (u32 reg = 0; reg < register_count; ++reg)
            if (!(unavailable & (u32{1} << reg))) { chosen = reg; break; }
        if (chosen != UINT32_MAX) {
            colors[value] = chosen; result.registers.emplace(values[value], chosen);
        }
    }
    result.stats.register_values = result.registers.size();
    result.stats.spilled_values = result.stats.values - result.stats.register_values - result.stats.rematerialized_values;
    for (auto* inst : instructions) {
        for (auto* value : result.inputs.at(inst)) if (result.Spilled(value)) ++result.stats.spill_input_sites;
        for (auto* value : result.definitions.at(inst)) if (result.Spilled(value)) ++result.stats.spill_definition_sites;
        if (inst->GetOp() == O::AddPhi && result.Register(inst))
            for (const auto& parameter : inst->GetArg<ir::Params>(0))
                if (parameter.data.IsValue() && result.Register(parameter.data.value.Def()) == result.Register(inst))
                    ++result.stats.phi_coalesced;
    }
    if (split_live_ranges) result.Split(block, live, targets, stops, exits, register_count);
    return result;
}

void ScalarAllocationPlan::Split(ir::Block* block, const RegisterPlan& live,
                                 const RegisterPlan::Targets& targets, const RegisterPlan::Stops& stops,
                                 const RegisterPlan::Exits& exits, u32 register_count) {
    splitting = true;
    // Fully colored ranges already avoid spill traffic. Preserve that exact
    // placement rather than paying transfers that cannot recover a spill.
    if (!stats.spilled_values || !register_count) return;
    const auto baseline_input_sites = stats.spill_input_sites;
    const auto baseline_definition_sites = stats.spill_definition_sites;
    std::vector<ir::Inst*> order;
    std::unordered_map<ir::Inst*, u32> positions, labels;
    for (auto& inst : block->GetInstList()) {
        positions.emplace(&inst, order.size()); order.push_back(&inst);
        if (inst.GetOp() == O::BindLabel) labels.emplace(inst.GetArg<ir::Value>(0).Def(), order.size() - 1);
    }
    if (order.empty()) return;
    std::vector<bool> leaders(order.size() + 1);
    leaders.front() = leaders.back() = true;
    for (u32 i = 0; i < order.size(); ++i) {
        auto* inst = order[i];
        if (inst->GetOp() == O::BindLabel) leaders[i] = true;
        if (inst->GetOp() == O::Goto || inst->GetOp() == O::NotGoto) {
            auto* target = targets.contains(inst) ? targets.at(inst) : inst;
            const auto label = labels.find(target);
            const auto position = label != labels.end() ? label->second : positions.at(target);
            leaders[position] = true; leaders[i + 1] = true;
        }
        if (stops.contains(inst) || exits.contains(inst)) leaders[i + 1] = true;
    }

    const auto eligible = [&](ir::Inst* value) { return Eligible(value) && !Rematerialized(value); };
    const auto sorted = [&](const LiveSet& values) {
        std::vector<ir::Inst*> result;
        for (auto* value : values) if (eligible(value)) result.push_back(value);
        std::sort(result.begin(), result.end(), [&](auto* a, auto* b) { return positions.at(a) < positions.at(b); });
        return result;
    };
    // Placements are fixed for the entire emitter, including implicit flag
    // rereads and producer-written pseudo results. A destination cannot share
    // a register with an input that an emitter may reread after writing it.
    const auto together = [&](ir::Inst* inst) {
        LiveSet values = live.At(inst);
        values.insert(live.After(inst).begin(), live.After(inst).end());
        values.insert(definitions.at(inst).begin(), definitions.at(inst).end());
        return sorted(values);
    };
    std::unordered_map<ir::Inst*, u32> previous_fragment_position;
    for (u32 first = 0; first < order.size();) {
        u32 end = first + 1;
        while (!leaders[end]) ++end;
        u32 phi_end = first;
        if (order[phi_end]->GetOp() == O::BindLabel) ++phi_end;
        while (phi_end < end && order[phi_end]->GetOp() == O::AddPhi) ++phi_end;
        for (u32 i = first; i < end; ++i) {
            auto* inst = order[i];
            auto candidates = together(inst);
            auto& placement = point_registers[inst];
            const bool edge = i == first || i + 1 == end || i < phi_end;
            if (edge) {
                // Every branch, function exit and phi entry agrees with the
                // existing global coloring on all live edge values. A jump
                // never inherits the previous lexical instruction's state.
                for (auto* value : candidates) if (auto reg = Register(value)) placement.emplace(value, *reg);
            } else {
                const auto& previous = point_registers.at(order[i - 1]);
                const auto priority = [&](ir::Inst* value) {
                    // Actual operands win over dormant live-through values;
                    // definitions need a stable place throughout the emitter.
                    return inputs.at(inst).contains(value) ? 2u : definitions.at(inst).contains(value) ? 1u : 0u;
                };
                std::stable_sort(candidates.begin(), candidates.end(), [&](auto* a, auto* b) {
                    if (priority(a) != priority(b)) return priority(a) > priority(b);
                    const auto next_a = live.NextUse(a, inst), next_b = live.NextUse(b, inst);
                    if (next_a != next_b) return next_a < next_b;
                    if (previous.contains(a) != previous.contains(b)) return previous.contains(a);
                    return positions.at(a) < positions.at(b);
                });
                if (candidates.size() > register_count) candidates.resize(register_count);
                u32 used{};
                for (auto* value : candidates) if (auto found = previous.find(value); found != previous.end()) {
                    placement.emplace(value, found->second); used |= u32{1} << found->second;
                }
                for (auto* value : candidates) if (!placement.contains(value)) {
                    for (u32 reg = 0; reg < register_count; ++reg) if (!(used & (u32{1} << reg))) {
                        placement.emplace(value, reg); used |= u32{1} << reg; break;
                    }
                }
            }
            if (i != first) {
                const auto& previous = point_registers.at(order[i - 1]);
                // Only incoming values transfer. New definitions, including
                // phi/pseudo definitions, are written by their owning emitter.
                for (auto* value : sorted(live.At(inst))) {
                    const auto before = previous.find(value);
                    const auto after = placement.find(value);
                    const std::optional<u32> from = before == previous.end() ? std::nullopt : std::optional{before->second};
                    const std::optional<u32> to = after == placement.end() ? std::nullopt : std::optional{after->second};
                    if (from == to) continue;
                    split_moves[inst].push_back({value, from, to}); split_homes.insert(value);
                    stats.split_transfer_stores += from.has_value(); stats.split_transfer_loads += to.has_value();
                }
            }
            for (auto* value : together(inst)) {
                const auto assigned = placement.find(value);
                const std::optional<u32> reg = assigned == placement.end() ? std::nullopt : std::optional{assigned->second};
                if (!reg) split_homes.insert(value);
                auto& ranges = fragments[value];
                if (!ranges.empty() && previous_fragment_position.at(value) + 1 == i && ranges.back().reg == reg)
                    ranges.back().last = inst;
                else ranges.push_back({inst, inst, reg});
                previous_fragment_position[value] = i;
            }
        }
        first = end;
    }
    for (const auto& [value, ranges] : fragments) {
        stats.fragments += ranges.size();
        for (const auto& range : ranges) {
            stats.register_fragments += range.reg.has_value(); stats.home_fragments += !range.reg.has_value();
        }
        if (std::any_of(ranges.begin(), ranges.end(), [&](const auto& range) { return range.reg != ranges.front().reg; }))
            ++stats.split_values;
    }
    stats.spill_input_sites = stats.spill_definition_sites = 0;
    for (auto* inst : order) {
        for (auto* value : inputs.at(inst)) if (!Rematerialized(value) && !RegisterAt(value, inst)) ++stats.spill_input_sites;
        for (auto* value : definitions.at(inst)) if (!Rematerialized(value) && !RegisterAt(value, inst)) ++stats.spill_definition_sites;
    }
    // Each transfer costs a real home access, including both accesses when
    // staging a register swap. A short live range used only once need not
    // profit from splitting even when the global coloring spills it. Keep
    // the requested mode, but return its exact stable-placement baseline
    // unless the complete static access estimate shows a net saving.
    const auto split_accesses = stats.spill_input_sites + stats.spill_definition_sites +
                               stats.split_transfer_loads + stats.split_transfer_stores;
    if (split_accesses >= baseline_input_sites + baseline_definition_sites) {
        point_registers.clear(); split_moves.clear(); fragments.clear(); split_homes.clear();
        stats.split_values = stats.fragments = stats.register_fragments = stats.home_fragments = 0;
        stats.split_transfer_loads = stats.split_transfer_stores = 0;
        stats.spill_input_sites = baseline_input_sites; stats.spill_definition_sites = baseline_definition_sites;
    }
}

}  // namespace swift::runtime::backend::loongarch64
