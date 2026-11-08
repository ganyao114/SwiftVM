#pragma once

#include <cstddef>
#include <cstdint>

#include "x87.h"

namespace swift::runtime::x86 {

// Architectural x86-64 register order, shared by IR construction and execution.
enum GPR : unsigned {
    RAX,
    RCX,
    RDX,
    RBX,
    RSP,
    RBP,
    RSI,
    RDI,
    R8,
    R9,
    R10,
    R11,
    R12,
    R13,
    R14,
    R15
};

struct GuestState {
    std::uint64_t gprs[16]{};
    std::uint64_t pc{}, packed_flags{1}, fs_base{}, gs_base{};
    alignas(16) std::uint8_t xmm[16][16]{};
    // Architectural controls and sticky SSE exception state. Native status is
    // merged at observers; direct execution accumulates it per instruction.
    std::uint32_t mxcsr{0x1f80};
    X87State x87;
    std::uint64_t x87_ip{}, x87_dp{};
    std::uint16_t x87_opcode{};
    alignas(16) std::uint8_t ymm_high[16][16]{};
};

static_assert(offsetof(GuestState, pc) == 128 && offsetof(GuestState, packed_flags) == 136);
static_assert(offsetof(GuestState, fs_base) == 144 && offsetof(GuestState, gs_base) == 152);
static_assert(offsetof(GuestState, xmm) == 160 && offsetof(GuestState, mxcsr) == 416);
static_assert(offsetof(GuestState, x87) == 432 && offsetof(GuestState, x87_ip) == 576);
static_assert(offsetof(GuestState, ymm_high) == 608 && sizeof(GuestState) == 864);

}  // namespace swift::runtime::x86
