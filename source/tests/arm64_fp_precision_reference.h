#pragma once
#include <algorithm>
#include <boost/multiprecision/cpp_int.hpp>
#include "base/types.h"

namespace fp_precision_reference {
using namespace swift;
using Big = boost::multiprecision::cpp_int;
struct Result { u64 value; u32 exceptions; };
inline u32 Fraction(u32 bits) { return bits == 16 ? 10 : bits == 32 ? 23 : 52; }
inline u32 Bias(u32 bits) { return bits == 16 ? 15 : bits == 32 ? 127 : 1023; }
// Decode an exact dyadic number and round directly to the target quantum.
// No runtime FP helpers, native floating-point values, or SoftFloat.
inline Result Evaluate(u64 value, u32 source, u32 destination, bool odd, u32 fpcr) {
    const u32 sf = Fraction(source), sbias = Bias(source), df = Fraction(destination), dbias = Bias(destination);
    const bool negative = value & (u64{1} << (source-1));
    const u64 sign = negative ? u64{1} << (destination-1) : 0;
    const u32 exponent = u32((value >> sf) & (2*sbias+1));
    u64 fraction = value & ((u64{1} << sf)-1);
    const u64 infinity = u64(2*dbias+1) << df;
    u32 flags = 0;
    if (exponent == 2*sbias+1) {
        if (!fraction) return {sign|infinity,0};
        if (!(fraction & (u64{1} << (sf-1)))) flags |= 1;
        if (fpcr & (1u<<25)) return {infinity | (u64{1} << (df-1)),flags};
        const u64 payload = df >= sf ? fraction << (df-sf) : fraction >> (sf-df);
        return {sign|infinity|payload|(u64{1} << (df-1)),flags};
    }
    if (!exponent && fraction && source != 16 && (fpcr & (1u<<24))) { fraction = 0; flags |= 128; }
    if (!exponent && !fraction) return {sign,flags};
    const Big significand = fraction | (exponent ? u64{1} << sf : 0);
    const s32 power = s32(exponent ? exponent : 1)-s32(sbias)-s32(sf);
    const s32 top = s32(boost::multiprecision::msb(significand))+power;
    if (destination != 16 && (fpcr & (1u<<24)) && top < 1-s32(dbias)) return {sign,flags|8};
    const s32 shift = std::max(top,1-s32(dbias))-s32(df)-power;
    const Big denominator = Big{1} << std::max(shift,0);
    Big result = shift > 0 ? Big(significand / denominator) : Big(significand << -shift);
    const Big remainder = shift > 0 ? Big(significand % denominator) : Big{0};
    const u32 mode = (fpcr >> 22)&3;
    if (remainder) {
        flags |= 16;
        if (top < 1-s32(dbias)) flags |= 8;
        if (odd) result |= 1;
        else if (mode == 0) result += u32(2*remainder > denominator || (2*remainder == denominator && bool(result & 1)));
        else if ((mode == 1 && !negative) || (mode == 2 && negative)) ++result;
    }
    s32 rounded_top = std::max(top,1-s32(dbias));
    if (result >= Big{1} << (df+1)) { result >>= 1; ++rounded_top; }
    if (rounded_top > s32(dbias)) {
        const bool inf = !odd && (mode == 0 || (mode == 1 && !negative) || (mode == 2 && negative));
        return {sign|(inf ? infinity : infinity-1),flags|20};
    }
    const u32 encoded_exponent = result >= Big{1} << df ? rounded_top+dbias : 0;
    return {sign|(u64(encoded_exponent) << df)|(result.convert_to<u64>() & ((u64{1} << df)-1)),flags};
}
}  // namespace fp_precision_reference
