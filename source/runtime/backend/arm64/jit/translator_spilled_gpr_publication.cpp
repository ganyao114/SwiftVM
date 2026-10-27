#include "translator.h"

namespace swift::runtime::backend::arm64 {

bool JitTranslator::CanAdoptPendingSpillWrite(const ir::Inst* inst,
                                              bool reads_forwarded_memory_input) {
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
    const auto* action = context.GetActions().Publication(inst->Id());
    ASSERT(action);
    return action->Has(PublicationAction::AdoptsSpill);
}

}  // namespace swift::runtime::backend::arm64
