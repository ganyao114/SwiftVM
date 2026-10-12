#pragma once

#include "runtime/common/unsigned_estimate.h"

namespace swift::arm64 {

enum class UnsignedEstimateOperation { Reciprocal, ReciprocalSquareRoot };

// URECPE and URSQRTE interpret a lane as an unsigned fixed-point fraction.
// Their quantization shares ARM's midpoint tables with the FP estimates,
// but they never inspect FPCR or change FPSR.
inline u32 EvaluateUnsignedEstimate(u32 value, UnsignedEstimateOperation operation) {
    return runtime::EvaluateUnsignedEstimateLane(value, u32(operation));
}

}  // namespace swift::arm64
