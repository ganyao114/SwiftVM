#pragma once

#include <cstdint>

namespace swift::runtime::execution {

// Immutable guest model, independent of the host ISA and per-unit options.
// The pair is {EAX | EBX<<32, ECX | EDX<<32}; the callback has no guest-state
// pointer and changes no architectural state. The backend owns its call ABI.
struct CpuQueryResult {
    std::uint64_t lower{}, upper{};
};
[[nodiscard]] CpuQueryResult QueryGuestCpu(std::uint64_t leaf, std::uint64_t subleaf) noexcept;

} // namespace swift::runtime::execution
