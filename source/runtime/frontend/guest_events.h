#pragma once

#include <cstddef>
#include <cstdint>

namespace swift::runtime::frontend {

inline constexpr std::size_t GuestEventInstructionBudget = 256;

// A guest-PC policy, independent of host pages, native layout and unit cuts.
constexpr bool CrossesGuestEventWindow(std::uint64_t source, std::uint64_t destination) noexcept {
    return (source >> 12) != (destination >> 12);
}

constexpr bool GuestEventTransfer(std::uint64_t source, std::uint64_t destination) noexcept {
    return destination <= source || CrossesGuestEventWindow(source, destination);
}

}  // namespace swift::runtime::frontend
