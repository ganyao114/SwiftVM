#pragma once

#include <cstdint>
#include <limits>

namespace swift::runtime::backend {

using NativeOffset = std::uint32_t;
inline constexpr NativeOffset NoNativeOffset = UINT32_MAX;

// Stable semantic IDs, not addresses or offsets in the current executable.
// The host binding resolves each ID using the code/helper ABI of this build.
enum class HostSymbol : std::uint16_t {
    None = 0,
    Dispatcher = 1,
    Exit = 2,
    GuestCpuQuery = 3,
    GuestSystemQuery = 4,
    ArmPairBridge = 16,
    ArmX87Bridge = 17,
    ArmVectorBridge = 18,
    ArmDivideU8 = 20,
    ArmDivideU16 = 21,
    ArmDivideU32 = 22,
    ArmDivideU64 = 23,
    ArmDivideS8 = 24,
    ArmDivideS16 = 25,
    ArmDivideS32 = 26,
    ArmDivideS64 = 27,
    ArmStringImplicit = 32,
    ArmStringExplicit = 33,
    ArmCrypto = 34,
    ArmDepositBits = 35,
    ArmExtractBits = 36,
    RiscvWideDivide = 48,
    RiscvCheckedX87 = 49,
    X87Execute = 50,
    StringImplicit = 51,
    StringExplicit = 52,
    Crypto = 53,
    CompareExchange128 = 54,
    DepositBits = 55,
    ExtractBits = 56,
};

enum class RelocationKind : std::uint16_t {
    Absolute64 = 1,       // An actual aligned 8-byte literal in the unit.
    Arm64MoveWide64 = 2,  // Four fixed MOVZ/MOVK instructions, shifts 0/16/32/48.
};

struct Relocation {
    NativeOffset offset{};
    RelocationKind kind{};
    HostSymbol symbol{HostSymbol::None};
    std::int64_t addend{};
};

static_assert(sizeof(Relocation) == 16);

namespace detail {
inline bool RelocationAddress(std::uintptr_t symbol,
                              std::int64_t addend,
                              std::uintptr_t& address) noexcept {
    if (!symbol) return false;
    if (addend >= 0) {
        const auto extra = static_cast<std::uint64_t>(addend);
        if (extra > std::numeric_limits<std::uintptr_t>::max() - symbol) return false;
        address = symbol + extra;
    } else {
        const auto extra = std::uint64_t{0} - static_cast<std::uint64_t>(addend);
        if (extra > symbol) return false;
        address = symbol - extra;
    }
    return true;
}
}  // namespace detail
}  // namespace swift::runtime::backend
