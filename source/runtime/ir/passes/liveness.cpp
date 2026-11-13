#include "detail.h"
#include "runtime/ir/flag_effects.h"

namespace swift::runtime::ir::passes {
namespace {
constexpr auto flag_observers =
        Effect::MayFault | Effect::Unknown | Effect::External | Effect::Control;
constexpr auto uniform_access = Effect::ReadGuestState | Effect::WriteGuestState;
constexpr auto flag_inputs = flag_observers | uniform_access | Effect::ReadFlags;
constexpr std::array<u64, 6> flag_bits{1, 2, 4, 8, 16, 64};
enum class FlagWriteKind { None, Metadata, Mask, Fixed, Opaque };

struct FlagWrite {
    u64 mask{};
    FlagWriteKind kind{FlagWriteKind::None};
    unsigned argument{};
};

// These are definite architectural writes, independent of a physical flags
// representation. Unrecognized metadata/writers stay observable barriers.
FlagWrite FlagWrites(const Builder& editor,
                     NodeId id,
                     const Operation& op,
                     OperationMetadata meta) {
    if (meta != OperationMetadata{}) {
        const bool integer = op.type == Type::U8 || op.type == Type::U16 || op.type == Type::U32 ||
                             op.type == Type::U64;
        bool known{};
        if (meta.profile == FlagProfile::ArithmeticRaw ||
            meta.profile == FlagProfile::ArithmeticCanonicalCarry)
            known = op.opcode == OpCode::Add || op.opcode == OpCode::Sub ||
                    op.opcode == OpCode::Neg || op.opcode == OpCode::Adc ||
                    op.opcode == OpCode::Sbb;
        else if (meta.profile == FlagProfile::LogicalZeroCVA)
            known = op.opcode == OpCode::And || op.opcode == OpCode::Or || op.opcode == OpCode::Xor;
        if (integer && known && meta.flag_outputs && !(meta.flag_outputs & ~all_flags) &&
            !meta.branch_only)
            return {meta.flag_outputs, FlagWriteKind::Metadata};
        return {0, FlagWriteKind::Opaque};
    }
    switch (op.opcode) {
        case OpCode::SetCarry:
        case OpCode::InvertCarry:
            return {FlagBits(Flag::Carry), FlagWriteKind::Fixed};
        case OpCode::SetOverflow:
            return {FlagBits(Flag::Overflow), FlagWriteKind::Fixed};
        case OpCode::PublishFCmpFlags:
            return {all_flags, FlagWriteKind::Fixed};
        case OpCode::ClearFlags:
        case OpCode::SaveFlags:
        case OpCode::RestoreFlags: {
            const unsigned argument = op.opcode == OpCode::ClearFlags ? 0 : 1;
            const auto inputs = editor.Operands(id);
            auto at = inputs.begin();
            for (unsigned i = 0; i < argument && at != inputs.end(); ++i) ++at;
            if (at != inputs.end() && (*at).Kind() == OperandKind::Flags &&
                !((*at).Payload() & ~all_flags))
                return {(*at).Payload(), FlagWriteKind::Mask, argument};
            return {0, FlagWriteKind::Opaque};
        }
        default:
            return {0, FlagWriteKind::Opaque};
    }
}

bool Observable(Effect effects, FlagWrite write) {
    constexpr auto reads =
            static_cast<unsigned>(Effect::ReadGuestState | Effect::ReadLocal |
                                  Effect::ReadHostState | Effect::ReadFlags | Effect::WriteFlags);
    return write.kind == FlagWriteKind::Opaque || (static_cast<unsigned>(effects) & ~reads);
}

u64 KeptFlags(FlagWrite write, u64 needed) {
    const auto keep = write.mask & needed;
    // Fixed multi-bit publications cannot express a partial write. Retain
    // the original operation if any output is needed; never synthesize one.
    return write.kind == FlagWriteKind::Fixed && keep ? write.mask : keep;
}

Error TrimFlags(Builder& editor,
                NodeId id,
                const Operation& op,
                FlagWrite write,
                u64 keep,
                bool discard_omitted = false) {
    if (write.kind == FlagWriteKind::Metadata) {
        auto meta = op.Metadata();
        meta.flag_outputs = keep;
        if (!keep) {
            meta.profile = FlagProfile::None;
            meta.discard_omitted_flags = false;
        } else if (discard_omitted)
            meta.discard_omitted_flags = true;
        return editor.SetMetadata(id, meta);
    }
    if (!keep) return editor.Rewrite(id, OpCode::Nop, Type::Void, {});
    std::array<Operand, 2> inputs;
    unsigned count{};
    for (const auto cell : editor.Operands(id)) {
        inputs[count++] = {cell.Payload(),
                           cell.Kind() == OperandKind::Value ? editor.UnitId() : 0,
                           cell.Count(),
                           cell.Code(),
                           cell.Kind(),
                           cell.TypeView()};
    }
    inputs[write.argument] = Operand::Flags(keep);
    return editor.Rewrite(id, op.opcode, op.type, {inputs.data(), count});
}
}  // namespace

void FlagGraph::Prepare(std::span<const BlockRecord> blocks,
                        bool function_scope,
                        OptimizeStats& stats) {
    // A single external exit has the same All-flags boundary as a Block.
    // Keep its Function scope; only the unused CFG summaries are omitted.
    function = function_scope &&
               (blocks.size() != 1 || blocks.front().terminal.kind == TerminalKind::Jump ||
                blocks.front().terminal.kind == TerminalKind::Branch);
    if (function) {
        // A terminal has at most two edges, so eight blocks also bound
        // the actual predecessor rows to sixteen. These are the same
        // four CFG buffers, with local storage for small Functions.
        const bool small = blocks.size() <= small_flags.size();
        if (small) {
            flags = std::span{small_flags}.first(blocks.size());
            predecessor_offsets = std::span{small_offsets}.first(blocks.size() + 1);
            pending = std::span{small_pending}.first(blocks.size());
        } else {
            flag_storage.resize(blocks.size());
            offset_storage.resize(blocks.size() + 1);
            pending_storage.resize(blocks.size());
            flags = flag_storage;
            predecessor_offsets = offset_storage;
            pending = pending_storage;
        }
        for (const auto& block : blocks) {
            const auto& terminal = block.terminal;
            if (terminal.kind == TerminalKind::Jump || terminal.kind == TerminalKind::Branch)
                ++predecessor_offsets[terminal.first.value + std::size_t{1}];
            if (terminal.kind == TerminalKind::Branch)
                ++predecessor_offsets[terminal.second.value + std::size_t{1}];
        }
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            predecessor_offsets[b + 1] += predecessor_offsets[b];
            pending[b] = predecessor_offsets[b];
        }
        if (small)
            predecessors = std::span{small_predecessors}.first(predecessor_offsets.back());
        else {
            predecessor_storage.resize(predecessor_offsets.back());
            predecessors = predecessor_storage;
        }
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            const auto& terminal = blocks[b].terminal;
            if (terminal.kind == TerminalKind::Jump || terminal.kind == TerminalKind::Branch)
                predecessors[pending[terminal.first.value]++] =
                        BlockId{static_cast<std::uint32_t>(b)};
            if (terminal.kind == TerminalKind::Branch)
                predecessors[pending[terminal.second.value]++] =
                        BlockId{static_cast<std::uint32_t>(b)};
        }
        // The fill cursors become the bounded ring queue; no second
        // block directory or per-node flags storage is constructed.
        stats.block_flags_bytes =
                small ? sizeof(small_flags) + sizeof(small_offsets) + sizeof(small_pending) +
                                sizeof(small_predecessors)
                      : flag_storage.capacity() * sizeof(FlagBlock) +
                                (offset_storage.capacity() + pending_storage.capacity()) *
                                        sizeof(std::size_t) +
                                predecessor_storage.capacity() * sizeof(BlockId);
    }
}

