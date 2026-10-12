#pragma once

#include <array>
#include <stdexcept>
#include "runtime/ir/instr.h"

namespace swift::runtime {

enum class SaturatingShiftMode : u32 {
    SQSHL, UQSHL, SQSHLU, SQSHRN, UQSHRN, SQSHRUN, SQRSHRN, UQRSHRN, SQRSHRUN
};
constexpr u64 SaturatingShiftControl(SaturatingShiftMode mode, u32 lanes) {
    return (u64{lanes} << 4) | u32(mode);
}
struct SaturatingShiftShape {
    u32 bits, shift, lanes;
    SaturatingShiftMode mode;
    bool Left() const { return u32(mode) <= u32(SaturatingShiftMode::SQSHLU); }
    bool SignedSource() const {
        return mode != SaturatingShiftMode::UQSHL && mode != SaturatingShiftMode::UQSHRN &&
               mode != SaturatingShiftMode::UQRSHRN;
    }
    bool SignedResult() const {
        return mode == SaturatingShiftMode::SQSHL || mode == SaturatingShiftMode::SQSHRN ||
               mode == SaturatingShiftMode::SQRSHRN;
    }
    bool Rounded() const { return u32(mode) >= u32(SaturatingShiftMode::SQRSHRN); }
    u32 SourceBits() const { return Left() ? bits : bits * 2; }
};
inline SaturatingShiftShape ValidateSaturatingShift(ir::Inst* inst) {
    for (u32 index = 1; index < 4; ++index)
        if (!inst->ArgAt(index).IsImm()) throw std::runtime_error("VecSatShiftImm requires immediate shape arguments");
    const u64 bits = inst->GetArg<ir::Imm>(1).Get(), shift = inst->GetArg<ir::Imm>(2).Get();
    const u64 control = inst->GetArg<ir::Imm>(3).Get(), mode = control & 15, lanes = control >> 4;
    if (mode > u32(SaturatingShiftMode::SQRSHRUN) || (bits != 8 && bits != 16 && bits != 32 && bits != 64))
        throw std::runtime_error("VecSatShiftImm requires a legal full-width mode and lane width");
    const bool left = mode <= u32(SaturatingShiftMode::SQSHLU);
    if ((left && (shift >= bits || !(lanes == 128 / bits || (bits != 64 && lanes == 64 / bits)))) ||
        (!left && (bits == 64 || shift == 0 || shift > bits || lanes != 64 / bits)))
        throw std::runtime_error("VecSatShiftImm requires a legal active shape and immediate shift");
    if (inst->ReturnType() != ir::ValueType::V128 || !inst->ArgAt(0).IsValue())
        throw std::runtime_error("VecSatShiftImm requires a V128 result and input");
    const auto input = inst->GetArg<ir::Value>(0);
    if (!input.Defined() || input.Type() != ir::ValueType::V128 || input.Def()->ReturnType() != ir::ValueType::V128)
        throw std::runtime_error("VecSatShiftImm requires a canonical V128 input view and producer");
    return {u32(bits), u32(shift), u32(lanes), SaturatingShiftMode(mode)};
}
inline ir::Inst* ValidateSaturatingShiftQC(ir::Inst* inst) {
    if (inst->ReturnType() != ir::ValueType::U8 || !inst->ArgAt(0).IsValue())
        throw std::runtime_error("VecSatShiftQC requires a U8 result and VecSatShiftImm producer");
    const auto input = inst->GetArg<ir::Value>(0);
    if (!input.Defined() || input.Type() != ir::ValueType::V128 || input.Def()->GetOp() != ir::OpCode::VecSatShiftImm)
        throw std::runtime_error("VecSatShiftQC requires a canonical VecSatShiftImm input");
    ValidateSaturatingShift(input.Def());
    return input.Def();
}
struct SaturatingShiftResult { std::array<u64, 2> value{}; u8 saturated{}; };
inline SaturatingShiftResult EvaluateSaturatingShift(const std::array<u64, 2>& source, SaturatingShiftShape shape) {
    const auto mask = [](u32 bits) { return bits == 64 ? ~u64{0} : (u64{1} << bits) - 1; };
    const auto extend = [](u64 value, u32 bits) { const u64 sign = u64{1} << (bits - 1); return (value ^ sign) - sign; };
    const auto arithmetic_right = [](u64 value, u32 shift) {
        if (!shift) return value;
        return (value >> shift) | (value >> 63 ? (~u64{0} << (64 - shift)) : 0);
    };
    SaturatingShiftResult output;
    const u32 source_bits = shape.SourceBits();
    for (u32 lane = 0; lane < shape.lanes; ++lane) {
        const u64 input = (source[lane * source_bits / 64] >> (lane * source_bits % 64)) & mask(source_bits);
        u64 result; bool overflow, negative;
        if (shape.Left()) {
            result = (input << shape.shift) & mask(shape.bits);
            const u64 original = shape.SignedResult() ? extend(input, shape.bits) : input;
            const u64 recovered = shape.SignedResult() ? arithmetic_right(extend(result, shape.bits), shape.shift) : result >> shape.shift;
            negative = (input >> (shape.bits - 1)) != 0;
            overflow = original != recovered || (shape.mode == SaturatingShiftMode::SQSHLU && negative);
        } else {
            result = shape.SignedSource() ? arithmetic_right(extend(input, source_bits), shape.shift) : input >> shape.shift;
            if (shape.Rounded()) result += (input >> (shape.shift - 1)) & 1;
            const u64 trimmed = shape.SignedResult() ? extend(result & mask(shape.bits), shape.bits) : result & mask(shape.bits);
            overflow = result != trimmed; negative = (result >> 63) != 0;
        }
        if (overflow) result = shape.SignedSource() && negative ? (shape.SignedResult() ? u64{1} << (shape.bits - 1) : 0) :
                                  shape.SignedResult() ? (u64{1} << (shape.bits - 1)) - 1 : mask(shape.bits);
        output.saturated |= u8(overflow);
        output.value[lane * shape.bits / 64] |= (result & mask(shape.bits)) << (lane * shape.bits % 64);
    }
    return output;
}
}  // namespace swift::runtime
