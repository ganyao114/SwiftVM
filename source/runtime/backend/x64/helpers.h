#pragma once
#include <cstdint>

namespace swift::runtime::backend::x64 {
struct Pair {
    std::uint64_t first, second;
};

Pair WideDivide(std::uint64_t, std::uint64_t, std::uint64_t, unsigned, std::uint64_t*) noexcept;
std::uint64_t CheckedX87(
        void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t) noexcept;
std::uint64_t RoundIntegral(std::uint64_t, unsigned, unsigned, unsigned) noexcept;
std::uint64_t FusedMultiplyAdd(
        std::uint64_t, std::uint64_t, std::uint64_t, unsigned, unsigned, unsigned) noexcept;
Pair CompareExchange128(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t) noexcept;
}  // namespace swift::runtime::backend::x64
