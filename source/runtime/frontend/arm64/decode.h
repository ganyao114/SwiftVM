#pragma once
#include <cstdint>
#include <span>
#include "runtime/frontend/source.h"

namespace swift::arm64 {
using runtime::frontend::DecodeTiming;
enum class DecodeError : std::uint8_t {
    None,
    InvalidInput,
    InvalidEncoding,
    Truncated,
    OverlappingTarget
};
enum class FlowKind : std::uint8_t { Linear, Conditional, Jump, Call, Return, Service };
struct DecodeOneResult;

class Instruction {
public:
    std::uint64_t PC() const noexcept { return pc; }

    std::uint32_t Word() const noexcept { return word; }

    std::uint16_t FamilyCode() const noexcept { return family; }

    FlowKind Flow() const noexcept;
    bool RelativeTarget(std::uint64_t&) const noexcept;
    bool Boundary() const noexcept;

private:
    std::uint64_t pc{};
    std::uint32_t word{};
    std::uint16_t family{};
    friend DecodeOneResult DecodeOne(std::span<const std::uint8_t>, std::uint64_t, DecodeTiming*);
};

static_assert(sizeof(Instruction) == 16);

struct DecodeOneResult {
    DecodeError error{DecodeError::None};
    Instruction instruction;

    explicit operator bool() const noexcept { return error == DecodeError::None; }
};

DecodeOneResult DecodeOne(std::span<const std::uint8_t>, std::uint64_t pc, DecodeTiming* = nullptr);
}  // namespace swift::arm64
