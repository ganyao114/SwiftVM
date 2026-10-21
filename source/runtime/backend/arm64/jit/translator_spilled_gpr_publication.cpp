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
    const auto found = gpr_plan->publications.find(inst);
    ASSERT(found != gpr_plan->publications.end());
    return found->second.adopts_spill;
}

}  // namespace swift::runtime::backend::arm64
