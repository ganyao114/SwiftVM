#pragma once

#include <atomic>
#include <limits>
#include "runtime/backend/context.h"

namespace swift::runtime::backend::loongarch64 {

// LoongArch keeps SSA homes on the host stack. These five otherwise unused
// State slots describe only the current, fully checked guest memory access.
// The memory lease and recovery frame continue to use slots 59..63.
constexpr u32 kGuestFaultBaseSlot = 54;
constexpr u32 kGuestFaultSizeSlot = 55;
constexpr u32 kGuestFaultSecondBaseSlot = 56;
constexpr u32 kGuestFaultSecondSizeSlot = 57;
constexpr u32 kGuestFaultCookieSlot = 58;
constexpr u32 state_offset_loongarch_guest_fault_cookie =
        state_offset_spill_area + kGuestFaultCookieSlot * sizeof(u64);

enum class GuestFaultKind : u8 { None, Native, TypedKernel };
struct GuestFaultRange { u64 base{}, size{}; };

constexpr u64 kNativeGuestFaultCookie = 0x53564d4c41363401ULL;
constexpr u64 kTypedGuestFaultCookie = 0x53564d4c41363402ULL;
static_assert(std::atomic_ref<u64>::is_always_lock_free);

inline void ClearGuestFaultScope(State* state) noexcept {
    std::atomic_ref{state->spill_area[kGuestFaultCookieSlot]}.store(0, std::memory_order_release);
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

inline GuestFaultKind GetGuestFaultKind(State* state) noexcept {
    if (!state) return GuestFaultKind::None;
    const auto cookie = std::atomic_ref{state->spill_area[kGuestFaultCookieSlot]}.load(std::memory_order_acquire);
    if (cookie == kNativeGuestFaultCookie) return GuestFaultKind::Native;
    if (cookie == kTypedGuestFaultCookie) return GuestFaultKind::TypedKernel;
    return GuestFaultKind::None;
}

inline bool SetGuestFaultScope(State* state, GuestFaultRange first,
                              GuestFaultKind kind = GuestFaultKind::Native,
                              GuestFaultRange second = {}) noexcept {
    ClearGuestFaultScope(state);
    const auto valid = [](GuestFaultRange range) {
        return range.size && range.size - 1 <= std::numeric_limits<u64>::max() - range.base;
    };
    if (!valid(first) || (second.size && !valid(second)) ||
        (kind != GuestFaultKind::Native && kind != GuestFaultKind::TypedKernel)) return false;
    state->spill_area[kGuestFaultBaseSlot] = first.base;
    state->spill_area[kGuestFaultSizeSlot] = first.size;
    state->spill_area[kGuestFaultSecondBaseSlot] = second.base;
    state->spill_area[kGuestFaultSecondSizeSlot] = second.size;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    std::atomic_ref{state->spill_area[kGuestFaultCookieSlot]}.store(
            kind == GuestFaultKind::Native ? kNativeGuestFaultCookie : kTypedGuestFaultCookie,
            std::memory_order_release);
    return true;
}

// Range membership is necessary but does not authorize a recovery frame or
// host PC. Runtime must additionally validate those against its own block.
inline GuestFaultKind ClassifyGuestFault(State* state, u64 fault_address) noexcept {
    const auto kind = GetGuestFaultKind(state);
    if (kind == GuestFaultKind::None) return kind;
    const auto contains = [=](u32 base_slot, u32 size_slot) {
        const auto base = state->spill_area[base_slot];
        const auto size = state->spill_area[size_slot];
        return fault_address >= base && fault_address - base < size;
    };
    return contains(kGuestFaultBaseSlot, kGuestFaultSizeSlot) ||
                   contains(kGuestFaultSecondBaseSlot, kGuestFaultSecondSizeSlot)
            ? kind : GuestFaultKind::None;
}

inline bool IsGuestMemoryHelperFault(State* state, u64 fault_address) noexcept {
    return ClassifyGuestFault(state, fault_address) == GuestFaultKind::TypedKernel;
}

}  // namespace swift::runtime::backend::loongarch64
