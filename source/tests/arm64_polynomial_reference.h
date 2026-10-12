#pragma once
#include <array>
#include "base/types.h"

namespace polynomial_reference {
using namespace swift;
using Vector = std::array<u64,2>;
inline Vector Multiply(u64 left,u64 right,u32 bits) {
    Vector result{};
    for (u32 coefficient=0;coefficient<bits;++coefficient) if ((right>>coefficient)&1) {
        result[0] ^= left<<coefficient;
        if (coefficient) result[1] ^= left>>(64-coefficient);
    }
    return result;
}
inline Vector Evaluate(Vector left,Vector right,bool widening,u32 bits,bool q) {
    if (bits==64) return Multiply(left[q],right[q],64);
    Vector result{};
    const u32 first = widening && q ? 8 : 0,lanes = widening ? 8 : q ? 16 : 8;
    for (u32 lane=0;lane<lanes;++lane) {
        const u32 input = first+lane,output_bits = widening ? 16 : 8;
        const u64 a = (left[input/8]>>(input%8*8))&255,b = (right[input/8]>>(input%8*8))&255;
        const u64 value = Multiply(a,b,8)[0] & (widening ? 65535 : 255);
        result[lane*output_bits/64] |= value<<(lane*output_bits%64);
    }
    return result;
}
}  // namespace polynomial_reference
