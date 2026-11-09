#pragma once

#include "runtime/frontend/source.h"
#include "types.h"

namespace swift::arm32 {
enum class Opcode : u16 {
    Undefined,
#define ARM32_OPCODE(name) name,
#include "opcodes.inc"
#undef ARM32_OPCODE
    Count
};

enum class FlowKind : u8 { Linear, Conditional, Jump, Call, Return, Service };
struct DecodeOneResult;

// The raw encoding and its identity are retained once, without operand storage.
class Instruction {
public:
    u64 PC() const noexcept { return pc; }

    u32 Address() const noexcept { return Location{pc}.Address(); }

    u32 Word() const noexcept { return word; }

    u32 SemanticWord() const noexcept;

    u16 OpcodeCode() const noexcept { return static_cast<u16>(opcode); }

    Opcode Code() const noexcept { return opcode; }

    u8 Length() const noexcept { return length; }

    bool Thumb() const noexcept { return Location{pc}.Thumb(); }

    Cond Condition() const noexcept;
    const char* Name() const noexcept;
    FlowKind Flow() const noexcept;
    bool RelativeTarget(u64&) const noexcept;
    bool Boundary() const noexcept;
    u64 NextPC() const noexcept;

private:
    u64 pc{};
    u32 word{};
    Opcode opcode{};
    u8 length{};
    u8 reserved{};
    friend DecodeOneResult DecodeOne(std::span<const u8>, u64, runtime::frontend::DecodeTiming*);
};

static_assert(sizeof(Instruction) == 16);
}  // namespace swift::arm32
