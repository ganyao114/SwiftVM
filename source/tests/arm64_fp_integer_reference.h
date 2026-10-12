#pragma once
#include <boost/multiprecision/cpp_int.hpp>
#include "base/types.h"

// Exact rational reference: integer rounding precedes range saturation.
// No runtime FP helpers, native floating-point arithmetic, or SoftFloat.
namespace fp_integer_reference {
using namespace swift;
using Big = boost::multiprecision::cpp_int;
struct Result { u64 value; u32 exceptions; };
inline Big Round(Big quotient, const Big& remainder, const Big& denominator, bool negative, u32 mode) {
    if (!remainder) return quotient;
    bool increment = (mode == 1 && !negative) || (mode == 2 && negative);
    if (mode == 0 || mode == 4)
        increment = 2*remainder > denominator || (2*remainder == denominator && (mode == 4 || bool(quotient & 1)));
    return quotient + u32(increment);
}
inline Result Evaluate(u64 value, u32 bits, u32 operation, u32 fpcr) {
    const u32 fraction_bits = bits == 16 ? 10 : bits == 32 ? 23 : 52, bias = bits == 16 ? 15 : bits == 32 ? 127 : 1023;
    const u64 sign = u64{1} << (bits-1), mask = bits == 64 ? ~u64{0} : (u64{1} << bits)-1;
    const u64 infinity = u64(2*bias+1) << fraction_bits, fraction_mask = (u64{1} << fraction_bits)-1;
    const bool is_unsigned = operation & 1;
    u32 flags = 0;
    if (operation < 2) {
        const bool negative = !is_unsigned && (value & sign);
        const Big integer = negative ? Big{value} - (Big{1} << bits) : Big{value};
        const Big magnitude = negative ? -integer : integer;
        if (!magnitude) return {0,0};
        u32 top = boost::multiprecision::msb(magnitude);
        const u32 shift = top > fraction_bits ? top-fraction_bits : 0;
        const Big denominator = Big{1} << shift, remainder = magnitude % denominator;
        const u32 mode = (fpcr >> 22)&3;
        Big significand = Round(magnitude / denominator,remainder,denominator,negative,mode);
        if (remainder) flags |= 16;
        if (significand >= Big{1} << (fraction_bits+1)) { significand >>= 1; ++top; }
        if (top > bias) {
            const bool inf = mode == 0 || (mode == 1 && !negative) || (mode == 2 && negative);
            return {(negative ? sign : 0) | (inf ? infinity : infinity-1),flags|20};
        }
        if (top < fraction_bits) significand <<= fraction_bits-top;
        return {(negative ? sign : 0) | (u64(top+bias)<<fraction_bits) | (significand.convert_to<u64>() & fraction_mask),flags};
    }
    const bool negative = value & sign;
    const u32 exponent = u32((value >> fraction_bits) & (2*bias+1));
    u64 fraction = value & fraction_mask;
    const Big low = is_unsigned ? Big{0} : -(Big{1} << (bits-1));
    const Big high = is_unsigned ? (Big{1} << bits)-1 : (Big{1} << (bits-1))-1;
    auto encoded = [&](const Big& integer) { return (integer & Big{mask}).convert_to<u64>(); };
    if (exponent == 2*bias+1) return {fraction ? 0 : encoded(negative ? low : high),1};
    if (!exponent && fraction && (fpcr & (1u << (bits == 16 ? 19 : 24)))) {
        fraction = 0; if (bits != 16) flags |= 128;
    }
    Big numerator = fraction | (exponent ? u64{1} << fraction_bits : 0), denominator = 1;
    const s32 shift = s32(exponent ? exponent : 1)-s32(bias)-s32(fraction_bits);
    if (shift >= 0) numerator <<= shift; else denominator <<= -shift;
    constexpr u32 modes[]{0,2,1,3,4};
    const Big remainder = numerator % denominator;
    Big integer = Round(numerator / denominator,remainder,denominator,negative,modes[(operation-2)/2]);
    if (negative) integer = -integer;
    if (integer < low || integer > high) return {encoded(integer < low ? low : high),flags|1};
    return {encoded(integer),flags | (remainder ? 16u : 0u)};
}
}  // namespace fp_integer_reference
