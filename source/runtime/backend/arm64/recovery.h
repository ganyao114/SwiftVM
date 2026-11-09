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
};

constexpr FaultRecovery DecodeFaultRecovery(const backend::FaultSite& site) noexcept {
    return {site.recovery[0], site.recovery[1], site.recovery[2], site.recovery[3]};
}

bool RestoreGuestState(execution::FaultContext&,
                       const backend::FaultSite&,
                       std::span<const std::byte> gprs,
                       std::uint64_t native_flags,
                       std::span<const std::byte> vectors = {}) noexcept;
}  // namespace swift::runtime::backend::arm64