Error AnalyzeLiveness(Context& context, FlagGraph& graph) {
    auto& editor = context.editor;
    auto& rows = context.rows;
    auto& stats = context.stats;
    const auto& policy = context.policy;
    auto& needs_final_cleanup = context.needs_final_cleanup;
    const auto operations = editor.Operations();
    const auto blocks = editor.Blocks();
    const bool function = graph.function;
    auto flags = graph.flags;
    auto pending = graph.pending;
    const auto predecessor_offsets = graph.predecessor_offsets;
    const auto predecessors = graph.predecessors;
    for (const auto& block : blocks)
        if (block.terminal.HasValue()) {
            if (!AddUse(rows, block.terminal.value.value)) return Error::CapacityOverflow;
            ++stats.value_uses;
        }
    // 1. Flags start live at every block exit. Within a block only live
    // observers demand them; covered outputs cease to root their input
    // graph. Only live inputs acquire uses. IDs predate their uses.
    u64 needed_flags = all_flags;
    BlockId flags_block{};
    FlagBlock* summary{};
    for (std::size_t at = operations.size(); at--;) {
        const auto id = NodeId{static_cast<std::uint32_t>(at)};
        const auto& op = operations[at];
        if (op.block != flags_block) {
            needed_flags = all_flags;
            flags_block = op.block;
            summary = function ? &flags[op.block.value] : nullptr;
        }
        if (op.opcode == OpCode::Nop && !op.flag_outputs && !op.properties) continue;
        const auto meta = op.Metadata();
        // The final walk skips every Nop, while this walk conservatively
        // observes non-default Nop metadata. Keep that existing path intact.
        if (op.opcode == OpCode::Nop) needs_final_cleanup = true;
        if (op.opcode == OpCode::FaultLocation && meta == OperationMetadata{}) {
            rows[at] |= live_bit;  // An immutable location has no value or flag inputs.
            continue;
        }
        const auto effects = EffectsFor(op.opcode, meta);
        const auto write = meta != OperationMetadata{} || HasAnyEffect(effects, Effect::WriteFlags)
                                   ? FlagWrites(editor, id, op, meta)
                                   : FlagWrite{};
        const auto keep = KeptFlags(write, needed_flags);
        const bool dead_setter =
                (write.kind == FlagWriteKind::Mask || write.kind == FlagWriteKind::Fixed) && !keep;
        if (keep != write.mask || dead_setter) {
            needs_final_cleanup = true;
            if (const auto error = TrimFlags(editor, id, op, write, keep); error != Error::None)
                return error;
        }
        if (dead_setter) {
            ++stats.erased;
            continue;
        }
        if (Observable(effects, write) || keep) rows[at] |= live_bit;
        if (!(rows[at] & live_bit)) {
            needs_final_cleanup = true;
            continue;
        }
        if (write.kind == FlagWriteKind::Metadata && keep && keep != all_flags &&
            !meta.branch_only && !(needed_flags & ~keep) && !meta.discard_omitted_flags)
            needs_final_cleanup = true;
        const auto inputs = editor.Operands(id);
        // GetFlags reads its whole carrier. Fault/helper observations
        // remain All even with private state; only actual Uniform access
        // needs the first operand, borrowed from this same use walk.
        const auto reads = write.kind == FlagWriteKind::Opaque ? all_flags
                           : HasAnyEffect(effects, flag_inputs)
                                   ? FlagReads(op.opcode, effects, inputs, policy)
                                   : 0;
        if (summary && (keep || reads)) {
            const auto exit_only = keep & ~(u64{summary->gen} | summary->kill);
            for (std::size_t bit = 0; exit_only && bit < flag_bits.size(); ++bit)
                if (exit_only & flag_bits[bit]) summary->exit_writers[bit] = id;
            summary->gen = static_cast<std::uint8_t>((summary->gen & ~keep) | reads);
            summary->kill |= static_cast<std::uint8_t>(keep);
        }
        needed_flags = (needed_flags & ~keep) | reads;
        const bool pin = Sensitive(op.opcode, effects);
        for (const auto cell : inputs)
            if (cell.Kind() == OperandKind::Value) {
                if (!AddUse(rows, static_cast<std::uint32_t>(cell.Payload()), pin))
                    return Error::CapacityOverflow;
                ++stats.value_uses;
            }
    }

    if (function) {
        // Summaries describe the conservative first live graph. Each
        // input bit only falls from All, so predecessors are revisited
        // at most six times through actual terminal edges, without N.
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            pending[b] = b;
            flags[b].queued = true;
        }
        std::size_t head{}, tail{}, count = blocks.size();
        while (count) {
            const auto b = pending[head];
            if (++head == pending.size()) head = 0;
            --count;
            auto& info = flags[b];
            info.queued = false;
            const auto& terminal = blocks[b].terminal;
            u64 output = all_flags;
            if (terminal.kind == TerminalKind::Jump)
                output = flags[terminal.first.value].input;
            else if (terminal.kind == TerminalKind::Branch)
                output = flags[terminal.first.value].input | flags[terminal.second.value].input;
            needs_final_cleanup |= output != all_flags;
            info.output = static_cast<std::uint8_t>(output);
            const auto input = static_cast<std::uint8_t>(info.gen | (output & ~info.kill));
            if (input == info.input) continue;
            info.input = input;
            for (auto edge = predecessor_offsets[b]; edge < predecessor_offsets[b + 1]; ++edge) {
                const auto predecessor = predecessors[edge].value;
                if (flags[predecessor].queued) continue;
                flags[predecessor].queued = true;
                pending[tail] = predecessor;
                if (++tail == pending.size()) tail = 0;
                ++count;
            }
        }
        // Correct only the at-most-six recorded exit writers per B.
        // Several bits can name one operation; group it before editing.
        for (auto& info : flags) {
            for (std::size_t bit = 0; bit < flag_bits.size(); ++bit) {
                const auto id = info.exit_writers[bit];
                if (!id) continue;
                auto exit_only = flag_bits[bit];
                for (auto other = bit + 1; other < flag_bits.size(); ++other)
                    if (info.exit_writers[other] == id) {
                        exit_only |= flag_bits[other];
                        info.exit_writers[other] = {};
                    }
                const auto& op = operations[id.value];
                const auto meta = op.Metadata();
                const auto effects = EffectsFor(op.opcode, meta);
                const auto write = FlagWrites(editor, id, op, meta);
                const auto keep = KeptFlags(write, (write.mask & ~exit_only) | info.output);
                if (keep == write.mask) continue;
                needs_final_cleanup = true;
                const bool dead_setter = write.kind != FlagWriteKind::Metadata && !keep;
                const bool dead_value =
                        !keep && !(rows[id.value] & count_mask) && !Observable(effects, write);
                if ((dead_setter || dead_value) && (rows[id.value] & live_bit))
                    for (const auto cell : editor.Operands(id))
                        if (cell.Kind() == OperandKind::Value &&
                            !DropUse(rows, static_cast<std::uint32_t>(cell.Payload())))
                            return Error::InvalidValue;
                if (const auto error = TrimFlags(editor, id, op, write, keep); error != Error::None)
                    return error;
                if (dead_value && !dead_setter)
                    if (const auto error = editor.EraseUnused(id); error != Error::None)
                        return error;
                if (dead_setter || dead_value) {
                    rows[id.value] = 0;
                    ++stats.erased;
                }
            }
        }
    }
    return Error::None;
}

