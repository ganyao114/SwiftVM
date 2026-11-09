#pragma once

#include "instruction.h"
#include "runtime/frontend/source.h"

namespace swift::arm32 {
using runtime::frontend::DecodeTiming;
enum class DecodeError : u8 { None, InvalidInput, InvalidEncoding, Truncated, OverlappingTarget };

struct DecodeOneResult {
    DecodeError error{DecodeError::None};
    Instruction instruction;

    explicit operator bool() const noexcept { return error == DecodeError::None; }
};

bool IsThumb32(u16 first_halfword) noexcept;
DecodeOneResult DecodeOne(std::span<const u8>, u64 location, DecodeTiming* = nullptr);
}  // namespace swift::arm32
