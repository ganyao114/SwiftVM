#pragma once

#include <bit>
#include <cstdint>

namespace swift::runtime::x86::flags {

// Portable state snapshot. Its low byte is a parity source, not x86 PF.
// ARM64's live NZCV and PF/AF carriers are converted at native boundaries.
inline constexpr std::uint64_t Carry = UINT64_C(1) << 29;
inline constexpr std::uint64_t Zero = UINT64_C(1) << 30;
inline constexpr std::uint64_t Sign = UINT64_C(1) << 31;
inline constexpr std::uint64_t Overflow = UINT64_C(1) << 28;
inline constexpr std::uint64_t Auxiliary = UINT64_C(1) << 26;
inline constexpr std::uint64_t Direction = UINT64_C(1) << 10;
inline constexpr std::uint64_t Arithmetic = Carry | Zero | Sign | Overflow | Auxiliary | 255;

constexpr bool GetCF(std::uint64_t value) noexcept { return (value & Carry) != 0; }
constexpr bool GetPF(std::uint64_t value) noexcept {
    return (std::popcount(static_cast<unsigned>(value & 255)) & 1) == 0;
}
constexpr bool GetAF(std::uint64_t value) noexcept { return (value & Auxiliary) != 0; }
constexpr bool GetZF(std::uint64_t value) noexcept { return (value & Zero) != 0; }
constexpr bool GetSF(std::uint64_t value) noexcept { return (value & Sign) != 0; }
constexpr bool GetOF(std::uint64_t value) noexcept { return (value & Overflow) != 0; }
constexpr bool GetDF(std::uint64_t value) noexcept { return (value & Direction) != 0; }

constexpr std::uint64_t ToRflags(std::uint64_t value) noexcept {
    return UINT64_C(0x202) | (value & Direction) | std::uint64_t{GetCF(value)} |
           (std::uint64_t{GetPF(value)} << 2) | (std::uint64_t{GetAF(value)} << 4) |
           (std::uint64_t{GetZF(value)} << 6) | (std::uint64_t{GetSF(value)} << 7) |
           (std::uint64_t{GetOF(value)} << 11);
}

constexpr std::uint64_t FromRflags(std::uint64_t value, std::uint64_t previous = 0) noexcept {
    return (previous & ~(Arithmetic | Direction)) | (value & Direction) |
           ((value & 1) << 29) | ((value & 4) ? 0 : 1) |
           (((value >> 4) & 1) << 26) | (((value >> 6) & 1) << 30) |
           (((value >> 7) & 1) << 31) | (((value >> 11) & 1) << 28);
}

} // namespace swift::runtime::x86::flags