Error EliminateDeadCode(Context& context, const FlagGraph& graph) {
    auto& editor = context.editor;
    auto& rows = context.rows;
    auto& stats = context.stats;
    const auto& policy = context.policy;
    const auto operations = editor.Operations();
    const bool function = graph.function;
    const auto flags = graph.flags;
    // 3. Recompute flags demand from the current graph, coupled to DCE.
    // A now-dead flag reader must not resurrect older flag writers.
    // Deleting a formerly live node releases its current inputs, so both
    // data and flag-only chains close in this same reverse walk.
    // Function outputs stay frozen. A reader killed here can leave
    // excess demand in a predecessor; no partially updated global
    // solution is consumed while nodes and uses are being removed.
    BlockId flags_block{};
    u64 needed_flags = all_flags;
    for (std::size_t at = operations.size(); at--;) {
        const auto id = NodeId{static_cast<std::uint32_t>(at)};
        const auto& op = operations[at];
        if (op.block != flags_block) {
            needed_flags = function ? flags[op.block.value].output : all_flags;
            flags_block = op.block;
        }
        if (op.opcode == OpCode::Nop) continue;
        const auto meta = op.Metadata();
        if (op.opcode == OpCode::FaultLocation && meta == OperationMetadata{}) continue;
        const auto effects = EffectsFor(op.opcode, meta);
        const auto write = meta != OperationMetadata{} || HasAnyEffect(effects, Effect::WriteFlags)
                                   ? FlagWrites(editor, id, op, meta)
                                   : FlagWrite{};
        const auto keep = KeptFlags(write, needed_flags);
        const bool dead_setter =
                (write.kind == FlagWriteKind::Mask || write.kind == FlagWriteKind::Fixed) && !keep;
        const bool live = (rows[at] & count_mask) || Observable(effects, write) || keep;
        // This changes the IR's write-after state contract. Grant it only
        // from final demand, before propagating this operation's reads.
        // An inherited permission remains literal semantics after Edit;
        // keeping its other dependencies conservative does not revoke it.
        const bool discard_omitted = live && write.kind == FlagWriteKind::Metadata && keep &&
                                     keep != all_flags && !meta.branch_only &&
                                     !(needed_flags & ~keep);
        // Release operands before a dead setter's Rewrite can resize the
        // arena. Metadata/mask trimming keeps every live value edge.
        if ((!live || dead_setter) && (rows[at] & live_bit))
            for (const auto cell : editor.Operands(id))
                if (cell.Kind() == OperandKind::Value &&
                    !DropUse(rows, static_cast<std::uint32_t>(cell.Payload())))
                    return Error::InvalidValue;
        if (keep != write.mask || dead_setter || (discard_omitted && !meta.discard_omitted_flags))
            if (const auto error = TrimFlags(editor, id, op, write, keep, discard_omitted);
                error != Error::None)
                return error;
        if (dead_setter) {
            ++stats.erased;
            continue;
        }
        if (live) {
            const auto reads = write.kind == FlagWriteKind::Opaque ? all_flags
                               : HasAnyEffect(effects, flag_inputs)
                                       ? FlagReads(op.opcode, effects, editor.Operands(id), policy)
                                       : 0;
            needed_flags = (needed_flags & ~keep) | reads;
            continue;
        }
        if (const auto error = editor.EraseUnused(id); error != Error::None) return error;
        ++stats.erased;
    }
    return Error::None;
}

}  // namespace swift::runtime::ir::passes
