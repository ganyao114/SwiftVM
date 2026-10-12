#pragma once
#include <array>
#include <boost/multiprecision/cpp_int.hpp>
#include "base/types.h"

namespace reciprocal_step_reference {
using namespace swift;
using Big = boost::multiprecision::cpp_int;
struct Result { u64 value; u32 exceptions; };
struct Format {
    u32 bits, fraction, bias, max_exponent, scale;
    u64 sign, fraction_mask, quiet, exponent_mask;
    explicit Format(u32 width) : bits(width), fraction(width == 16 ? 10 : width == 32 ? 23 : 52),
        bias(width == 16 ? 15 : width == 32 ? 127 : 1023), max_exponent(2 * bias + 1), scale(fraction + bias - 1),
        sign(u64{1} << (width - 1)), fraction_mask((u64{1} << fraction) - 1), quiet(u64{1} << (fraction - 1)),
        exponent_mask(u64(max_exponent) << fraction) {}
    bool NaN(u64 value) const { return (value & exponent_mask) == exponent_mask && (value & fraction_mask); }
    bool Signaling(u64 value) const { return NaN(value) && !(value & quiet); }
    Big Magnitude(u64 value) const {
        const u32 exponent = (value & exponent_mask) >> fraction;
        Big significand = value & fraction_mask;
        if (!exponent) return significand;
        return (significand + (Big{1} << fraction)) << (exponent - 1);
    }
};

// Exact fused reciprocal-step arithmetic in minimum-subnormal units, followed by a
// single rational rounding. No production FP or host floating point is used.
inline Result Evaluate(u64 a, u64 b, u32 bits, bool rsqrt, u32 fpcr) {
    const Format f(bits);
    const bool flush = fpcr & (1u << (bits == 16 ? 19 : 24));
    u32 flags = 0;
    if (flush) for (u64* source : {&a, &b}) {
        if (!(*source & f.exponent_mask) && (*source & f.fraction_mask)) {
            *source &= f.sign;
            if (bits != 16) flags |= 128;
        }
    }
    a ^= f.sign;  // ARM negates the first operand before selecting a NaN.
    u64 selected = 0;
    for (u64 source : {a, b}) if (f.Signaling(source)) { selected = source; break; }
    if (selected) return {fpcr & (1u << 25) ? f.exponent_mask | f.quiet : selected | f.quiet, flags | 1};
    for (u64 source : {a, b}) if (f.NaN(source)) { selected = source; break; }
    if (selected) return {fpcr & (1u << 25) ? f.exponent_mask | f.quiet : selected | f.quiet, flags};
    const bool negative = bool(a & f.sign) != bool(b & f.sign);
    const u64 sign = negative ? f.sign : 0;
    const bool ainf = (a & ~f.sign) == f.exponent_mask, binf = (b & ~f.sign) == f.exponent_mask;
    if ((ainf && !(b & ~f.sign)) || (binf && !(a & ~f.sign)))
        return {rsqrt ? (u64(f.bias) << f.fraction) | (u64{1} << (f.fraction - 1)) : u64(f.bias + 1) << f.fraction, flags};
    if (ainf || binf) return {sign | f.exponent_mask, flags};
    Big product = f.Magnitude(a) * f.Magnitude(b);
    if (negative) product = -product;
    Big numerator = (Big{rsqrt ? 3 : 2} << (2 * f.scale)) + product;
    if (numerator == 0) return {(fpcr >> 22 & 3) == 2 ? f.sign : 0, flags};
    const bool result_negative = numerator < 0;
    if (result_negative) numerator = -numerator;
    const u64 result_sign = result_negative ? f.sign : 0;
    const Big denominator = Big{1} << (f.scale + u32(rsqrt));
    const bool tiny = numerator < (denominator << f.fraction);
    if (tiny && flush) return {result_sign, flags | 8};
    s32 top = s32(boost::multiprecision::msb(numerator)) - s32(boost::multiprecision::msb(denominator));
    if (top >= 0 ? numerator < (denominator << top) : (numerator << -top) < denominator) --top;
    u32 shift = top > s32(f.fraction) ? u32(top - f.fraction) : 0;
    const Big divisor = denominator << shift;
    Big rounded = numerator / divisor, remainder = numerator % divisor;
    const u32 mode = (fpcr >> 22) & 3;
    if (remainder != 0) {
        flags |= 16 | (tiny ? 8 : 0);
        bool increment = (mode == 1 && !result_negative) || (mode == 2 && result_negative);
        if (mode == 0) increment = 2 * remainder > divisor || (2 * remainder == divisor && bool(rounded & 1));
        if (increment) ++rounded;
    }
    if (rounded >= (Big{1} << (f.fraction + 1))) { rounded >>= 1; ++shift; }
    const u32 exponent = rounded < (Big{1} << f.fraction) ? 0 : shift + 1;
    if (exponent >= f.max_exponent) {
        const bool infinity = mode == 0 || (mode == 1 && !result_negative) || (mode == 2 && result_negative);
        return {result_sign | (infinity ? f.exponent_mask : f.exponent_mask - 1), flags | 20};
    }
    return {result_sign | (u64(exponent) << f.fraction) | (rounded.convert_to<u64>() & f.fraction_mask), flags};
}
}  // namespace reciprocal_step_reference

