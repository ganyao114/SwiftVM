#pragma once
#include <span>
#include "frozen_gpr_plan.h"
#include "gpr_width_verifier.h"
#include "flags_only_spill_plan.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::backend::arm64 {
struct GPRPlanVerificationInput {
    ir::Block* block{};
    const FrozenGPRPlan* plan{};
    const BlockAnalysisIndex* analysis{};
    FlagsOnlySpillPlan* flags_only_spills{};
};
// Computes only the width certificates and flags-only spill plans the
// allocator consumes, skipping the pure plan audit (see SVM_VERIFY_PLAN).
void VerifyGPRWidthElisionsOnly(std::span<const GPRPlanVerificationInput> blocks,
                                const RegAlloc& allocation, const FeatureSet& features,
                                GPRWidthCertificates* certificates = nullptr,
                                const GPRSMask& uniforms = GPRSMask{});
void VerifyGPRAllocationPlan(std::span<const GPRPlanVerificationInput> blocks,
                              const RegAlloc& allocation, const FeatureSet& features,
                              ir::HIRFunction* function = nullptr, bool internal_edges = true,
                              GPRWidthCertificates* certificates = nullptr,
                              const GPRSMask& uniforms = GPRSMask{}, bool fresh_snapshots = false);
}
