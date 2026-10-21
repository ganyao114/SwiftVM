#include "scalar_fpr_publication_plan.h"
#include "runtime/backend/arm64/jit/block_analysis_index.h"
#include <array>
#include <algorithm>
#include <iterator>

namespace swift::runtime::backend::arm64 {
namespace {
bool IsFusionBarrier(ir::OpCode op) {
    using O = ir::OpCode;
    switch (op) {
        case O::LoadMemory: case O::StoreMemory: case O::LoadMemoryTSO: case O::StoreMemoryTSO:
        case O::MemoryCopy: case O::MemoryCopyTSO: case O::CompareAndSwap: case O::CompareAndSwap128:
        case O::CheckMemoryAlignment: case O::AtomicExchange: case O::AtomicFetchAdd: case O::AtomicRMW:
        case O::CallLambda: case O::CallLocation: case O::CallDynamic: case O::X87Op: case O::Sse42Str:
        case O::GetUniformAddress: case O::UniformBarrier: case O::Goto: case O::NotGoto: case O::BindLabel:
            return true;
        default: return false;
    }
}
bool PublicationShape(const ScalarFPRPublication& p) {
    return p.low_store && p.high_store && p.zero && p.target >= 16 && p.target <= 31 &&
           p.low_store->GetOp() == ir::OpCode::SetHostFPR && p.high_store->GetOp() == ir::OpCode::SetHostFPR &&
           p.zero->GetOp() == ir::OpCode::LoadImm && p.zero->ReturnType() == ir::ValueType::U64 &&
           p.zero->GetArg<ir::Imm>(0).Get() == 0 &&
           p.low_store->GetArg<ir::Imm>(1).Get() == p.target && p.high_store->GetArg<ir::Imm>(1).Get() == p.target &&
           p.low_store->GetArg<ir::Imm>(2).Get() == 0 && p.high_store->GetArg<ir::Imm>(2).Get() == sizeof(u64) &&
           p.high_store->GetArg<ir::Value>(0).Def() == p.zero && p.low_store->Id() + 1 == p.high_store->Id();
}
struct Owner { u32 birth, end; bool external_uses; };
}

ScalarFPRPublicationPlan BuildScalarFPRPublicationPlan(ir::Block* block, const RegAlloc& allocation,
        const BlockAnalysisIndex* analysis) {
    ScalarFPRPublicationPlan plan;
    const bool needed = std::any_of(block->GetInstList().begin(), block->GetInstList().end(), [](const ir::Inst& inst) {
        if (inst.GetOp() != ir::OpCode::SetHostFPR || inst.GetArg<ir::Imm>(2).Get() != 0) return false;
        const auto value = inst.GetArg<ir::Value>(0);
        return value.Defined() && !ir::IsFloatValueType(value.Type()) && ir::GetValueSizeByte(value.Type()) == 8;
    });
    if (!needed) return plan;
    BlockAnalysisIndex owned;
    if (!analysis) owned.Build(block);
    const auto& index = analysis ? *analysis : owned;
    std::array<std::vector<Owner>, 32> owners;
    for (auto* inst : index.Instructions()) {
        if (!inst->HasValue() || inst->IsBitCastOperation() || !ir::IsFloatValueType(inst->ReturnType())) continue;
        const ir::Value value{inst};
        if (allocation.ValueType(value) != RegAlloc::FPR) continue;
        const auto target = allocation.ValueFPR(value).id;
        if (target >= 32) continue;
        owners[target].push_back({inst->Id(), index.LastInstructionUse(inst),
                                 inst->GetUses(false) > index.DirectUseCount(inst)});
    }
    std::map<ir::Inst*, u32> zero_uses;
    auto& list = block->GetInstList();
    for (auto it = list.begin(); it != list.end(); ++it) {
        auto* low = it.operator->();
        if (low->GetOp() != ir::OpCode::SetHostFPR || low->GetArg<ir::Imm>(2).Get() != 0) continue;
        const auto next = std::next(it);
        if (next == list.end()) continue;
        auto* high = next.operator->();
        if (high->GetOp() != ir::OpCode::SetHostFPR) continue;
        ScalarFPRPublication p{low, high, high->GetArg<ir::Value>(0).Def(), nullptr,
                               static_cast<u16>(low->GetArg<ir::Imm>(1).Get())};
        if (!PublicationShape(p)) continue;
        auto* load = low->GetArg<ir::Value>(0).Def();
        if (load && load->GetOp() == ir::OpCode::ZeroExtend64) {
            p.load_extension = load; load = load->GetArg<ir::Value>(0).Def();
        }
        bool load_ok = load && load->GetOp() == ir::OpCode::LoadMemory && load->GetUses(false) == 1 &&
                       load->Id() < low->Id();
        if (load_ok) {
            load_ok = p.load_extension
                    ? load->ReturnType() == ir::ValueType::U32 && p.load_extension->GetUses(false) == 1 &&
                      p.load_extension->GetArg<ir::Value>(0).Def() == load &&
                      low->GetArg<ir::Value>(0).Def() == p.load_extension &&
                      load->Id() < p.load_extension->Id() && p.load_extension->Id() < low->Id()
                    : load->ReturnType() == ir::ValueType::U64 && low->GetArg<ir::Value>(0).Def() == load;
        }
        if (load_ok) {
            for (auto* node : index.Between(load->Id(), high->Id() + 1)) {
                if (node == low || node == high || node == p.zero || node == p.load_extension) continue;
                if (IsFusionBarrier(node->GetOp()) ||
                    ((node->GetOp() == ir::OpCode::GetHostFPR || node->GetOp() == ir::OpCode::SetHostFPR) &&
                     node->GetArg<ir::Imm>(node->GetOp() == ir::OpCode::GetHostFPR ? 0 : 1).Get() == p.target)) {
                    load_ok = false; break;
                }
            }
            for (const auto& owner : owners[p.target])
                if (owner.external_uses || (owner.birth <= high->Id() && owner.end > load->Id())) load_ok = false;
        }
        if (load_ok) {
            plan.loads.emplace(load, p);
            if (p.load_extension) plan.covered.push_back(p.load_extension);
            plan.covered.push_back(low); plan.covered.push_back(high);
        } else {
            const auto value = low->GetArg<ir::Value>(0);
            if (!value.Defined() || ir::IsFloatValueType(value.Type()) || ir::GetValueSizeByte(value.Type()) != 8) continue;
            plan.values.emplace(low, p); plan.covered.push_back(high);
        }
        ++zero_uses[p.zero];
    }
    for (const auto& [zero, uses] : zero_uses)
        if (zero->GetUses(false) == uses) plan.covered.push_back(zero);
    return plan;
}
}
