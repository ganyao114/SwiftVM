#include "translator.h"

namespace swift::runtime::backend::arm64 {
std::optional<u16> JitTranslator::PlannedCompoundProducer(const ir::Inst* inst) const {
    if (flags_audit_block_edge == FlagsRegsAuditEdgeKind::RegionInternal || backedge_flags_recipe ||
        DeadEdgePlan())
        return std::nullopt;
    const auto* row = context.GetActions().Consumer(inst->Id());
    return row && row->Has(ConsumerFlag::CompoundClear) ? std::optional<u16>{row->related}
                                                        : std::nullopt;
}
}  // namespace swift::runtime::backend::arm64
