#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "instruction.h"
#include "runtime/arch/x86/cpu.h"
#include "runtime/frontend/source.h"

namespace swift::x86 {

// Optional diagnostic accumulator over actual selected byte-decoder calls,
// including unsuccessful attempts. The caller owns/resets this accumulator;
// formation, allocation, buffer writes and validation remain outside it.
using runtime::frontend::DecodeTiming;
enum class DecodeError : std::uint8_t {
    None,
    InvalidInput,
    InvalidEncoding,
    Truncated,
    OverlappingTarget,  // Region formation only; never emitted by DecodeOne.
    UnsupportedGuestFeature,
};

struct DecodeOneResult {
    DecodeError error{DecodeError::None};
    Instruction instruction{};

    explicit operator bool() const noexcept { return error == DecodeError::None; }
};

[[nodiscard]] DecodeOneResult DecodeOne(
        std::span<const std::uint8_t> bytes,
        std::uint64_t pc,
        DecodeTiming* timing = nullptr,
        runtime::x86::GuestCpuModel model = runtime::x86::BaselineGuestCpu);

}  // namespace swift::x86
