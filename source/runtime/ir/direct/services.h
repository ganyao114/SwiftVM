#pragma once

#include <cstdint>
#include <span>

#include "runtime/atomic.h"

namespace swift::runtime::ir::direct {

enum class AccessStatus : std::uint8_t { Completed, Fault, UnsupportedBeforeAccess };

struct AccessResult {
    AccessStatus status{AccessStatus::Completed};
    std::uint64_t fault_address{};
    // Leave the current execution fragment after the successful write and its
    // architectural commit. A changed source is never retried as a cache miss.
    bool code_changed{};
};

using ::swift::runtime::AtomicOperation;
using ::swift::runtime::AtomicRequest;
using ::swift::runtime::AtomicValue;

struct CpuResult {
    std::uint64_t lower{}, upper{};
};

// Bound by the execution owner. Buffer lengths are the exact access widths;
// unsupported access must be reported before memory or architectural effects.
struct Services {
    void* opaque{};
    AccessResult (*read)(void*, std::uint64_t, std::span<std::uint8_t>, bool ordered) noexcept {};
    AccessResult (*write)(void*,
                          std::uint64_t,
                          std::span<const std::uint8_t>,
                          bool ordered) noexcept {};
    AccessResult (*atomic)(void*, const AtomicRequest&, AtomicValue& observed) noexcept {};
    CpuResult (*cpuid)(void*, std::uint32_t leaf, std::uint32_t subleaf) noexcept {};
    // Read-only readiness query. True exits before the next guest effect.
    bool (*guest_events)(void*) noexcept {};
};

}  // namespace swift::runtime::ir::direct
