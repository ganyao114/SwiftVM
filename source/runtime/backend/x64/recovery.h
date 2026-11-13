#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include "runtime/backend/code.h"

namespace swift::runtime::execution {
struct FaultContext;
}

namespace swift::runtime::backend::x64 {
constexpr std::uint64_t FlagsImageToNative(std::uint64_t image) noexcept {
    return ((image >> 8) & 0xc1) | ((image & 1) << 11);
}

inline bool ValidRecovery(const FaultSite& site, std::uint32_t frame_bytes = UINT32_MAX) noexcept {
    const auto mode = site.recovery[0] & 3;
    if (mode > 2 || (site.recovery[0] & ~31u) || (mode != 2 && site.recovery[0] != mode))
        return false;
    if (mode != 2) return true;
    const auto native = site.recovery[1] & 15, saved = site.recovery[1] >> 4;
    const auto offset = site.recovery[2] | (std::uint32_t{site.recovery[3]} << 8) |
                        (std::uint32_t{site.recovery[7]} << 16);
    return !(native & saved) && (native | saved) &&
           (!saved || (!(offset & 7) && frame_bytes >= 8 && offset <= frame_bytes - 8));
}

bool RestoreGuestState(execution::FaultContext&,
                       const FaultSite&,
                       std::span<const std::byte> gprs,
                       std::uint64_t native_flags = 0) noexcept;
}  // namespace swift::runtime::backend::x64
