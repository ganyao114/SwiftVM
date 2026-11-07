#pragma once
#include <cstddef>
#include <cstdint>

namespace swift::runtime::arm64 {
struct GuestState {
    std::uint64_t gprs[32]{};
    std::uint64_t pc{}, packed_flags{1};
    alignas(16) std::uint8_t q[32][16]{};
    std::uint32_t fpcr{}, fpsr{};
    std::uint64_t tpidr_el0{};
    std::uint32_t numeric_control{0x1f80};
    // Cold numerical helpers report unsupported controls before committing.
    std::uint32_t helper_status{};
};

static_assert(offsetof(GuestState, pc) == 256 && offsetof(GuestState, packed_flags) == 264);
static_assert(offsetof(GuestState, q) == 272 && offsetof(GuestState, fpcr) == 784);
static_assert(offsetof(GuestState, fpsr) == 788 && offsetof(GuestState, tpidr_el0) == 792);
static_assert(offsetof(GuestState, numeric_control) == 800 && sizeof(GuestState) == 816);
}  // namespace swift::runtime::arm64
