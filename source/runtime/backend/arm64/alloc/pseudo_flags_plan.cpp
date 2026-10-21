#include "pseudo_flags_plan.h"

namespace swift::runtime::backend::arm64 {
void PseudoFlagsPlan::RecordPseudo(const ir::Inst& pseudo, u16 first, u16 last) {
    static_assert(static_cast<u64>(ir::Flags::All | ir::Flags::Positive) == 0x7f);
    ASSERT(pseudo.GetOp() == ir::OpCode::SaveFlags ||
           pseudo.GetOp() == ir::OpCode::BranchOnlyFlags);
    auto* owner = pseudo.GetArg<ir::Value>(0).Def();
    if (!owner || !owner->HasValue()) return;
    if (owner->Id() < first || owner->Id() > last) return;
    const auto flags = pseudo.GetArg<ir::Flags>(1);
    const bool branch_only = pseudo.GetOp() == ir::OpCode::BranchOnlyFlags;
    if (flags == ir::Flags::None && !branch_only) return;
    ASSERT((static_cast<u64>(flags) & ~u64{0x7f}) == 0);
    if (packed.empty()) {
        first_id = first;
        packed.resize(last - first + 1);
    }
    packed[owner->Id() - first_id] |= static_cast<u16>(static_cast<u64>(flags) |
            (branch_only ? 0x80 : 0) |
            (!branch_only && True(flags & ir::Flags::Parity) ? 0x8000 : 0));
}
}
