#pragma once

#include "arm64_frontend.h"
#include "cpu.h"

namespace swift::arm64 {

constexpr ir::ValueType GPRType(bool is64) {
    return is64 ? ir::ValueType::U64 : ir::ValueType::U32;
}

// Single-sided operand: right side is Void (Null) so EmitOperand takes the
// fast path (register or materialized immediate, no composite operation).
// DataClass::ToArgClass() handles Void correctly (maps to ArgClass{}).
inline ir::Operand SingleOperand(const ir::DataClass& data) {
    return ir::Operand{data};
}

}  // namespace swift::arm64
