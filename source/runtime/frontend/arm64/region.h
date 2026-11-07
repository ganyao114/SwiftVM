#pragma once
#include <vector>
#include "decode.h"
#include "runtime/code_source.h"

namespace swift::arm64 {
using runtime::frontend::BlockWindow;
using runtime::frontend::DecodeLimits;
using runtime::frontend::ReadyEntryLookup;

struct DecodedBlock {
    std::uint64_t pc{};
    std::uint32_t first_instruction{}, instruction_count{};
};

struct DecodedFunction {
    DecodeError error{DecodeError::None};
    std::uint64_t error_pc{};
    std::vector<Instruction> instructions;
    std::vector<DecodedBlock> blocks;
    std::uint32_t entry_block{};
    std::size_t decoded_bytes{}, decode_calls{}, splits{}, budget_exits{};

    explicit operator bool() const noexcept { return error == DecodeError::None; }
};

DecodedFunction DecodeFunction(std::span<const BlockWindow>,
                               std::uint64_t entry,
                               DecodeLimits = {},
                               DecodeTiming* = nullptr,
                               ReadyEntryLookup = {},
                               std::vector<std::uint64_t>* continuations = nullptr);
}  // namespace swift::arm64
