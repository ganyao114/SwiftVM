#pragma once

#include <vector>
#include "runtime/backend/riscv64/jit/jit_context.h"
#include "runtime/ir/block.h"

namespace swift::runtime::backend::riscv64 {

struct RetirementPhaseHome {
    const ir::Inst* value{};
    ir::ValueType type{};
    u32 offset{};
    u32 bytes{}, parts{};
};

struct RetirementPhaseHead {
    const ir::Inst* label{};
    u64 guest_pc{};
    std::vector<RetirementPhaseHome> live;
    std::vector<const ir::Inst*> backedges;
};

struct RetirementPhasePlan {
    u64 ir_fingerprint{};
    u32 reachable_instructions{};
    std::vector<RetirementPhaseHead> heads;
    [[nodiscard]] const RetirementPhaseHead* Find(const ir::Inst* label) const;
};

// Throws before body emission if any reachable internal backedge is unproved.
// This descriptor authorizes exit only; it has no continuation or lifetime token.
[[nodiscard]] RetirementPhasePlan PrepareRetirementPhases(
        ir::Block& block, const JitContext& context, bool has_phi_moves, bool has_function_flow);

}  // namespace swift::runtime::backend::riscv64
