#pragma once

#include <cstdint>

namespace swift::runtime::x86::sse42 {
struct Block128 {
    std::uint64_t low{}, high{};
};
inline constexpr unsigned MaskShift = 0, IndexShift = 16;
inline constexpr unsigned SignBit = 24, ZeroBit = 25, CarryBit = 26, OverflowBit = 27;

// All sixteen bytes are inputs, independent of a terminator or explicit
// length. The memory access itself belongs to the instruction's caller.
std::uint64_t CompareImplicit(Block128 left, Block128 right, std::uint8_t control) noexcept;
// lengths.low/high contain raw RAX/RDX. Control bit8 selects signed64 lengths;
// otherwise only their signed32 low halves participate. The absolute length
// saturates at 8 words or 16 bytes, including INT_MIN.
std::uint64_t CompareExplicit(Block128 left,
                              Block128 right,
                              Block128 lengths,
                              std::uint16_t control) noexcept;
}  // namespace swift::runtime::x86::sse42
