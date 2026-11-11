#pragma once

#include <cstdint>

namespace swift::runtime {

// Guest ELF machine identities, independent of the selected host backend.
enum class GuestISA : std::uint16_t { Arm32 = 40, X86_64 = 62, Arm64 = 183 };

// Compiled entries may include architectural translation context. Memory
// ownership and source dependencies always use the physical guest address.
constexpr std::uint64_t CodeAddress(GuestISA isa, std::uint64_t location) noexcept {
    return isa == GuestISA::Arm32 ? std::uint32_t(location) : location;
}

// Cold binding of one actual Uniform buffer. Register identities stay in the
// frontend; backend publication and recovery consume only byte offsets.
struct GuestLayout {
    GuestISA isa{GuestISA::X86_64};
    std::uint32_t state_bytes{}, gprs_offset{}, pc_offset{}, flags_offset{}, stack_offset{};
    std::uint32_t vectors_offset{UINT32_MAX}, numeric_control_offset{UINT32_MAX};
    std::uint8_t gpr_count{}, vector_count{};
    // Effective guest-register pins requested for this guest.
    std::uint8_t pinned_uniform_slots{};
};

}  // namespace swift::runtime
