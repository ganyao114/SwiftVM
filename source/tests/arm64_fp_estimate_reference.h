#pragma once
#include <boost/multiprecision/cpp_int.hpp>
#include "base/types.h"

namespace fp_estimate_reference {
using namespace swift;
using Big = boost::multiprecision::cpp_int;
struct Result { u64 value; u32 exceptions; };
// Quantize the normalized rational argument, then round its reciprocal or
// reciprocal square root. The root oracle uses multiprecision integer sqrt,
// independently of the runtime's bounded binary search.
inline Result Evaluate(u64 value,u32 bits,u32 operation,u32 fpcr) {
    const u32 fraction_bits = bits == 16 ? 10 : bits == 32 ? 23 : 52;
    const s32 bias = bits == 16 ? 15 : bits == 32 ? 127 : 1023;
    const u64 sign = value & (u64{1}<<(bits-1)), fraction_mask = (u64{1}<<fraction_bits)-1;
    const u64 infinity = u64(2*bias+1)<<fraction_bits, quiet = u64{1}<<(fraction_bits-1);
    s32 exponent = s32((value>>fraction_bits)&(2*bias+1));
    u64 fraction = value&fraction_mask;
    u32 flags = 0;
    if (exponent == 2*bias+1 && fraction)
        return {fpcr&(1u<<25) ? infinity|quiet : value|quiet, fraction&quiet ? 0u : 1u};
    if (!exponent && fraction && (fpcr&(1u<<(bits == 16 ? 19 : 24)))) { fraction = 0; if (bits != 16) flags |= 128; }
    if (operation == 2) return {sign|(u64(exponent ? 2*bias+1-exponent : 2*bias)<<fraction_bits),flags};
    if (!exponent && !fraction) return {sign|infinity,flags|2};
    if (operation == 1 && sign) return {infinity|quiet,flags|1};
    if (exponent == 2*bias+1) return {sign,flags};
    Big significand = fraction | (exponent ? u64{1}<<fraction_bits : 0);
    if (!exponent) {
        const u32 top = boost::multiprecision::msb(significand);
        exponent = 1-s32(fraction_bits-top);
        significand <<= fraction_bits-top;
    }
    const Big unit = Big{1}<<fraction_bits;
    if (operation == 0 && exponent < -1) {
        const u32 mode = (fpcr>>22)&3;
        const bool inf = mode == 0 || (mode == 1 && !sign) || (mode == 2 && sign);
        return {sign|(inf ? infinity : infinity-1),flags|20};
    }
    const u32 scale = operation == 0 ? 256 : 128;
    const Big interval = (significand*scale)/unit;
    // The midpoint argument is (interval+1/2)/(2*scale), additionally
    // halved for the odd-exponent root interval. Output scale is 256.
    const Big denominator = 2*interval+1;
    const Big numerator = operation == 0 ? Big(Big{256}*4*scale) :
                          Big(Big{256}*256*4*scale*((exponent&1) ? 2 : 1));
    Big estimate;
    if (operation == 0) estimate = (2*numerator+denominator)/(2*denominator);
    else {
        const Big quotient = numerator/denominator;
        estimate = sqrt(quotient);
        if ((2*estimate+1)*(2*estimate+1)*denominator < 4*numerator) ++estimate;
    }
    if (operation == 0) {
        const s32 output_exponent = 2*bias-1-exponent;
        if (output_exponent <= 0) {
            if (fpcr&(1u<<(bits == 16 ? 19 : 24))) return {sign,flags|8};
            const Big output = (estimate<<(fraction_bits-8))>>(1-output_exponent);
            return {sign|output.convert_to<u64>(),flags};
        }
        const Big output = (Big{output_exponent}<<fraction_bits)|((estimate-256)<<(fraction_bits-8));
        return {sign|output.convert_to<u64>(),flags};
    }
    const Big output = (Big{(3*bias-1-exponent)/2}<<fraction_bits)|((estimate-256)<<(fraction_bits-8));
    return {output.convert_to<u64>(),flags};
}
}  // namespace fp_estimate_reference
