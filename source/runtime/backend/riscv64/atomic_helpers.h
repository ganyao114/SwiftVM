#pragma once

#include <cstdint>

namespace swift::runtime::backend::riscv64 {

struct AtomicPair {
    std::uint64_t low{}, high{};
};

// Standalone private/single-thread fallback. Linked units use the execution
// owner's atomic service instead; this does not qualify shared guest memory.
// The writer admits only 16-byte aligned addresses.
AtomicPair CompareExchange128(std::uint64_t address,
                              std::uint64_t expected_low,
                              std::uint64_t expected_high,
                              std::uint64_t desired_low,
                              std::uint64_t desired_high) noexcept;

extern "C" std::uint64_t SvmRVCompareExchange64(std::uint64_t address,
                                                std::uint64_t expected,
                                                std::uint64_t desired,
                                                std::uint64_t* observed,
                                                std::uint64_t* fault_address) noexcept;
extern "C" const char SvmRVCompareExchange64Begin[], SvmRVCompareExchange64End[],
        SvmRVCompareExchange64Fault[];

}  // namespace swift::runtime::backend::riscv64
