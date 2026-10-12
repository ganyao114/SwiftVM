#pragma once

#include <array>
#include <cstring>
#include <stdexcept>
#include "runtime/ir/instr.h"

namespace swift::runtime {

struct SaturatingMultiplyShape { u32 bits, mode, lanes; };
inline SaturatingMultiplyShape ValidateSaturatingMultiply(ir::Inst* inst) {
    const auto width = inst->GetArg<ir::Imm>(3).Get();
    const auto control = inst->GetArg<ir::Imm>(4).Get();
    const auto lanes = control >> 2;
    if ((width != 16 && width != 32) || inst->ReturnType() != ir::ValueType::V128 ||
        !(lanes == 1 || lanes == 64 / width || lanes == 128 / width))
        throw std::runtime_error("VecSatMulHigh requires legal V128 width/control");
    for (u32 input = 0; input < 3; ++input)
        if (inst->GetArg<ir::Value>(input).Type() != ir::ValueType::V128 ||
            inst->GetArg<ir::Value>(input).Def()->ReturnType() != ir::ValueType::V128)
            throw std::runtime_error("VecSatMulHigh requires three V128 inputs");
    return {u32(width), u32(control & 3), u32(lanes)};
}
inline ir::Inst* ValidateSaturatingMultiplyQC(ir::Inst* inst) {
    const auto input = inst->GetArg<ir::Value>(0);
    if (inst->ReturnType() != ir::ValueType::U8 || input.Type() != ir::ValueType::V128 ||
        input.Def()->GetOp() != ir::OpCode::VecSatMulHigh)
        throw std::runtime_error("VecSatMulQC requires a VecSatMulHigh V128 producer and U8 result");
    ValidateSaturatingMultiply(input.Def());
    return input.Def();
}
struct SaturatingMultiplyResult { std::array<u64, 2> value{}; u8 saturated{}; };

inline SaturatingMultiplyResult EvaluateSaturatingMultiply(const std::array<u64, 2>& lhs,
    const std::array<u64, 2>& rhs, const std::array<u64, 2>& accumulator, SaturatingMultiplyShape shape) {
    SaturatingMultiplyResult result;
    const s64 sign = s64{1} << (shape.bits - 1), lower = -sign, upper = sign - 1;
    const u64 mask = (u64{1} << shape.bits) - 1;
    const auto lane = [&](const std::array<u64, 2>& value, u32 index) {
        const u64 raw = (value[index * shape.bits / 64] >> (index * shape.bits % 64)) & mask;
        return raw & u64(sign) ? s64(raw) - (s64{1} << shape.bits) : s64(raw);
    };
    for (u32 index = 0; index < shape.lanes; ++index) {
        // 32x32 signed products, their negation and the rounding constant all
        // fit s64. Never form an overflowing doubled product or shifted acc.
        s64 product = lane(lhs, index) * lane(rhs, index);
        if (shape.mode == 3) product = -product;
        if (shape.mode != 0) product += s64{1} << (shape.bits - 2);
        const s64 divisor = s64{1} << (shape.bits - 1);
        s64 exact = product / divisor;
        if (product < 0 && product % divisor != 0) --exact;
        if (shape.mode >= 2) exact += lane(accumulator, index);
        const s64 value = exact < lower ? lower : exact > upper ? upper : exact;
        result.saturated |= u8(value != exact);
        result.value[index * shape.bits / 64] |= (u64(value) & mask) << (index * shape.bits % 64);
    }
    return result;
}
}  // namespace swift::runtime
