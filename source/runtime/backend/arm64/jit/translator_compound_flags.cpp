#include "translator.h"

namespace swift::runtime::backend::arm64 {

bool JitTranslator::MatchCompoundLogicalClear(ir::Inst* inst) const {
    if (flags_audit_block_edge == FlagsRegsAuditEdgeKind::RegionInternal ||
        backedge_flags_recipe || flag_state.dead_edge_integer_branch || !inst ||
        inst->GetOp() != ir::OpCode::ClearFlags ||
        inst->GetArg<ir::Flags>(0) !=
                (ir::Flags::CV | ir::Flags::AuxiliaryCarry)) {
        return false;
    }

    auto& list = cur_block->GetInstList();
    auto clear = list.iterator_to(*inst);
    if (clear == list.begin()) {
        return false;
    }
    auto producer = std::prev(clear);
    auto save = std::next(clear);
    if (save == list.end() || save->GetOp() != ir::OpCode::SaveFlags ||
        save->GetArg<ir::Value>(0).Def() != &*producer ||
        save->GetArg<ir::Flags>(1) !=
                (ir::Flags::NZ | ir::Flags::Parity)) {
        return false;
    }
    for (auto* pseudo : producer->GetPseudoOperations()) {
        if (pseudo->GetOp() == ir::OpCode::BranchOnlyFlags) {
            return false;
        }
    }
    switch (producer->GetOp()) {
        case ir::OpCode::And:
        case ir::OpCode::AndNot:
        case ir::OpCode::Or:
        case ir::OpCode::Xor:
            return true;
        default:
            return false;
    }
}

bool JitTranslator::MatchCompoundZeroLogicalClear(ir::Inst* inst) const {
    if (!MatchCompoundLogicalClear(inst)) {
        return false;
    }
    auto& list = cur_block->GetInstList();
    auto producer = std::prev(list.iterator_to(*inst));
    return scalar_copy_analysis.IsSelfXor(&*producer);
}

}  // namespace swift::runtime::backend::arm64
