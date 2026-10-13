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

bool ScalarAllocationPlan::Interferes(ir::Inst* left, ir::Inst* right) const {
    const auto found = interference.find(left);
    return found != interference.end() && found->second.contains(right);
}

ScalarAllocationPlan ScalarAllocationPlan::Build(ir::Block* block, const RegisterPlan& live,
                                                const RegisterPlan::Targets& targets,
                                                u32 register_count) {
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
            indices.emplace(&inst, values.size()); values.push_back(&inst);
            result.interference.try_emplace(&inst);
        }
    }
    result.stats.values = values.size();
    if (instructions.empty()) return result;

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
                if (Eligible(source)) {
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
        for (auto* value : live.At(inst)) if (Eligible(value)) together.insert(value);
        for (auto* value : live.After(inst)) if (Eligible(value)) together.insert(value);
        for (auto* value : result.definitions.at(inst)) together.insert(value);
        result.stats.max_pressure = std::max(result.stats.max_pressure, u32(together.size()));
        // Before/after are deliberately one clique. Emitters may write a
        // result before rereading an input to compute flags or another half.
        for (auto* left : together) for (auto* right : together) if (left != right) {
            if (!indices.contains(left) || !indices.contains(right))
                throw std::runtime_error("foreign value in LoongArch scalar liveness");
            result.interference.at(left).insert(right);
        }
        for (auto* value : result.inputs.at(inst)) { costs[indices.at(value)] += weights[i] * 4; ++result.stats.input_sites; }
        for (auto* value : result.definitions.at(inst)) { costs[indices.at(value)] += weights[i]; ++result.stats.definition_sites; }
        // Phi edge reads and terminal uses are already in the liveness sets.
        // Give them cost too, without confusing a lexical hint with deadness.
        for (auto* value : live.ForBranch(inst)) if (Eligible(value)) costs[indices.at(value)] += weights[i];
        if (i + 1 == instructions.size()) for (auto* value : live.ForFallthrough(inst))
            if (Eligible(value)) costs[indices.at(value)] += weights[i] * 4;
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
    result.stats.spilled_values = result.stats.values - result.stats.register_values;
    for (auto* inst : instructions) {
        for (auto* value : result.inputs.at(inst)) if (result.Spilled(value)) ++result.stats.spill_input_sites;
        for (auto* value : result.definitions.at(inst)) if (result.Spilled(value)) ++result.stats.spill_definition_sites;
        if (inst->GetOp() == O::AddPhi && result.Register(inst))
            for (const auto& parameter : inst->GetArg<ir::Params>(0))
                if (parameter.data.IsValue() && result.Register(parameter.data.value.Def()) == result.Register(inst))
                    ++result.stats.phi_coalesced;
    }
    return result;
}

}  // namespace swift::runtime::backend::loongarch64
