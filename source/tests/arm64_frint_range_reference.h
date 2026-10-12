#pragma once
#include <boost/multiprecision/cpp_int.hpp>
#include "base/types.h"

// Exact FPRoundIntN integer/rational oracle, independent of runtime FP helpers.
namespace frint_range_reference {
using namespace swift;
using Big = boost::multiprecision::cpp_int;
struct Result { u64 value; u32 exceptions; };
inline Result Evaluate(u64 value,u32 bits,u32 operation,u32 fpcr) {
    const u32 fraction_bits = bits == 32 ? 23 : 52,bias = bits == 32 ? 127 : 1023;
    const u64 sign = u64{1}<<(bits-1),fraction_mask = (u64{1}<<fraction_bits)-1;
    const bool negative = value & sign;
    const u32 exponent = u32((value>>fraction_bits)&(2*bias+1)),integer_bits = operation < 2 ? 32 : 64;
    const u64 indefinite = sign | (u64(bias+integer_bits-1)<<fraction_bits);
    if (exponent == 2*bias+1) return {indefinite,1};
    u64 fraction = value & fraction_mask;u32 flags = 0;
    if (!exponent && fraction && (fpcr & (1u<<24))) { fraction = 0;flags = 128; }
    Big numerator = fraction | (exponent ? u64{1}<<fraction_bits : 0),denominator = 1;
    const s32 shift = s32(exponent ? exponent : 1)-s32(bias)-s32(fraction_bits);
    if (shift >= 0) numerator <<= shift;else denominator <<= -shift;
    Big quotient = numerator/denominator;const Big remainder = numerator%denominator;
    const u32 mode = operation & 1 ? (fpcr>>22)&3 : 3;
    bool increment = remainder && ((mode == 1 && !negative) || (mode == 2 && negative));
    if (mode == 0) increment = 2*remainder > denominator || (2*remainder == denominator && bool(quotient&1));
    quotient += u32(increment);const Big integer = negative ? Big(-quotient) : quotient;
    if (integer < -(Big{1}<<(integer_bits-1)) || integer > (Big{1}<<(integer_bits-1))-1)
        return {indefinite,flags|1};
    if (remainder) flags |= 16;
    if (!quotient) return {negative ? sign : 0,flags};
    const u32 top = boost::multiprecision::msb(quotient);
    const Big significand = (quotient<<fraction_bits)>>top;
    return {(negative ? sign : 0) | (u64(top+bias)<<fraction_bits) | (significand.convert_to<u64>()&fraction_mask),flags};
}
}  // namespace frint_range_reference
