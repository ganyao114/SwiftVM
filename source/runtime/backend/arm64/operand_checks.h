#pragma once

#include "aarch64/operands-aarch64.h"

namespace swift::runtime::backend::arm64 {

inline bool IsUnmodifiedRegister(const vixl::aarch64::Operand& operand) {
    using namespace vixl::aarch64;
    if (operand.IsShiftedRegister()) return operand.GetShiftAmount() == 0;
    if (!operand.IsExtendedRegister() || operand.GetShiftAmount() != 0) return false;
    return operand.GetExtend() == UXTX || operand.GetExtend() == SXTX;
}

} // namespace swift::runtime::backend::arm64
