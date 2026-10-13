#include "vector_allocation_plan.h"

#include <algorithm>
#include <stdexcept>

namespace swift::runtime::backend::loongarch64 {
namespace {
using O = ir::OpCode;
bool Binary(O op) {
    switch (op) {
        case O::Vec4Add: case O::Vec4Sub: case O::Vec4Mul:
        case O::Vec4And: case O::Vec4Or:
        case O::VecAdd: case O::VecSub: case O::VecMul:
        case O::VecAnd: case O::VecOr: case O::VecXor: case O::VecAndNot:
        case O::VecCmpEq: case O::VecCmpGt: case O::VecMin: case O::VecMax:
        case O::VecAvg: case O::VecSatAdd: case O::VecSatSub:
        case O::VecMulHigh16: return true;
        default: return false;
    }
}
} // namespace

bool VectorAllocationPlan::Eligible(ir::Inst* inst) {
    if (!inst || inst->ReturnType() < ir::ValueType::V8 || inst->ReturnType() > ir::ValueType::V128) return false;
    // Producer-written pseudo results retain their canonical ABI; their
    // lexical no-op marker is not a new physical definition.
    const auto op = inst->GetOp();
    return op != O::VecSatMulQC && op != O::VecSatShiftQC && op != O::Div128Remainder && op != O::CpuidUpper;
}

bool VectorAllocationPlan::PreservesRegisters(ir::Inst* inst) {
    const auto op = inst->GetOp();
    switch (op) {
        case O::Goto: case O::NotGoto: case O::BindLabel: case O::AddPhi:
        case O::Nop: case O::AdvancePC: case O::Checkpoint: case O::SetLocation: case O::GetLocation:
        case O::UniformBarrier: case O::XchgBarrier: case O::PushRSB: case O::PopRSB:
        case O::CallReturn: case O::GetUniformAddress: return true;
        case O::LoadUniform: return ir::GetValueSizeByte(inst->ReturnType()) <= 16;
        case O::StoreUniform: return ir::GetValueSizeByte(inst->GetArg<ir::Value>(1).Type()) <= 16;
        case O::Zero: case O::BitCast: case O::GetResult:
        case O::Select: case O::SelectZero: case O::CondSelect:
            return ir::GetValueSizeByte(inst->ReturnType()) <= 16;
        default: break;
    }
    // The scalar positive list uses GPRs for integer/VOID annotations. FP and
    // vector annotations may use typed helpers and must take the ABI bridge.
    if (inst->ReturnType() > ir::ValueType::S64) return false;
    switch (op) {
#define NATIVE(name) case O::name: return true;
#include "scalar_native.inc"
#undef NATIVE
        default: return false;
    }
}

std::optional<u32> VectorAllocationPlan::Register(ir::Inst* inst) const {
    const auto found = registers.find(inst);
    return found == registers.end() ? std::nullopt : std::optional{found->second};
}

bool VectorAllocationPlan::Native128(ir::Inst* inst, const HostFeatures& features) {
    if (!features.lsx || !inst || inst->ReturnType() == ir::ValueType::V256 ||
        inst->ReturnType() == ir::ValueType::VOID) return false;
    if (Binary(inst->GetOp())) return true;
    switch (inst->GetOp()) {
        case O::VecShiftLeftImm: case O::VecShiftRightImm: case O::VecShiftRightArithmeticImm:
        case O::VecShiftLeft: case O::VecShiftRight: case O::VecShiftRightArithmetic:
        case O::VecShuffle32: case O::VecDupPairs32:
        case O::VecZip: case O::VecExtractBytes: return true;
        // VecByteShift's zero/count==16 paths still use a raw-pair emitter.
        default: return false;
    }
}

VectorAllocationPlan VectorAllocationPlan::Build(ir::Block* block, const HostFeatures& features,
        std::span<const WideVectorPair> wide, const RegisterPlan::Targets& targets,
        const RegisterPlan::Stops& stops, const RegisterPlan::Exits& exits, bool cfg_allocation) {
    if (!block) throw std::invalid_argument("LoongArch vector plan needs an IR block");
    VectorAllocationPlan result;
    result.liveness = RegisterPlan::Build(block, targets, stops, {}, exits);
    std::vector<ir::Inst*> order;
    std::unordered_map<ir::Inst*, u32> positions;
    for (auto& inst : block->GetInstList()) {
        positions.emplace(&inst, order.size()); order.push_back(&inst);
        if (Native128(&inst, features)) result.region.insert(&inst);
    }
    // Validate provenance before admitting any pair. No ID mutation and no
    // assumption about sparse-home adjacency are made by this analysis.
    std::unordered_set<ir::Inst*> ownership;
    for (const auto& pair : wide) {
        const auto low = positions.find(pair.low), high = positions.find(pair.high);
        if (low == positions.end() || high == positions.end() || high->second != low->second + 1 ||
            pair.low->GetOp() != pair.high->GetOp() || !Binary(pair.low->GetOp()) ||
            pair.low->ReturnType() != ir::ValueType::V128 || pair.high->ReturnType() != ir::ValueType::V128)
            throw std::invalid_argument("LoongArch LASX pair lacks adjacent V256 provenance");
        for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
            auto& a = pair.low->ArgAt(slot);
            auto& b = pair.high->ArgAt(slot);
            if (a.GetType() != b.GetType() ||
                (a.IsImm() && a.Get<ir::Imm>().Get() != b.Get<ir::Imm>().Get()) ||
                (!a.IsValue() && !a.IsImm() && !a.IsVoid()))
                throw std::invalid_argument("LoongArch LASX pair has mismatched lane controls");
        }
        for (u32 side = 0; side < 2; ++side) {
            const auto& inputs = side ? pair.right : pair.left;
            if (!positions.contains(inputs[0]) || !positions.contains(inputs[1]) ||
                positions.at(inputs[0]) >= low->second || positions.at(inputs[1]) >= low->second ||
                pair.low->GetArg<ir::Value>(side).Def() != inputs[0] ||
                pair.high->GetArg<ir::Value>(side).Def() != inputs[1] ||
                inputs[0]->ReturnType() != ir::ValueType::V128 || inputs[1]->ReturnType() != ir::ValueType::V128)
                throw std::invalid_argument("LoongArch LASX pair has foreign or mismatched input halves");
        }
        if (!ownership.insert(pair.low).second || !ownership.insert(pair.high).second)
            throw std::invalid_argument("LoongArch LASX pair has overlapping ownership");
        if (!features.lasx || !features.lsx) continue;
        result.pairs.emplace(pair.low, pair); result.tails.insert(pair.high);
        auto incoming = result.liveness.At(pair.low);
        const auto& high_incoming = result.liveness.At(pair.high);
        incoming.insert(high_incoming.begin(), high_incoming.end());
        result.fused_inputs.emplace(pair.low, std::move(incoming));
    }
    if (cfg_allocation && result.CFGNodes() > 1) result.Allocate(block, targets);
    return result;
}

