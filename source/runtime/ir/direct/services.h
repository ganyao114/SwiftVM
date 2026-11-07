#pragma once

#include <cstdint>
#include <span>

namespace swift::runtime::ir::direct {

enum class AccessStatus : std::uint8_t { Completed, Fault, UnsupportedBeforeAccess };

struct AccessResult {
    AccessStatus status{AccessStatus::Completed};
    std::uint64_t fault_address{};
    // Leave the current execution fragment after the successful write and its
    // architectural commit. A changed source is never retried as a cache miss.
    bool code_changed{};
};

struct AtomicValue {
    std::uint64_t low{}, high{};
};

enum class AtomicOperation : std::uint8_t {
    CompareExchange,
    Exchange,
    FetchAdd,
    Add,
    Sub,
    And,
    Or,
    Xor,
    Neg,
    AddCarry,
    SubBorrow,
    CompareExchange128,
};

struct AtomicRequest {
    std::uint64_t address{};
    AtomicOperation operation{};
    unsigned bits{};
    AtomicValue expected, desired;
    bool carry{};
};

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
};

}  // namespace swift::runtime::ir::direct
