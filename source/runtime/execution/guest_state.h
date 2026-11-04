#pragma once

#include <cstddef>
#include <cstdint>

namespace swift::runtime::execution {

// distorm/x86-64 order. This is the new runner's actual ABI, independent of
// the retired VM's CPU/thread structures.
enum GPR : unsigned { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
struct GuestState {
    std::uint64_t gprs[16]{};
    std::uint64_t pc{}, packed_flags{1}, fs_base{}, gs_base{};
    alignas(16) std::uint8_t xmm[16][16]{};
    // Preserve the complete guest word, including explicitly written status
    // bits. Numeric operations do not accumulate MXCSR exception state.
    std::uint32_t mxcsr{0x1f80};
};
static_assert(offsetof(GuestState, pc) == 128 && offsetof(GuestState, packed_flags) == 136);
static_assert(offsetof(GuestState, fs_base) == 144 && offsetof(GuestState, gs_base) == 152);
static_assert(offsetof(GuestState, xmm) == 160 && offsetof(GuestState, mxcsr) == 416);
static_assert(sizeof(GuestState) == 432);

} // namespace swift::runtime::execution
