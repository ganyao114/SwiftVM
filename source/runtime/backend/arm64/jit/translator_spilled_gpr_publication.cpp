#include "translator.h"

namespace swift::runtime::backend::arm64 {

bool JitTranslator::CanAdoptPendingSpillWrite(
        ir::Inst* inst, bool reads_forwarded_memory_input) {
    if (!inst) {
        return false;
    }
    switch (inst->GetOp()) {
        case ir::OpCode::LoadMemory:
        case ir::OpCode::StoreMemory:
        case ir::OpCode::LoadMemoryTSO:
        case ir::OpCode::StoreMemoryTSO:
            return reads_forwarded_memory_input;
        default:
            break;
    }
    if (inst->GetOp() != ir::OpCode::SetHostGPR) {
        return true;
    }
    if (pinned_gprs.GetRecipes().select_publications.contains(inst) ||
        pinned_gprs.GetRecipes().spilled_publications.contains(inst) ||
        pinned_gprs.GetRecipes().dead_writes.contains(inst) ||
        pinned_gprs.GetRecipes().transfers.contains(inst) ||
        pinned_gprs.GetRecipes().copies.contains(inst) ||
        context.IsHostWriteCoalesced(inst->Id())) {
        return false;
    }
    if (auto update = pinned_load_update_instructions.find(inst);
        update != pinned_load_update_instructions.end() &&
        inst == update->second.publication) {
        return false;
    }
    const auto published = inst->GetArg<ir::Value>(0);
    if (auto update = MatchBiasedMemoryUpdate(published.Def());
        update && update->publication == inst) {
        return false;
    }
    if (auto update = MatchPreIndexMemoryUpdate(published.Def());
        update && update->publication == inst) {
        return false;
    }
    if (published.Def() && pinned_gprs.IsZeroExtendElided(published.Def())) {
        return false;
    }
    return !pinned_gprs.FixedHomeForUse(published, inst).has_value();
}

}  // namespace swift::runtime::backend::arm64
