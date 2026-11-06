#pragma once

#include <cstdint>

namespace swift::runtime::x86 {

struct GuestCpuModel {
    bool cet_ibt{}, cet_shadow_stack{};
};
inline constexpr GuestCpuModel BaselineGuestCpu{};

// Immutable guest model, independent of the host ISA and per-unit options.
// The pair is {EAX | EBX<<32, ECX | EDX<<32}; the callback has no guest-state
// pointer and changes no architectural state. The backend owns its call ABI.
struct CpuQueryResult {
    std::uint64_t lower{}, upper{};
};
[[nodiscard]] CpuQueryResult QueryGuestCpu(std::uint64_t leaf, std::uint64_t subleaf) noexcept;
// Fixed 1 GHz guest TSC, independent of the host CPU's counter frequency.
[[nodiscard]] CpuQueryResult QueryGuestSystem(std::uint64_t kind, std::uint64_t argument) noexcept;

}  // namespace swift::runtime::x86
