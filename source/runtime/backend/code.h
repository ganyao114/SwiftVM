#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include "runtime/ir/types.h"

namespace swift::runtime::backend {

enum class CompileError : std::uint8_t {
    None, InvalidProgram, UnsupportedOpcode, UnsupportedType, UnsupportedOperand,
    UnsupportedFlags, UnsupportedControlFlow, FrameTooLarge, CodeTooLarge,
};

// Byte offsets from the owning native unit, independent of its callable entry.
using NativeOffset = std::uint32_t;
inline constexpr NativeOffset NoNativeOffset = UINT32_MAX;

struct FaultSite {
    NativeOffset first_offset{}, last_offset{};
    std::uint64_t guest_pc{};
    NativeOffset unaligned_offset{NoNativeOffset}, fallback_offset{NoNativeOffset};
    // Backend-owned recovery description. Generic lookup only matches ranges.
    std::array<std::uint8_t, 8> recovery{};
};
static_assert(sizeof(FaultSite) == 32);

enum class LinkKind : std::uint8_t { Static, Dynamic };
struct LinkSite {
    LinkKind kind{LinkKind::Static};
    NativeOffset data_offset{}, branch_offset{}, fallback_offset{};
    std::uint64_t target_pc{}; // Only Static links carry a guest target.
};
static_assert(sizeof(LinkSite) == 24);

struct EntryPoint {
    std::uint64_t pc{};
    NativeOffset offset{};
};
static_assert(sizeof(EntryPoint) == 16);

// A borrowed view. The code heap owns mappings and recovery buffers; every
// callable entry into this unit retains this original base/range descriptor.
struct NativeCode {
    using Function = std::uint64_t(*)(std::uint8_t*);
    Function entry{};
    std::size_t bytes{};
    std::uint8_t* writable{};
    std::span<const FaultSite> faults;
};
static_assert(sizeof(NativeCode) == 40 && std::is_trivially_destructible_v<NativeCode>);

struct CompileResult {
    CompileError error{CompileError::None};
    ir::NodeId node{};
    ir::BlockId block{};
    std::vector<std::uint32_t> words;
    std::uint32_t frame_bytes{}, spill_slots{};
    explicit operator bool() const noexcept { return error == CompileError::None; }
    std::vector<FaultSite> faults;
    std::vector<LinkSite> links;
    std::vector<EntryPoint> entries;
};

} // namespace swift::runtime::backend
