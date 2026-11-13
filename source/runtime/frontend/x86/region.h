#pragma once

#include "decode.h"
#include "runtime/code_source.h"

namespace swift::x86 {

using runtime::frontend::BlockWindow;

struct DecodedBlock {
    std::uint64_t pc{};
    std::uint32_t first_instruction{}, instruction_count{};
};

static_assert(sizeof(DecodedBlock) == 16);

using runtime::frontend::DecodeLimits;

// A cold, read-only borrow of actual callable entries in the current execution
// domain. Pending keys are not ready. No directory or code owner is copied.
using runtime::frontend::ReadyEntryLookup;

struct DecodedFunction {
    DecodeError error{DecodeError::None};
    std::uint64_t error_pc{};
    std::vector<Instruction> instructions;
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
// Ready entries are real exit frontiers; the request's own entry is always
// decoded. CALL remains a region exit: its continuation is formed when first
// requested, rather than expanding the speculative graph for entry reuse.
// Without a ready lookup, discovery retains the original bounded region.
[[nodiscard]] DecodedFunction DecodeFunction(
        std::span<const BlockWindow> readable,
        std::uint64_t entry_pc,
        DecodeLimits limits = {},
        DecodeTiming* timing = nullptr,
        ReadyEntryLookup ready = {},
        runtime::x86::GuestCpuModel model = runtime::x86::BaselineGuestCpu,
        std::vector<runtime::SourceSpan>* source_spans = nullptr,
        std::vector<std::uint64_t>* continuations = nullptr,
        DecodedFunction* reusable_decoded = nullptr);

}  // namespace swift::x86
