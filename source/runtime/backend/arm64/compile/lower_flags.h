#pragma once

#include <span>
#include "actions.h"

namespace swift::runtime::compile {
class BlockAnalysisIndex;
class PseudoFlagsPlan;
class CompileWorkspace;
}  // namespace swift::runtime::compile
namespace swift::runtime::ir {
class Inst;
}
namespace swift::runtime::backend::arm64 {

struct ConsumerActions;

// Preparation-only prefix facts. Emission retains the compact availability
// masks in ConsumerAction; the first AdvancePC is needed only by lowerings.
// Temporary node identity rejects a foreign block reusing the same numeric ID.
struct PstateEligibility final {
    struct Test {
        const ir::Inst* instruction;
        u8 mask;
        bool operator==(const Test&) const = default;
    };
    std::vector<Test> tests;
    std::optional<u32> first_advance;
    [[nodiscard]] ir::Flags At(const ir::Inst*) const;
};
PstateEligibility PreparePstateEligibility(const compile::BlockAnalysisIndex&);

// Only IDs and final conditions survive preparation. Carrier-window discovery
// shares the numbered block's use graph with the other target lowerings.
struct CarryActions final {
    std::vector<CarryAction> actions;
    [[nodiscard]] const CarryAction* Find(u32 id) const;
};
CarryActions LowerCarryConditions(const ir::Block*,
                                  compile::BlockAnalysisIndex&,
                                  const FeatureSet&,
                                  const ConsumerActions&);

// Preparation-only sparse certificates. Producer and capture IDs belong to
// the same numbered block; mutable token validity remains emission state.
struct FlagsTokenActions final {
    std::vector<FlagsTokenAction> retentions;
    [[nodiscard]] bool Retains(u32 producer, u32 home, u32 point) const {
        return FlagsTokenAction::Retains(retentions, producer, home, point);
    }
};

FlagsTokenActions LowerFlagsTokens(compile::BlockAnalysisIndex&,
                                   compile::CompileWorkspace&,
                                   compile::AllocationView,
                                   const FeatureSet&,
                                   std::span<const ir::Inst* const> publications,
                                   const compile::PseudoFlagsPlan& demands);
}  // namespace swift::runtime::backend::arm64
