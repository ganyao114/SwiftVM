#pragma once

#include <functional>

#include "runtime/backend/arm64/compile/register_state.h"
#include "runtime/backend/register_mask.h"
#include "runtime/compile/allocate/constraints.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/workspace.h"

namespace swift::runtime::compile {
class SemanticFacts;
}  // namespace swift::runtime::compile
namespace swift::runtime::backend::arm64 {

// One matcher call owns its alias queue until its caller submits the result.
// A direct transfer has no queue and does not borrow the shared buffer.
class FixedGPRTransferUses final {
public:
    explicit FixedGPRTransferUses(compile::CompileWorkspace& workspace) : workspace(workspace) {}
    FixedGPRTransferUses(const FixedGPRTransferUses&) = delete;
    FixedGPRTransferUses(FixedGPRTransferUses&&) = delete;
    FixedGPRTransferUses& operator=(const FixedGPRTransferUses&) = delete;
    FixedGPRTransferUses& operator=(FixedGPRTransferUses&&) = delete;
    void Reset() { candidates.reset(); }
    void Append(u16 position) {
        if (!candidates) candidates.emplace(workspace);
        candidates->Entries().push_back({position, 0});
    }
    [[nodiscard]] std::span<const compile::LoweringCandidate> Aliases() const {
        return candidates ? candidates->Entries() : std::span<const compile::LoweringCandidate>{};
    }

private:
    compile::CompileWorkspace& workspace;
    std::optional<compile::LoweringCandidateScope> candidates;
};

struct FixedGPRTransfer {
    const ir::Inst* read{};
    std::span<const compile::LoweringCandidate> aliases;
    u16 source{};
    u16 target{};
    u32 last_use{};
};

bool IsFixedGPRPublicationObserver(ir::OpCode op);
bool IsFixedGPRPublicationObserver(const ir::Inst& inst, const FeatureSet& features);

// Shared mutable match preparation may request use/home-access indices.
// It never materializes registers, changes IR or relies on an allocation result.
// A transfer's alias view ends when its explicit use scope resets or returns.
std::optional<FixedGPRTransfer> MatchFixedGPRTransfer(const ir::Inst* publication,
                                                      compile::BlockAnalysisIndex& uses,
                                                      RegisterStatePreparation& state,
                                                      FixedGPRTransferUses& aliases);
std::optional<u16> MatchNarrowFixedGPRRead(
        const ir::Inst& read,
        compile::BlockAnalysisIndex& uses,
        RegisterStatePreparation& state,
        const std::function<ir::Flags(const ir::Inst*)>& set_flags,
        bool allow_widen_extend = false);

// Produced once from sealed semantic IR and the declared target pool. These
// records carry no ordinary mappings and are not regenerated on physical retry.
[[nodiscard]] Vector<compile::FixedGPRInput> BuildFixedGPRInputs(const ir::Block*,
                                                                 GPRSMask,
                                                                 const FeatureSet&,
                                                                 compile::SemanticFacts&,
                                                                 compile::CompileWorkspace&);
[[nodiscard]] Vector<compile::FixedGPRInput> BuildFixedGPRInputs(const ir::HIRFunction*,
                                                                 GPRSMask,
                                                                 const FeatureSet&,
                                                                 compile::SemanticFacts&,
                                                                 compile::CompileWorkspace&);

}  // namespace swift::runtime::backend::arm64
