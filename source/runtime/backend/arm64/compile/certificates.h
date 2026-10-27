#pragma once

#include <span>
#include "actions.h"
#include "runtime/compile/allocate/fixed_validation.h"
#include "width_contracts.h"

namespace swift::runtime::backend::arm64 {
struct EmissionCertificateInput {
    const ir::Block* block{};
    compile::BlockAnalysisIndex* analysis{};
    std::span<const compile::PhysicalGPRWrite> writes;
};
struct EmissionCertificates {
    WidthCertificates widths;
    std::vector<FlagsOnlySpillAction> flags_only_spills;
};
// Required production proofs. Preparation may request the unique index's
// resolved-use derivation; no physical result or IR can be edited here.
[[nodiscard]] EmissionCertificates PrepareEmissionCertificates(
        std::span<const EmissionCertificateInput>,
        compile::AllocationView,
        const UnitActions&,
        const compile::FixedValidationInputs&,
        const FeatureSet&,
        const GPRSMask& uniforms);
std::vector<u32> ProveZeroStores(compile::BlockAnalysisIndex&,
                                 compile::AllocationView,
                                 std::vector<const ir::Inst*>* incoming = nullptr);
}  // namespace swift::runtime::backend::arm64
