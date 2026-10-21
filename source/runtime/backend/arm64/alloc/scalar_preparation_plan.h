#pragma once

#include "runtime/backend/arm64/jit/resident_scalar_fpr_analysis.h"
#include "runtime/backend/arm64/jit/scalar_copy_analysis.h"
#include "runtime/backend/arm64/jit/raw_carry_branch_analysis.h"

namespace swift::runtime::backend::arm64 {

// Constructed by target allocation planning; emission receives a const view.
// These IR-only decisions deliberately contain no emission state or callbacks.
struct ScalarPreparationPlan {
    ResidentScalarFPRAnalysis resident;
    ScalarCopyAnalysis copies;
    RawCarryBranchAnalysis raw_carry;

    [[nodiscard]] bool Empty() const {
        return resident.Empty() && copies.Empty() && raw_carry.Empty();
    }
};

}  // namespace swift::runtime::backend::arm64
