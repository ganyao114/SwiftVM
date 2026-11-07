#pragma once
#include <array>
#include <cstring>
#include "estimate_tables.h"
#include "numeric.h"

namespace swift::runtime::arm64 {

struct SaturatingMultiplyShape {
    u32 bits, mode, lanes;
};

struct SaturatingMultiplyResult {
    std::array<u64, 2> value{};
    u8 saturated{};
};

inline SaturatingMultiplyResult EvaluateSaturatingMultiply(const std::array<u64, 2>& lhs,
                                                           const std::array<u64, 2>& rhs,
                                                           const std::array<u64, 2>& accumulator,
                                                           SaturatingMultiplyShape shape) {
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
}  // namespace swift::runtime::arm64

namespace swift::runtime::arm64 {

enum class SaturatingShiftMode : u32 {
    SQSHL,
    UQSHL,
    SQSHLU,
    SQSHRN,
    UQSHRN,
    SQSHRUN,
    SQRSHRN,
    UQRSHRN,
    SQRSHRUN
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

struct SaturatingShiftResult {
    std::array<u64, 2> value{};
    u8 saturated{};
};

inline SaturatingShiftResult EvaluateSaturatingShift(const std::array<u64, 2>& source,
                                                     SaturatingShiftShape shape) {
    const auto mask = [](u32 bits) { return bits == 64 ? ~u64{0} : (u64{1} << bits) - 1; };
    const auto extend = [](u64 value, u32 bits) {
        const u64 sign = u64{1} << (bits - 1);
        return (value ^ sign) - sign;
    };
    const auto arithmetic_right = [](u64 value, u32 shift) {
        if (!shift) return value;
        return (value >> shift) | (value >> 63 ? (~u64{0} << (64 - shift)) : 0);
    };
    SaturatingShiftResult output;
    const u32 source_bits = shape.SourceBits();
    for (u32 lane = 0; lane < shape.lanes; ++lane) {
        const u64 input =
                (source[lane * source_bits / 64] >> (lane * source_bits % 64)) & mask(source_bits);
        u64 result;
        bool overflow, negative;
        if (shape.Left()) {
            result = (input << shape.shift) & mask(shape.bits);
            const u64 original = shape.SignedResult() ? extend(input, shape.bits) : input;
            const u64 recovered =
                    shape.SignedResult() ? arithmetic_right(extend(result, shape.bits), shape.shift)
                                         : result >> shape.shift;
            negative = (input >> (shape.bits - 1)) != 0;
            overflow = original != recovered ||
                       (shape.mode == SaturatingShiftMode::SQSHLU && negative);
        } else {
            result = shape.SignedSource()
                             ? arithmetic_right(extend(input, source_bits), shape.shift)
                             : input >> shape.shift;
            if (shape.Rounded()) result += (input >> (shape.shift - 1)) & 1;
            const u64 trimmed = shape.SignedResult() ? extend(result & mask(shape.bits), shape.bits)
                                                     : result & mask(shape.bits);
            overflow = result != trimmed;
            negative = (result >> 63) != 0;
        }
        if (overflow)
            result = shape.SignedSource() && negative
                             ? (shape.SignedResult() ? u64{1} << (shape.bits - 1) : 0)
                     : shape.SignedResult() ? (u64{1} << (shape.bits - 1)) - 1
                                            : mask(shape.bits);
        output.saturated |= u8(overflow);
        output.value[lane * shape.bits / 64] |= (result & mask(shape.bits))
                                                << (lane * shape.bits % 64);
    }
    return output;
}
}  // namespace swift::runtime::arm64

namespace swift::runtime::arm64 {

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

}  // namespace swift::runtime::arm64