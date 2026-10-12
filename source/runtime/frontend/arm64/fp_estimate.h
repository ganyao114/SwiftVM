#pragma once

#include "runtime/common/estimate_tables.h"
#include "base/types.h"

namespace swift::arm64 {

enum class FPEstimateOperation { Reciprocal, ReciprocalSquareRoot, ReciprocalExponent };
struct FPEstimateResult { u64 value; u32 exceptions; };

using runtime::ReciprocalMidpoint;
using runtime::ReciprocalRootMidpoint;
using runtime::kReciprocalMidpoints;
using runtime::kReciprocalRootMidpoints;

inline FPEstimateResult EvaluateEstimateFP(u64 value, u32 bits,
                                          FPEstimateOperation operation, u32 fpcr) {
    const u32 fraction_bits = bits == 16 ? 10 : bits == 32 ? 23 : 52;
    const s32 bias = bits == 16 ? 15 : bits == 32 ? 127 : 1023;
    const u64 sign = value & (u64{1} << (bits - 1));
    const u64 fraction_mask = (u64{1} << fraction_bits) - 1;
    const u64 exponent_mask = u64(2 * bias + 1) << fraction_bits;
    const u64 quiet = u64{1} << (fraction_bits - 1);
    s32 exponent = s32((value & exponent_mask) >> fraction_bits);
    u64 fraction = value & fraction_mask;
    u32 exceptions = 0;
    if (exponent == 2 * bias + 1 && fraction) {
        if (!(fraction & quiet)) exceptions |= 1;
        return {fpcr & (1u << 25) ? exponent_mask | quiet : sign | exponent_mask | fraction | quiet, exceptions};
    }
    if (!exponent && fraction && (fpcr & (1u << (bits == 16 ? 19 : 24)))) {
        fraction = 0;
        if (bits != 16) exceptions |= 128;
    }
    if (operation == FPEstimateOperation::ReciprocalExponent)
        return {sign | (u64(exponent ? 2 * bias + 1 - exponent : 2 * bias) << fraction_bits), exceptions};
    if (!exponent && !fraction) return {sign | exponent_mask, exceptions | 2};
    if (operation == FPEstimateOperation::ReciprocalSquareRoot && sign)
        return {exponent_mask | quiet, exceptions | 1};
    if (exponent == 2 * bias + 1) return {sign, exceptions};
    if (!exponent) {
        exponent = 1;
        while (!(fraction & (u64{1} << fraction_bits))) { fraction <<= 1; --exponent; }
        fraction &= fraction_mask;
    }
    if (operation == FPEstimateOperation::Reciprocal) {
        if (exponent < -1) {
            const u32 mode = (fpcr >> 22) & 3;
            const bool infinity = mode == 0 || (mode == 1 && !sign) || (mode == 2 && sign);
            return {sign | (infinity ? exponent_mask : exponent_mask - 1), exceptions | 20};
        }
        const u32 estimate = kReciprocalMidpoints[u32(fraction >> (fraction_bits - 8))];
        const s32 result_exponent = 2 * bias - 1 - exponent;
        if (result_exponent <= 0) {
            if (fpcr & (1u << (bits == 16 ? 19 : 24))) return {sign, exceptions | 8};
            return {sign | (u64(estimate) << (fraction_bits - 8)) >> (1 - result_exponent), exceptions};
        }
        return {sign | (u64(result_exponent) << fraction_bits) |
                (u64(estimate - 256) << (fraction_bits - 8)), exceptions};
    }
    const bool odd = (exponent & 1) != 0;
    const u32 estimate = kReciprocalRootMidpoints[u32(odd)][u32(fraction >> (fraction_bits - 7))];
    return {(u64((3 * bias - 1 - exponent) / 2) << fraction_bits) |
            (u64(estimate - 256) << (fraction_bits - 8)), exceptions};
}
}  // namespace swift::arm64
