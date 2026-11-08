#pragma once

#include <cstdint>
#include "abi_layout.inc"

namespace swift::runtime::backend::arm64::abi {

// Linked raw flags keep only the low 32 bits of AF input state. The upper
// tag is internal to one qualified Function loop; every exit is Complete.
inline constexpr std::uint64_t DeferredTestTag = std::uint64_t{1} << 63;
inline constexpr std::uint64_t PackedDynamicFlags = 0xf40000ff;

// The cold directory and retired native tails share one PC mix and layout.
// These are backend/runtime facts, never frontend or public IR facts.
inline constexpr unsigned DispatchSlotShift = SVM_A64_SLOT_SHIFT,
                          DispatchOccupiedOffset = SVM_A64_OCCUPIED_OFFSET,
                          DispatchHashShift = SVM_A64_DISPATCH_HASH_SHIFT;
inline constexpr unsigned L1Bits = SVM_A64_L1_BITS, L1EntryShift = SVM_A64_L1_ENTRY_SHIFT;
inline constexpr unsigned StateRegister = SVM_A64_STATE_REG, ExtraScalarRegister = 28;

constexpr int ContextOffset(unsigned offset) noexcept {
    return static_cast<int>(offset) - SVM_A64_CTX_STATE;
}

inline constexpr int ContextSlotsOffset = ContextOffset(SVM_A64_CTX_SLOTS),
                     ContextDispatchesOffset = ContextOffset(SVM_A64_CTX_DISPATCHES),
                     ContextHitsOffset = ContextOffset(SVM_A64_CTX_HITS),
                     ContextProbesOffset = ContextOffset(SVM_A64_CTX_PROBES),
                     ContextL1Offset = ContextOffset(SVM_A64_CTX_L1);

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

// Linked units append x28 after the requested baseline scalar bank. Standalone
// units use only its original x2-based slots and retain their save convention.
constexpr unsigned ScalarHomeRegister(unsigned slot, unsigned base_registers) noexcept {
    return slot == base_registers ? ExtraScalarRegister : slot + 2;
}

// Linked vector pins reserve v16..v31. v8/v9 are writer temporaries;
// the root saves the host's d8..d15 and helpers retain all live Q lanes.
constexpr unsigned VectorHomeRegister(unsigned slot, bool pinned) noexcept {
    return slot < 6 ? slot + 2 : pinned ? slot + 4 : slot + 10;
}

}  // namespace swift::runtime::backend::arm64::abi
