#pragma once

#include <cstdint>

namespace swift::runtime::x86 {
inline std::uint64_t ParallelBitDeposit(std::uint64_t value, std::uint64_t mask) noexcept {
    std::uint64_t result{}, source_bit = 1;
    while (mask) {
        const auto lowest = mask & (std::uint64_t{} - mask);
        if (value & source_bit) result |= lowest;
        mask ^= lowest;
        source_bit <<= 1;
    }
    return result;
}

inline std::uint64_t ParallelBitExtract(std::uint64_t value, std::uint64_t mask) noexcept {
    std::uint64_t result{}, result_bit = 1;
    while (mask) {
        const auto lowest = mask & (std::uint64_t{} - mask);
        if (value & lowest) result |= result_bit;
        mask ^= lowest;
        result_bit <<= 1;
    }
    return result;
}
}  // namespace swift::runtime::x86
