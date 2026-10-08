#pragma once

#include <functional>

#include "runtime/backend/reg_alloc.h"
#include "runtime/backend/arm64/jit/block_analysis_index.h"
#include "runtime/backend/arm64/jit/guest_state_map.h"

namespace swift::runtime::backend::arm64 {

struct FixedGPRTransfer {
    ir::Inst* read{};
    ir::Inst* publication{};
    std::vector<ir::Inst*> aliases{};
    u16 source{};
    u16 target{};
    u32 last_use{};
    bool operator==(const FixedGPRTransfer&) const = default;
};

bool IsFixedGPRPublicationObserver(ir::OpCode op);
bool IsFixedGPRPublicationObserver(const ir::Inst& inst, const FeatureSet& features);

// Semantic matchers shared by preallocation and target preparation. Neither
// materializes registers, changes the IR, nor relies on an allocation result.
std::optional<FixedGPRTransfer> MatchFixedGPRTransfer(
        ir::Inst* publication, const BlockAnalysisIndex& uses, const GuestStateMap& state);
std::optional<u16> MatchNarrowFixedGPRRead(
        ir::Inst& read, const BlockAnalysisIndex& uses, const GuestStateMap& state,
        const std::function<ir::Flags(ir::Inst*)>& set_flags);

void PrepareFixedGPRConstraints(ir::Block* block, RegAlloc& allocation,
                                const FeatureSet& features);
void PrepareFixedGPRConstraints(ir::HIRFunction* function, RegAlloc& allocation,
                                const FeatureSet& features);
// Independent check of the final physical writers, including earlier writes
// introduced by other coalescers. Run once per completed allocation attempt.
void VerifyFixedGPRConstraints(ir::Block* block, const RegAlloc& allocation,
                               const FeatureSet& features);

}  // namespace swift::runtime::backend::arm64
