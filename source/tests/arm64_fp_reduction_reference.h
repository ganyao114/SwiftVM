#pragma once
#include <boost/multiprecision/cpp_int.hpp>
#include "base/types.h"
namespace fp_reduction_reference {
using namespace swift;
using Big = boost::multiprecision::cpp_int;
struct FloatFormat {
    u32 bits, fraction, bias, max_exponent;
    u64 sign, fraction_mask, exponent_mask, quiet;
    explicit FloatFormat(u32 width) : bits(width), fraction(width == 16 ? 10 : width == 32 ? 23 : 52),
        bias(width == 16 ? 15 : width == 32 ? 127 : 1023), max_exponent(2 * bias + 1),
        sign(u64{1} << (width - 1)), fraction_mask((u64{1} << fraction) - 1),
        exponent_mask(u64(max_exponent) << fraction), quiet(u64{1} << (fraction - 1)) {}
    bool NaN(u64 value) const { return (value & exponent_mask) == exponent_mask && (value & fraction_mask); }
    bool Signaling(u64 value) const { return NaN(value) && !(value & quiet); }
    bool Subnormal(u64 value) const { return !(value & exponent_mask) && (value & fraction_mask); }
    bool Flush(u32 fpcr) const { return fpcr & (1u << (bits == 16 ? 19 : 24)); }
    Big ExactMagnitude(u64 value) const {
        const u32 exponent = (value & exponent_mask) >> fraction;
        Big magnitude = value & fraction_mask;
        if (exponent) magnitude = (magnitude + (Big{1} << fraction)) << (exponent - 1);
        return magnitude;
    }
};
struct Ref { u64 value; u32 flags; };
// Every finite float is an integer multiple of the minimum subnormal. This
// reference adds those unbounded integers and rounds exactly once to H/S/D.
inline Ref Reference(u64 a, u64 b, u32 bits, u32 operation, u32 fpcr) {
    const FloatFormat f(bits);
    u32 flags = 0;
    for (u64* value : {&a, &b}) if (f.Flush(fpcr) && f.Subnormal(*value)) {
        *value &= f.sign;
        if (bits != 16) flags |= 128;
    }
    const bool numeric = operation >= 6, minimum = operation & 1;
    const bool signaling = f.Signaling(a) || f.Signaling(b);
    if (signaling || ((f.NaN(a) || f.NaN(b)) &&
                     (operation == 2 || !numeric || (f.NaN(a) && f.NaN(b))))) {
        if (signaling) flags |= 1;
        u64 selected = f.Signaling(a) ? a : f.Signaling(b) ? b : f.NaN(a) ? a : b;
        return {fpcr & (1u << 25) ? f.exponent_mask | f.quiet : selected | f.quiet, flags};
    }
    if (f.NaN(a)) return {b, flags};
    if (f.NaN(b)) return {a, flags};
    if (operation != 2) {
        if (!(a & ~f.sign) && !(b & ~f.sign)) return {minimum ? a | b : a & b, flags};
        const bool less = bool(a & f.sign) != bool(b & f.sign) ? bool(a & f.sign)
                         : (a & f.sign) ? a > b : a < b;
        return {minimum == less ? a : b, flags};
    }
    const bool ainf = (a & ~f.sign) == f.exponent_mask, binf = (b & ~f.sign) == f.exponent_mask;
    if (ainf || binf) {
        if (ainf && binf && bool(a & f.sign) != bool(b & f.sign))
            return {f.exponent_mask | f.quiet, flags | 1};
        return {ainf ? a : b, flags};
    }
    Big sum = (a & f.sign ? -f.ExactMagnitude(a) : f.ExactMagnitude(a)) +
              (b & f.sign ? -f.ExactMagnitude(b) : f.ExactMagnitude(b));
    const u32 rounding = (fpcr >> 22) & 3;
    if (sum == 0) {
        const u64 sign = bool(a & f.sign) == bool(b & f.sign) ? a & f.sign
                       : rounding == 2 ? f.sign : 0;
        return {sign, flags};
    }
    const bool negative = sum < 0;
    if (negative) sum = -sum;
    const u64 sign = negative ? f.sign : 0;
    if (f.Flush(fpcr) && sum < (Big{1} << f.fraction)) return {sign, flags | 8};
    const u32 top = boost::multiprecision::msb(sum);
    u32 shift = top > f.fraction ? top - f.fraction : 0;
    Big rounded = sum >> shift, remainder = sum - (rounded << shift);
    if (remainder != 0) {
        flags |= 16;
        bool increment = rounding == 1 ? !negative : rounding == 2 ? negative : false;
        if (rounding == 0) {
            const Big half = Big{1} << (shift - 1);
            increment = remainder > half || (remainder == half && bool(rounded & 1));
        }
        if (increment) ++rounded;
    }
    if (rounded >= (Big{1} << (f.fraction + 1))) { rounded >>= 1; ++shift; }
    const u32 exponent = rounded < (Big{1} << f.fraction) ? 0 : shift + 1;
    if (exponent >= f.max_exponent) {
        const bool infinity = rounding == 0 || (rounding == 1 && !negative) || (rounding == 2 && negative);
        return {sign | (infinity ? f.exponent_mask : f.exponent_mask - 1), flags | 20};
    }
    return {sign | (u64(exponent) << f.fraction) | (rounded.convert_to<u64>() & f.fraction_mask), flags};
}
}