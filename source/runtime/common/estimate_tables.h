#pragma once

#include <array>
#include "base/types.h"

namespace swift::runtime {

// ARM estimates use the midpoint of a quantized significand interval. Keep
// the complete calculation in integers; host reciprocal/sqrt instructions
// have a different precision and are not interchangeable with this contract.
constexpr u32 ReciprocalMidpoint(u32 index) {
    const u32 denominator = 2 * index + 1;
    return (262144 + denominator / 2) / denominator;
}
constexpr u32 ReciprocalRootMidpoint(u32 index, bool odd_exponent) {
    const u64 numerator = u64{65536} * (odd_exponent ? 1024 : 512);
    const u32 denominator = 2 * index + 1;
    u32 lower = 256, upper = 512;
    while (lower + 1 < upper) {
        const u32 middle = (lower + upper) / 2;
        if (u64{middle} * middle * denominator <= numerator) lower = middle;
        else upper = middle;
    }
    const u64 midpoint = 2 * lower + 1;
    return lower + (midpoint * midpoint * denominator <= 4 * numerator);
}

inline constexpr auto kReciprocalMidpoints = [] {
    std::array<u16, 256> table{};
    for (u32 index = 0; index < table.size(); ++index)
        table[index] = u16(ReciprocalMidpoint(256 + index));
    return table;
}();
inline constexpr auto kReciprocalRootMidpoints = [] {
    std::array<std::array<u16, 128>, 2> table{};
    for (u32 parity = 0; parity < table.size(); ++parity)
        for (u32 index = 0; index < table[parity].size(); ++index)
            table[parity][index] = u16(ReciprocalRootMidpoint(128 + index, parity != 0));
    return table;
}();

static_assert([] {
    for (u16 estimate : kReciprocalMidpoints)
        if (estimate < 256 || estimate > 511) return false;
    for (const auto& parity : kReciprocalRootMidpoints)
        for (u16 estimate : parity)
            if (estimate < 256 || estimate > 511) return false;
    return true;
}(), "ARM estimates fit the implicit one and eight fraction bits");

}  // namespace swift::runtime
