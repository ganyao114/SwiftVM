#pragma once
#include <cstdint>
#include <optional>
#include "runtime/ir/types.h"

namespace swift::runtime::arm32 {
inline constexpr std::uint32_t FPSCRModeMask = 0x07f70000;

class ITState {
public:
    constexpr ITState() = default;

    explicit constexpr ITState(std::uint8_t bits) : bits(bits) {}

    constexpr std::uint8_t Value() const noexcept { return bits; }

    constexpr bool IsInITBlock() const noexcept { return (bits & 15) != 0; }

    constexpr bool IsLastInITBlock() const noexcept { return (bits & 15) == 8; }

    constexpr ir::Condition Cond() const noexcept {
        return IsInITBlock() ? static_cast<ir::Condition>(bits >> 4) : ir::Condition::AL;
    }

    constexpr ITState Advance() const noexcept {
        return ITState{
                static_cast<std::uint8_t>((bits & 7) ? (bits & 0xe0) | ((bits << 1) & 31) : 0)};
    }

private:
    std::uint8_t bits{};
};

class FPSCR {
public:
    constexpr FPSCR(std::uint32_t value = 0) : value(value) {}

    constexpr std::uint32_t Value() const noexcept { return value; }

    constexpr std::size_t Len() const noexcept { return ((value >> 16) & 7) + 1; }

    constexpr std::optional<std::size_t> Stride() const noexcept {
        switch ((value >> 20) & 3) {
            case 0:
                return 1;
            case 3:
                return 2;
            default:
                return {};
        }
    }

    constexpr unsigned RMode() const noexcept { return (value >> 22) & 3; }

    constexpr bool AHP() const noexcept { return value & (1u << 26); }

    constexpr bool DN() const noexcept { return value & (1u << 25); }

    constexpr bool FTZ() const noexcept { return value & (1u << 24); }

private:
    std::uint32_t value;
};

class Location {
public:
    constexpr Location(std::uint64_t key = 0) : key(key) {}

    static constexpr Location Make(std::uint32_t address,
                                   bool thumb = false,
                                   std::uint8_t it = 0,
                                   std::uint32_t fpscr = 0) noexcept {
        return Location{
                std::uint64_t(address) |
                (std::uint64_t((fpscr & FPSCRModeMask) | (std::uint32_t(it) << 8) | unsigned(thumb))
                 << 32)};
    }

    constexpr std::uint64_t Key() const noexcept { return key; }

    constexpr std::uint64_t UniqueHash() const noexcept { return key; }

    constexpr std::uint32_t Address() const noexcept { return key; }

    constexpr std::uint32_t PC() const noexcept { return Address(); }

    constexpr std::uint32_t Context() const noexcept { return key >> 32; }

    constexpr bool Thumb() const noexcept { return Context() & 1; }

    constexpr bool TFlag() const noexcept { return Thumb(); }

    constexpr bool EFlag() const noexcept { return Context() & 2; }

    constexpr ITState IT() const noexcept {
        return ITState{static_cast<std::uint8_t>(Context() >> 8)};
    }

    constexpr auto FPSCR() const noexcept -> arm32::FPSCR {
        return arm32::FPSCR{Context() & FPSCRModeMask};
    }

    constexpr Location WithPC(std::uint32_t pc) const noexcept {
        return Location{(key & ~UINT64_C(0xffffffff)) | pc};
    }

    constexpr Location SetPC(std::uint32_t pc) const noexcept { return WithPC(pc); }

    constexpr Location Advance(unsigned bytes) const noexcept { return WithPC(Address() + bytes); }

    constexpr Location AdvancePC(int bytes) const noexcept { return WithPC(Address() + bytes); }

    constexpr Location SetTFlag(bool thumb) const noexcept {
        return Location{(key & ~(UINT64_C(1) << 32)) | (std::uint64_t(thumb) << 32)};
    }

    constexpr Location SetEFlag(bool big) const noexcept {
        return Location{(key & ~(UINT64_C(2) << 32)) | (std::uint64_t(big) << 33)};
    }

    constexpr Location SetIT(ITState it) const noexcept {
        return Location{(key & ~(UINT64_C(255) << 40)) | (std::uint64_t(it.Value()) << 40)};
    }

    constexpr Location AdvanceIT() const noexcept { return SetIT(IT().Advance()); }

    constexpr Location SetFPSCR(std::uint32_t fpscr) const noexcept {
        return Location{(key & ~(std::uint64_t(FPSCRModeMask) << 32)) |
                        (std::uint64_t(fpscr & FPSCRModeMask) << 32)};
    }

    constexpr operator std::uint64_t() const noexcept { return key; }

    friend constexpr bool operator==(Location, Location) = default;

private:
    std::uint64_t key;
};
}  // namespace swift::runtime::arm32
