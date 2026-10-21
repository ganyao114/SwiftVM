#pragma once

#include <stdexcept>
#include "runtime/ir/instr.h"

namespace swift::runtime::backend::riscv64 {

constexpr u32 RVUniformValueWidth(ir::ValueType type) {
    switch (type) {
        case ir::ValueType::U8: case ir::ValueType::S8: case ir::ValueType::V8: return 1;
        case ir::ValueType::U16: case ir::ValueType::S16: case ir::ValueType::V16: return 2;
        case ir::ValueType::U32: case ir::ValueType::S32: case ir::ValueType::V32: return 4;
        case ir::ValueType::U64: case ir::ValueType::S64: case ir::ValueType::V64: return 8;
        case ir::ValueType::V128: return 16;
        case ir::ValueType::V256: return 32;
        default: return 0;
    }
}

// Check the final storage view before widening or emitting any code.
inline void VerifyRVScalarUniformSpanForOpcode(const ir::Inst& inst, ir::OpCode op, u32 capacity) {
    const auto type = op == ir::OpCode::StoreUniform
            ? inst.GetArg<ir::Value>(1).Type() : inst.ReturnType();
    const auto width = RVUniformValueWidth(type);
    if (!width) return;
    const auto offset = inst.GetArg<ir::Uniform>(0).GetOffset();
    if (offset > capacity || width > capacity - offset)
        throw std::runtime_error("RV64 scalar Uniform access exceeds configured state span");
}

}  // namespace swift::runtime::backend::riscv64
