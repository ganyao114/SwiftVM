#pragma once

#include "runtime/common/types.h"

namespace swift::runtime::backend {

enum class FaultRecoveryKind : u8 {
    GuestFault,
    IndirectCallMiss,
    ExternalContinuation,
};

struct FaultEntry {
    u8* host_start{};
    u8* host_end{};
    u8* owner_start{};
    u8* recovery{};
    VAddr guest_loc{};
    FaultRecoveryKind recovery_kind{FaultRecoveryKind::GuestFault};

    [[nodiscard]] bool Contains(const u8* host_pc) const {
        const auto pc = reinterpret_cast<uintptr_t>(host_pc);
        return pc >= reinterpret_cast<uintptr_t>(host_start) &&
               pc < reinterpret_cast<uintptr_t>(host_end);
    }
};

} // namespace swift::runtime::backend
