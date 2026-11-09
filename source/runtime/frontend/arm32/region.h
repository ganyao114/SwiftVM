#pragma once

#include <array>
#include <vector>
#include "decode.h"
#include "runtime/code_source.h"

namespace swift::arm32 {
using runtime::frontend::BlockWindow;
using runtime::frontend::DecodeLimits;
using runtime::frontend::ReadyEntryLookup;

struct DecodedBlock {
    u64 pc{};
    u32 first_instruction{}, instruction_count{};
};

struct DecodedFunction {
    DecodeError error{DecodeError::None};
    u64 error_pc{};
    std::vector<Instruction> instructions;
    std::vector<DecodedBlock> blocks;
    u32 entry_block{};
    std::size_t decoded_bytes{}, decode_calls{}, splits{}, budget_exits{};

    explicit operator bool() const noexcept { return error == DecodeError::None; }
};

// Windows describe physical bytes; all control-flow identities are full Locations.
std::span<const u8> SourceBytes(std::span<const BlockWindow>, u32 address) noexcept;
std::span<const u8> InstructionBytes(std::span<const BlockWindow>,
                                     u32 address,
                                     std::array<u8, 4>& edge) noexcept;
DecodedFunction DecodeFunction(std::span<const BlockWindow>,
                               u64 entry,
                               DecodeLimits = {},
                               DecodeTiming* = nullptr,
                               ReadyEntryLookup = {},
                               std::vector<u64>* continuations = nullptr);
}  // namespace swift::arm32
