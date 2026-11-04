#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "runtime/externals/distorm/distorm.h"

namespace swift::x86 {

struct BlockWindow {
    std::uint64_t pc{};
    std::span<const std::uint8_t> bytes;
};
struct DecodedBlock {
    std::uint64_t pc{};
    std::uint32_t first_instruction{}, instruction_count{};
};
static_assert(sizeof(DecodedBlock) == 16);

struct DecodeLimits {
    std::uint32_t max_instructions{4096}, max_blocks{256};
};
// Optional diagnostic accumulator over actual distorm_decompose calls only,
// including unsuccessful attempts. The caller owns/resets this accumulator;
// formation, allocation, buffer writes and validation remain outside it.
struct DecodeTiming { std::uint64_t nanoseconds{}, calls{}; };
enum class DecodeError : std::uint8_t {
    None, InvalidInput, InvalidEncoding, Truncated, OverlappingTarget,
};
struct DecodedFunction {
    DecodeError error{DecodeError::None};
    std::uint64_t error_pc{};
    std::vector<_DInst> instructions;
    std::vector<DecodedBlock> blocks;
    std::uint32_t entry_block{};
    // Committed valid bytes; actual parser calls include a failed attempt.
    // splits counts accepted late entries inside already decoded slices.
    // budget_exits counts actual edge/body-frontier decisions, not unique PCs.
    std::size_t decoded_bytes{}, decode_calls{}, splits{}, budget_exits{};
    explicit operator bool() const noexcept { return error == DecodeError::None; }
};

// Discover a bounded reachable region, not an ABI/symbol-defined function.
// Readable windows must be ordered, nonempty and nonoverlapping; an instruction
// must fit in one window. A window end is a sequential exit frontier; explicit
// direct edges can enter other windows. Bytes stay immutable/alive throughout
// this call and are borrowed, never copied.
// One append-only instruction buffer owns all records. Blocks are sorted-PC
// slices of it; splitting never reorders or re-decodes a committed record.
// Ordinary geometric vector growth may relocate the buffer; slices use indices.
// max_blocks bounds admitted entries, including pending entries and late splits.
// Exhausted budgets end at actual instruction/edge frontiers; outside targets
// exit. A target in an instruction's middle reports OverlappingTarget instead
// of decoding an overlapping stream. Unknown ISA forms remain real records for
// the emitter to reject; no supported-opcode census is performed here.
// CPUID is a serializing unit boundary: include that record, then stop without
// queuing its sequential successor. Other paths can admit their own entries.
// Failure retains diagnostic decode work but has no usable block set or IR.
// The decoder can report a truncated byte sequence as InvalidEncoding.
// Allocation failures propagate. All traversal is iterative.
[[nodiscard]] DecodedFunction DecodeFunction(std::span<const BlockWindow> readable,
                                              std::uint64_t entry_pc,
                                              DecodeLimits limits = {},
                                              DecodeTiming* timing = nullptr);

}  // namespace swift::x86
