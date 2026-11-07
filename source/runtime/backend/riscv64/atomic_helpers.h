#pragma once

#include <cstdint>

namespace swift::runtime::backend::riscv64 {

struct AtomicPair {
    std::uint64_t low{}, high{};
};

// The writer admits only 16-byte aligned addresses. RV64G has no pair CAS,
// so this one operation uses the platform's 128-bit atomic implementation.
AtomicPair CompareExchange128(std::uint64_t address,
                              std::uint64_t expected_low,
                              std::uint64_t expected_high,
                              std::uint64_t desired_low,
                              std::uint64_t desired_high) noexcept;

}  // namespace swift::runtime::backend::riscv64
