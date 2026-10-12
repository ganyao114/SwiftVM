#pragma once

#include "runtime/common/types.h"

namespace swift::runtime::backend::x64 {

enum class GuestAccessKind : u8 { Read, Write, ReadWrite };

// Offsets name exactly one emitted direct [R14] guest-memory instruction,
// including its prefixes. R14 remains the effective host address on faults.
// No helper, State/uniform, stack or address-validation code belongs here.
struct GuestAccessRange {
    u32 begin_offset{};
    u32 end_offset{};
    u32 size{};
    GuestAccessKind kind{};
};

// This describes guest-access site coverage, not Runtime recovery capability.
enum class GuestAccessCoverage : u8 { Precise, Unsupported };

} // namespace swift::runtime::backend::x64
