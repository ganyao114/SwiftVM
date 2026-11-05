#pragma once

#include <cstdint>

namespace swift::runtime::backend::arm64::abi {

// Linked raw flags keep only the low 32 bits of AF input state. The upper
// tag is internal to one qualified Function loop; every exit is Complete.
inline constexpr std::uint64_t DeferredTestTag = std::uint64_t{1} << 63;
inline constexpr std::uint64_t PackedDynamicFlags = 0xf40000ff;

// The cold directory and retired native tails share one PC mix and layout.
// These are backend/runtime facts, never frontend or public IR facts.
inline constexpr unsigned DispatchSlotShift = 5, DispatchOccupiedOffset = 24, DispatchHashShift = 13;
inline constexpr unsigned ContextSlotsOffset = 24, ContextDispatchesOffset = 56,
        ContextHitsOffset = 64, ContextProbesOffset = 144;
// Per actual dynamic tail: sixteen (PC,entry) pairs plus one writable base alias.
inline constexpr unsigned DynamicCacheEntries = 16, DynamicCacheWords = DynamicCacheEntries * 4 + 2;

// The runtime's pinned Uniform ABI. Callers admit only the actual 8/12/16
// layouts; this one mapping is consumed by allocation and machine emission.
constexpr unsigned PinnedUniformRegister(unsigned slot, unsigned count) noexcept {
    return slot < 8 ? 20 + slot : count == 16 ? slot : slot + 4;
}

constexpr unsigned PinnedUniformSlot(unsigned reg, unsigned count) noexcept {
    return reg >= 20 ? reg - 20 : count == 16 ? reg : reg - 4;
}

constexpr unsigned PinnedVectorRegister(unsigned slot) noexcept { return 16 + slot; }
constexpr unsigned PinnedVectorSlot(unsigned reg) noexcept { return reg - 16; }

// Linked vector pins reserve v16..v31. v8/v9 are writer temporaries;
// the root saves the host's d8..d15 and helpers retain all live Q lanes.
constexpr unsigned VectorHomeRegister(unsigned slot, bool pinned) noexcept {
    return slot < 6 ? slot + 2 : pinned ? slot + 4 : slot + 10;
}

} // namespace swift::runtime::backend::arm64::abi
