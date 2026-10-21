#include "translator.h"

namespace swift::runtime::backend::arm64 {
ir::Inst* JitTranslator::PlannedCompoundProducer(ir::Inst* inst) const {
    if (flags_audit_block_edge == FlagsRegsAuditEdgeKind::RegionInternal ||
        backedge_flags_recipe || DeadEdgePlan() || !gpr_plan->consumers) return nullptr;
    const auto found = gpr_plan->consumers->compound_clears.find(inst);
    return found == gpr_plan->consumers->compound_clears.end() ? nullptr : found->second;
}
}
