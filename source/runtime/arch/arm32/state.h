#pragma once
#include <cstddef>
#include <cstdint>
#include "location.h"

namespace swift::runtime::arm32 {
struct alignas(16) GuestState {
    std::uint64_t r[15]{};
    std::uint64_t psr_aux{0x10};
    std::uint64_t pc{}, packed_flags{1};
    alignas(16) std::uint8_t q[16][16]{};
    std::uint32_t fpscr{}, numeric_control{0x1f80};
    std::uint32_t tpidrurw{}, tpidruro{};
    std::uint32_t service_word{}, helper_status{};
    std::uint32_t reservation_valid{};
    std::uint64_t service_pc{};
    std::uint64_t numeric_addend{};

    std::uint32_t Cpsr() const noexcept {
        const auto location = Location{pc};
        const auto it = location.IT().Value();
        return static_cast<std::uint32_t>(psr_aux & 0x090f01df) |
               static_cast<std::uint32_t>(packed_flags & 0xf0000000) |
               (std::uint32_t(location.Thumb()) << 5) | (std::uint32_t(location.EFlag()) << 9) |
               ((std::uint32_t(it) & 3) << 25) | ((std::uint32_t(it) & 252) << 8);
    }

    void SetCpsr(std::uint32_t cpsr) noexcept {
        packed_flags = (packed_flags & ~UINT64_C(0xf0000000)) | (cpsr & 0xf0000000);
        psr_aux = cpsr & 0x090f01df;
        const auto it = static_cast<std::uint8_t>(((cpsr >> 25) & 3) | ((cpsr >> 8) & 252));
        pc = Location::Make(static_cast<std::uint32_t>(pc), cpsr & 32, it, fpscr)
                     .SetEFlag(cpsr & 512)
                     .Key();
    }

    void RekeyFPSCR() noexcept { pc = Location{pc}.SetFPSCR(fpscr).Key(); }

    std::uint32_t Register(unsigned index) const noexcept {
        return index == 15 ? static_cast<std::uint32_t>(pc) : static_cast<std::uint32_t>(r[index]);
    }
};

static_assert(offsetof(GuestState, psr_aux) == 120 && offsetof(GuestState, pc) == 128);
static_assert(offsetof(GuestState, packed_flags) == 136 && offsetof(GuestState, q) == 144);
static_assert(offsetof(GuestState, fpscr) == 400 && offsetof(GuestState, service_pc) == 432);
static_assert(sizeof(GuestState) == 448);
}  // namespace swift::runtime::arm32
