#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include "runtime/backend/code.h"

namespace swift::runtime::execution {
struct FaultContext;
}

namespace swift::runtime::backend::arm64 {
struct FaultRecovery {
    std::uint8_t test_bits{}, test_register{}, parity_register{}, carry_direct{};
    std::uint8_t logical_bits{}, logical_register{};
};

constexpr std::uint8_t EncodeDeferredLogical(unsigned bits, unsigned reg) noexcept {
    const unsigned size_code = bits == 8    ? 1u
                               : bits == 16 ? 2u
                               : bits == 32 ? 3u
                               : bits == 64 ? 4u
                                            : 0u;
    return size_code && reg < 31 ? static_cast<std::uint8_t>((size_code << 5) | reg) : 0u;
}

constexpr FaultRecovery DecodeFaultRecovery(const backend::FaultSite& site) noexcept {
    const auto encoded = site.recovery[7];
    const auto size_code = static_cast<unsigned>(encoded >> 5);
    const auto logical_bits = (size_code >= 1 && size_code <= 4)
                                      ? static_cast<std::uint8_t>(4u << size_code)
                                      : std::uint8_t{};
    const auto logical_reg =
            logical_bits ? static_cast<std::uint8_t>(encoded & 0x1fu) : std::uint8_t{};
    return {site.recovery[0],
            site.recovery[1],
            site.recovery[2],
            site.recovery[3],
            logical_bits,
            logical_reg};
}

bool RestoreGuestState(execution::FaultContext&,
                       const backend::FaultSite&,
                       std::span<const std::byte> gprs,
                       std::uint64_t native_flags,
                       std::span<const std::byte> vectors = {}) noexcept;
}  // namespace swift::runtime::backend::arm64
