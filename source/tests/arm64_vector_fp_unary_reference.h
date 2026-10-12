#pragma once
#include <boost/multiprecision/cpp_int.hpp>
#include "base/types.h"

namespace vector_fp_unary_reference {
using namespace swift;
using Big = boost::multiprecision::cpp_int;
struct Result { u64 value; u32 exceptions; };
inline Big IntegerRoot(const Big& value) {
    Big root = Big{1} << ((boost::multiprecision::msb(value) + 2) / 2);
    for (;;) {
        Big next = (root + value/root)/2;
        if (next >= root) return root;
        root = next;
    }
}
// Operations: sqrt, nearest, minus infinity, plus infinity, zero, ties away,
// exact FPCR rounding (X), and quiet FPCR rounding (I). Integer-only oracle.
inline Result Evaluate(u64 value, u32 bits, u32 operation, u32 fpcr) {
    const u32 fraction_bits = bits == 32 ? 23 : 52, bias = bits == 32 ? 127 : 1023;
    const u32 maximum_exponent = 2*bias+1, scale = fraction_bits+bias-1;
    const u64 sign = value & (u64{1}<<(bits-1)), exponent = (value>>fraction_bits)&maximum_exponent;
    const u64 fraction_mask = (u64{1}<<fraction_bits)-1, quiet = u64{1}<<(fraction_bits-1);
    const u64 exponent_mask = u64(maximum_exponent)<<fraction_bits;
    u64 fraction = value & fraction_mask;
    u32 flags = 0;
    if (exponent == maximum_exponent && fraction)
        return {fpcr & (1u<<25) ? exponent_mask|quiet : value|quiet, fraction & quiet ? 0u : 1u};
    if (!exponent && fraction && (fpcr & (1u<<24))) { value = sign; fraction = 0; flags |= 128; }
    if (!(value & ~(u64{1}<<(bits-1)))) return {value,flags};
    if (exponent == maximum_exponent)
        return sign && operation == 0 ? Result{exponent_mask|quiet,flags|1} : Result{value,flags};
    const Big magnitude = exponent ? Big{fraction|(u64{1}<<fraction_bits)} << (exponent-1) : Big{fraction};
    u32 mode = (fpcr>>22)&3;
    if (operation == 0) {
        if (sign) return {exponent_mask|quiet,flags|1};
        const Big target = magnitude << scale, root = IntegerRoot(target);
        const u32 top = boost::multiprecision::msb(root);
        u32 shift = top > fraction_bits ? top-fraction_bits : 0;
        Big rounded = root >> shift;
        const Big encoded = rounded << shift;
        const bool exact = encoded*encoded == target;
        bool increment = mode == 1 && !exact;
        if (mode == 0) {
            const Big midpoint = (2*rounded+1) << shift;
            increment = 4*target > midpoint*midpoint || (4*target == midpoint*midpoint && bool(rounded&1));
        }
        if (increment) ++rounded;
        if (rounded >= (Big{1}<<(fraction_bits+1))) { rounded >>= 1; ++shift; }
        return {(u64(shift+1)<<fraction_bits)|(rounded.convert_to<u64>()&fraction_mask),flags|(exact ? 0 : 16)};
    }
    switch (operation) {
        case 1: mode = 0; break;
        case 2: mode = 2; break;
        case 3: mode = 1; break;
        case 4: mode = 3; break;
        case 5: mode = 4; break;
        default: break;
    }
    Big rounded = magnitude >> scale, remainder = magnitude-(rounded<<scale);
    if (remainder != 0) {
        bool increment = (mode == 1 && !sign)||(mode == 2 && sign);
        if (mode == 0 || mode == 4) increment = remainder > (Big{1}<<(scale-1)) ||
            (remainder == (Big{1}<<(scale-1)) && (mode == 4 || bool(rounded&1)));
        if (increment) ++rounded;
    }
    if (remainder != 0 && operation == 6) flags |= 16;
    if (rounded == 0) return {sign,flags};
    const u32 top = boost::multiprecision::msb(rounded);
    const u64 significand = ((rounded<<fraction_bits)>>top).convert_to<u64>();
    return {sign|(u64(top+bias)<<fraction_bits)|(significand&fraction_mask),flags};
}
}  // namespace vector_fp_unary_reference
