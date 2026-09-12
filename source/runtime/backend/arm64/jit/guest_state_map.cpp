#include "guest_state_map.h"

#include "runtime/backend/arm64/helper_call_contract.h"

namespace swift::runtime::backend::arm64 {

void GuestStateMap::Analyze(ir::Block* next_block,
                            const FeatureSet& next_features) {
    block = next_block;
    features = next_features;
    block_entry_width_facts = {};
    if (const auto found = function_entry_width_facts.find(block);
        found != function_entry_width_facts.end()) {
        block_entry_width_facts = found->second;
    }
    fault_capture_values.clear();
    fault_width_captures.clear();
    fixed_home_uses.clear();
    fixed_home_use_counts.clear();
    registered_fixed_home_uses.clear();
}

bool GuestStateMap::ClobbersFixedHome(const ir::Inst& inst,
                                      u32 home) const {
    if (inst.GetOp() == ir::OpCode::SetHostGPR &&
        inst.GetArg<ir::Imm>(1).Get() == home) {
        return true;
    }
    return home <= 9 && HelperCallContract::InstructionClobbersGPR(inst, home,
                                                                   features);
}

GuestStateMap::ExtensionFacts GuestStateMap::CurrentEntryExtensionFacts(
        u32 home) const {
    return home < block_entry_width_facts.size()
            ? block_entry_width_facts[home]
            : ExtensionFacts{};
}

bool GuestStateMap::FixedHomeSurvives(u32 home,
                                      u32 after,
                                      u32 before) const {
    ASSERT(block);
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() > after && inst.Id() < before &&
            ClobbersFixedHome(inst, home)) {
            return false;
        }
    }
    return true;
}

bool GuestStateMap::PublicationWindowSafe(
        u32 home,
        ir::Value early_value,
        u32 after,
        u32 before,
        const ir::Inst* ignored) const {
    ASSERT(block);
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() <= after || inst.Id() >= before || &inst == ignored) {
            continue;
        }
        if (ClobbersFixedHome(inst, home)) {
            return false;
        }
        if (MayFaultOrObserve(inst) &&
            !FaultCaptureContains(
                    inst, home, early_value.Def(),
                    ir::GetValueSizeByte(early_value.Type()) <= sizeof(u32))) {
            return false;
        }
    }
    return true;
}

bool GuestStateMap::MayFaultOrObserve(const ir::Inst& inst) const {
    const auto helper = HelperCallContract::Resolve(inst, features);
    return helper ? helper->RequiresGuestStatePublication()
                  : MayFaultOrObserve(inst.GetOp());
}

bool GuestStateMap::MayFaultOrObserve(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::LoadMemory:
        case ir::OpCode::StoreMemory:
        case ir::OpCode::LoadMemoryTSO:
        case ir::OpCode::StoreMemoryTSO:
        case ir::OpCode::MemoryCopy:
        case ir::OpCode::MemoryCopyTSO:
        case ir::OpCode::CompareAndSwap:
        case ir::OpCode::CompareAndSwap128:
        case ir::OpCode::CheckMemoryAlignment:
        case ir::OpCode::AtomicExchange:
        case ir::OpCode::AtomicFetchAdd:
        case ir::OpCode::AtomicRMW:
        case ir::OpCode::CallLambda:
        case ir::OpCode::CallLocation:
        case ir::OpCode::CallDynamic:
        case ir::OpCode::X87Op:
        case ir::OpCode::Sse42Str:
            return true;
        default:
            return false;
    }
}

}  // namespace swift::runtime::backend::arm64
