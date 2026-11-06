#pragma once
#include <array>
#include <cstdint>
#include <span>
#include "state.h"

namespace swift::runtime::x86 {
enum class StateImageKind : std::uint8_t {
    FxSave,
    FxRestore,
    XSave,
    XRestore,
    XSaveOpt,
    XSaveCompact
};
inline constexpr std::uint64_t GuestXcr0 = 7;
inline constexpr std::uint64_t XsaveCompactedFormat = std::uint64_t{1} << 63;
inline constexpr std::uint32_t GuestMxcsrMask = 0xffff;
inline constexpr std::size_t XsaveBytes = 832;
using StateImage = std::array<std::uint8_t, XsaveBytes>;
void ExportLegacyState(const GuestState&, std::span<std::uint8_t, 512>, bool wide = true) noexcept;
bool ImportLegacyState(GuestState&,
                       std::span<const std::uint8_t, 512>,
                       std::uint64_t components = 3,
                       bool wide = true) noexcept;
}  // namespace swift::runtime::x86
