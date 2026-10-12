#pragma once
#include "base/types.h"

// Mathematical classification and exact dyadic significand/exponent ordering.
// This oracle never calls the runtime FP helpers or SoftFloat.
namespace fp_compare_reference {
struct Result { swift::u64 value; swift::u32 exceptions; };
inline Result Evaluate(swift::u64 a, swift::u64 b, swift::u32 bits, swift::u32 operation, swift::u32 fpcr) {
    using namespace swift;
    const u32 fraction_bits = bits == 16 ? 10 : bits == 32 ? 23 : 52;
    const u64 sign = u64{1} << (bits - 1), fraction_mask = (u64{1} << fraction_bits) - 1;
    const u64 exponent_mask = (sign - 1) ^ fraction_mask, quiet = u64{1} << (fraction_bits - 1);
    u32 flags = 0;
    auto flush = [&](u64 x) {
        if (!(x & exponent_mask) && (x & fraction_mask) && (fpcr & (1u << (bits == 16 ? 19 : 24)))) {
            if (bits != 16) flags |= 128;
            return x & sign;
        }
        return x;
    };
    a = flush(a); b = flush(b);
    auto nan = [&](u64 x) { return (x & exponent_mask) == exponent_mask && (x & fraction_mask); };
    if (nan(a) || nan(b)) {
        if (operation != 0 || (nan(a) && !(a & quiet)) || (nan(b) && !(b & quiet))) flags |= 1;
        return {0, flags};
    }
    const bool absolute = operation >= 5;
    const bool negative_a = !absolute && (a & sign), negative_b = !absolute && (b & sign);
    auto magnitude = [&](u64 x, u64 y) {
        const u32 ex = u32((x & exponent_mask) >> fraction_bits), ey = u32((y & exponent_mask) >> fraction_bits);
        const u64 sx = (x & fraction_mask) | (ex ? u64{1} << fraction_bits : 0);
        const u64 sy = (y & fraction_mask) | (ey ? u64{1} << fraction_bits : 0);
        if (!sx || !sy) return sx == sy ? 0 : sx ? 1 : -1;
        // Infinity is the only nonfinite value surviving classification.
        const u32 maximum = u32(exponent_mask >> fraction_bits);
        if (ex == maximum || ey == maximum) return ex == ey ? 0 : ex == maximum ? 1 : -1;
        if (ex == ey || (ex <= 1 && ey <= 1)) return sx == sy ? 0 : sx > sy ? 1 : -1;
        // Each normal significand is in [1,2), so disjoint normal exponent
        // intervals order directly. Exponent 0 denotes the subnormal interval.
        return ex > ey ? 1 : -1;
    };
    int relation;
    if (!(a & ~sign) && !(b & ~sign)) relation = 0;
    else if (negative_a != negative_b) relation = negative_a ? -1 : 1;
    else { relation = magnitude(a,b); if (negative_a) relation = -relation; }
    const bool match = operation == 0 ? relation == 0 : operation == 1 || operation == 5 ? relation >= 0 :
                       operation == 2 || operation == 6 ? relation > 0 : operation == 3 ? relation <= 0 : relation < 0;
    return {match ? (bits == 64 ? ~u64{0} : (u64{1} << bits) - 1) : 0, flags};
}
}  // namespace fp_compare_reference
