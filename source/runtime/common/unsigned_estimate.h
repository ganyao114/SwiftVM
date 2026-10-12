#pragma once

#include <stdexcept>
#include "runtime/common/estimate_tables.h"
#include "runtime/ir/instr.h"

namespace swift::runtime {

// ARM unsigned fixed-point estimate; lane input/output are raw u32 bits.
// The reciprocal saturates below 1/2; reciprocal-root below 1/4. These
// operations never inspect FPCR or publish flags/FPSR exceptions.
inline u32 EvaluateUnsignedEstimateLane(u32 value, u32 operation) {
    if (!operation) {
        if (!(value & 0x80000000u)) return UINT32_MAX;
        return u32(kReciprocalMidpoints[(value >> 23) - 256]) << 23;
    }
    if (!(value & 0xc0000000u)) return UINT32_MAX;
    const bool lower = !(value & 0x80000000u);
    const u32 index = value >> (lower ? 23 : 24);
    return u32(kReciprocalRootMidpoints[u32(lower)][index - 128]) << 23;
}

inline u32 ValidateUnsignedEstimate(ir::Inst* inst) {
    if (!inst->ArgAt(1).IsImm() || inst->GetArg<ir::Imm>(1).Get() > 1)
        throw std::runtime_error("VecUnsignedEstimate requires exact immediate operation 0 or 1");
    if (inst->ReturnType() != ir::ValueType::V128 || !inst->ArgAt(0).IsValue())
        throw std::runtime_error("VecUnsignedEstimate requires a V128 result and input");
    const auto input = inst->GetArg<ir::Value>(0);
    if (!input.Defined() || input.Type() != ir::ValueType::V128 || input.Def()->ReturnType() != ir::ValueType::V128)
        throw std::runtime_error("VecUnsignedEstimate requires canonical V128 input view and producer");
    return u32(inst->GetArg<ir::Imm>(1).Get());
}
}  // namespace swift::runtime