void VectorAllocationPlan::Allocate(ir::Block* block, const RegisterPlan::Targets& targets) {
    std::vector<ir::Inst*> order, values;
    std::unordered_map<ir::Inst*, u32> positions, indices, labels;
    std::unordered_map<ir::Inst*, LiveSet> definitions;
    for (auto& inst : block->GetInstList()) {
        positions.emplace(&inst, order.size()); order.push_back(&inst);
        definitions.try_emplace(&inst);
        if (inst.GetOp() == O::BindLabel) labels.emplace(inst.GetArg<ir::Value>(0).Def(), order.size() - 1);
        if (Eligible(&inst)) { indices.emplace(&inst, values.size()); values.push_back(&inst); }
    }
    allocation_stats.values = values.size();
    std::vector<std::unordered_set<u32>> graph(values.size());
    for (u32 i = 0; i < order.size(); ++i) {
        auto* inst = order[i];
        if (!Eligible(inst)) continue;
        if (WideTail(inst)) continue;
        if (inst->GetOp() == O::AddPhi) {
            u32 entry = i;
            while (entry && order[entry - 1]->GetOp() == O::AddPhi) --entry;
            if (entry && order[entry - 1]->GetOp() == O::BindLabel) --entry;
            definitions.at(order[entry]).insert(inst);
        } else {
            definitions.at(inst).insert(inst);
            if (auto* pair = WideHead(inst)) definitions.at(inst).insert(pair->high);
        }
    }
    // Loop weighting is a spill preference only. Every interference comes
    // from fixed-point CFG liveness, including phi edge sources and exits.
    std::vector<u32> weights(order.size(), 1);
    for (auto* inst : order) if (inst->GetOp() == O::Goto || inst->GetOp() == O::NotGoto) {
        auto* anchor = targets.contains(inst) ? targets.at(inst) : inst;
        const auto first = labels.contains(anchor) ? labels.at(anchor) : positions.at(anchor);
        const auto last = positions.at(inst);
        if (first <= last) for (u32 i = first; i <= last; ++i) weights[i] = std::min(weights[i] * 8, u32{4096});
    }
    std::vector<u64> costs(values.size(), 1);
    for (u32 i = 0; i < order.size(); ++i) {
        auto* inst = order[i];
        if (WideTail(inst)) continue;
        LiveSet together;
        for (auto* value : At(inst)) if (Eligible(value)) together.insert(value);
        for (auto* value : After(inst)) if (Eligible(value)) together.insert(value);
        together.insert(definitions.at(inst).begin(), definitions.at(inst).end());
        allocation_stats.max_pressure = std::max(allocation_stats.max_pressure, u32(together.size()));
        // Keep resident inputs disjoint from destinations, including raw
        // bridges and fused definitions whose second marker never executes
        // separately. Native LSX emitters can read these inputs directly.
        for (auto* left : together) for (auto* right : together) if (left != right)
            graph.at(indices.at(left)).insert(indices.at(right));
        for (auto* value : together) costs[indices.at(value)] += weights[i];
    }
    std::vector<u32> degrees(values.size()), stack;
    std::vector<bool> remaining(values.size(), true);
    for (u32 i = 0; i < values.size(); ++i) {
        degrees[i] = graph[i].size(); allocation_stats.interference_edges += degrees[i];
    }
    allocation_stats.interference_edges /= 2;
    for (u32 removed = 0; removed < values.size(); ++removed) {
        u32 chosen = UINT32_MAX;
        for (u32 i = 0; i < values.size(); ++i) if (remaining[i] && degrees[i] < kRegisterCount) { chosen = i; break; }
        if (chosen == UINT32_MAX) for (u32 i = 0; i < values.size(); ++i) if (remaining[i] &&
                (chosen == UINT32_MAX || costs[i] * (degrees[chosen] + 1) < costs[chosen] * (degrees[i] + 1))) chosen = i;
        remaining[chosen] = false; stack.push_back(chosen);
        for (auto neighbor : graph[chosen]) if (remaining[neighbor]) --degrees[neighbor];
    }
    std::vector<u32> colors(values.size(), UINT32_MAX);
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        u32 unavailable{};
        for (auto neighbor : graph[*it]) if (colors[neighbor] != UINT32_MAX) unavailable |= u32{1} << colors[neighbor];
        for (u32 reg = 0; reg < kRegisterCount; ++reg) if (!(unavailable & (u32{1} << reg))) {
            colors[*it] = reg; registers.emplace(values[*it], reg); break;
        }
    }
    allocation_stats.register_values = registers.size();
    allocation_stats.spilled_values = values.size() - registers.size();
}

const WideVectorPair* VectorAllocationPlan::WideHead(ir::Inst* inst) const {
    const auto found = pairs.find(inst);
    return found == pairs.end() ? nullptr : &found->second;
}

const LiveSet& VectorAllocationPlan::At(ir::Inst* inst) const {
    // The original high inputs execute early at the low marker. An eviction
    // must not discard a dirty high-only source whose last lexical use is the
    // skipped high marker, even under pressure from loading the other side.
    if (auto found = fused_inputs.find(inst); found != fused_inputs.end()) return found->second;
    return liveness.At(inst);
}

const LiveSet& VectorAllocationPlan::After(ir::Inst* inst) const {
    // Both native halves retire together at the original instruction's
    // second lexical marker. Keep the high half's future uses alive while
    // completing the fused low marker, before the original high definition.
    if (auto* pair = WideHead(inst)) return liveness.After(pair->high);
    return liveness.After(inst);
}

} // namespace swift::runtime::backend::loongarch64
