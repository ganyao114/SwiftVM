#include "detail.h"

#include <algorithm>

namespace swift::runtime::ir::passes {
namespace {
constexpr std::size_t local_cells = 16;

bool Numeric32Consumer(const Operation& op) {
    if (op.type != Type::U32) return false;
    // Two physical words mean two atoms, without a compound RHS or a wide
    // literal. Every Value occurrence here is a numeric 32-bit input.
    switch (op.opcode) {
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::And:
        case OpCode::Or:
        case OpCode::Xor:
        case OpCode::AndNot:
        case OpCode::Mul:
            return op.operand_count == 2;
        case OpCode::Neg:
            return op.operand_count == 1;
        case OpCode::CondSelect:
            return op.operand_count == 3;
        default:
            return false;
    }
}

// One actual copy edge. Normally the source carrier must already have the
// right bits. A sole numeric U32 consumer can instead perform its own U32
// read of a wider parent; raw consumers and wider uses retain the normalizer.
// There is no chain resolution or use scan.
bool ForwardCopy(const Builder& editor,
                 std::span<const Operation> operations,
                 Operand& input,
                 CellRange consumer_inputs,
                 NodeId consumer_id,
                 const Operation& consumer,
                 const std::vector<u64>& rows) {
    if (input.kind != OperandKind::Value || input.payload >= operations.size()) return false;
    const auto unit = input.unit;
    const auto& source = operations[static_cast<std::size_t>(input.payload)];
    const auto input_type = input.type == Type::Void ? source.type : input.type;
    if (source.opcode == OpCode::BitExtract) {
        const auto meta = consumer.Metadata();
        if (source.type != Type::U8 || Metadata(source) || source.operand_count != 3 ||
            consumer.opcode != OpCode::And || consumer.type != Type::U8 ||
            consumer.operand_count != 2 || source.block != consumer.block ||
            consumer_id.value - input.payload != 1 || input_type != Type::U8 ||
            (rows[input.payload] & count_mask) != 1 || (rows[input.payload] & pin_bit) ||
            meta.profile != FlagProfile::LogicalZeroCVA || meta.branch_only ||
            (meta.flag_outputs != FlagBits(Flag::Zero) &&
             meta.flag_outputs != FlagBits(Flag::Carry | Flag::Zero)))
            return false;
        auto outer_at = consumer_inputs.begin();
        ++outer_at;
        const auto mask = *outer_at;
        if (mask.Kind() != OperandKind::Immediate || mask.Payload() != 1 ||
            !Scalar(mask.TypeView()))
            return false;
        const auto inputs = editor.Operands(NodeId{static_cast<std::uint32_t>(input.payload)});
        auto at = inputs.begin();
        const auto parent = *at++, first = *at++, length = *at;
        if (parent.Kind() != OperandKind::Value || first.Kind() != OperandKind::Immediate ||
            first.Payload() || length.Kind() != OperandKind::Immediate || length.Payload() != 8)
            return false;
        const auto replacement = Operand::ValueOf(
                {unit, NodeId{static_cast<std::uint32_t>(parent.Payload())}}, parent.TypeView());
        if (!Scalar(ReadType(replacement, operations))) return false;
        // The extract reads raw low8, regardless of its parent's old view.
        // AND #1 can read those same bits through an explicit U8 edge.
        input = Operand::ValueOf({unit, NodeId{static_cast<std::uint32_t>(parent.Payload())}},
                                 Type::U8);
        return true;
    }
    const bool raw_copy = source.opcode == OpCode::BitCast;
    const bool normalize = source.opcode == OpCode::ZeroExtend32 ||
                           source.opcode == OpCode::ZeroExtend32To64 ||
                           source.opcode == OpCode::ZeroExtend64;
    if ((!raw_copy && !normalize) || Metadata(source) ||
        (!Scalar(source.type) && source.type != Type::V128) || input_type != source.type)
        return false;
    const auto cells = editor.Operands(NodeId{static_cast<std::uint32_t>(input.payload)});
    auto at = cells.begin();
    if (at == cells.end()) return false;
    const auto original = *at++;
    if (at != cells.end() || original.Kind() != OperandKind::Value) return false;
    const auto original_id = static_cast<std::uint32_t>(original.Payload());
    const auto& producer = operations[original_id];
    const auto original_view = original.TypeView();
    const auto view = original_view == Type::Void ? producer.type : original_view;
    if (raw_copy) {
        if (view != source.type) return false;
    } else {
        if (!Scalar(view)) return false;
        const auto bits =
                std::min(TypeBits(view), source.opcode == OpCode::ZeroExtend64 ? 64u : 32u);
        const bool raw = producer.opcode == OpCode::BitCast ||
                         producer.opcode == OpCode::GetResult ||
                         producer.opcode == OpCode::GetFlags;
        if (bits != 64 && (!Scalar(producer.type) || TypeBits(producer.type) > bits || raw)) {
            if (source.opcode != OpCode::ZeroExtend32 || source.type != Type::U32 ||
                view != Type::U32 || !Scalar(producer.type) || TypeBits(producer.type) != 64 ||
                raw || (rows[static_cast<std::size_t>(input.payload)] & count_mask) != 1 ||
                !Numeric32Consumer(consumer))
                return false;
        }
    }
    input = Operand::ValueOf({unit, NodeId{original_id}}, source.type);
    return true;
}

struct UniformEntry {
    std::uint32_t offset{}, bytes{};
    Type type{Type::Void};
    Operand value;
};

class UniformCache {
public:
    void Clear() {
        valid = 0;
        next = 0;
    }

    const Operand* Find(std::uint32_t offset, std::uint32_t bytes, Type type) const {
        const Operand* wide{};
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            if (!(valid & (1u << i)) || entry.offset != offset) continue;
            if (entry.bytes == bytes && entry.type == type) return &entry.value;
            // Architectural U32 writes use a complete U64 slot. A subsequent
            // U32 read still gets a real ZeroExtend32, including raw sources.
            if (type == Type::U32 && bytes == 4 && entry.type == Type::U64 && entry.bytes == 8)
                wide = &entry.value;
        }
        return wide;
    }

    void Invalidate(std::uint32_t offset, std::uint32_t bytes) {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            if ((valid & (1u << i)) && u64{entry.offset} < u64{offset} + bytes &&
                u64{offset} < u64{entry.offset} + entry.bytes)
                valid &= ~(1u << i);
        }
    }

    void Add(std::uint32_t offset, std::uint32_t bytes, Type type, Operand value) {
        auto slot = entries.size();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            if (!(valid & (1u << i)) || u64{entry.offset} >= u64{offset} + bytes ||
                u64{offset} >= u64{entry.offset} + entry.bytes)
                continue;
            if (entry.offset == offset && entry.bytes == bytes && entry.type == type) slot = i;
            valid &= ~(1u << i);
        }
        // Updating PC or the same GPR occupies its existing slot. Repeated
        // writes must not rotate through the table and evict unrelated values.
        if (slot == entries.size()) {
            slot = next;
            next = (next + 1) % entries.size();
        }
        entries[slot] = {offset, bytes, type, value};
        valid |= 1u << slot;
    }

private:
    std::array<UniformEntry, 8> entries{};
    std::size_t next{};
    unsigned valid{};
};

std::uint32_t CachedBytes(Type type) {
    return type == Type::U32 || type == Type::U64 || type == Type::V128 ? TypeBits(type) / 8 : 0;
}

}  // namespace

Error RewriteValues(Context& context) {
    auto& editor = context.editor;
    auto& rows = context.rows;
    auto& stats = context.stats;
    const auto& policy = context.policy;
    const auto signed_mul_widen_low = context.signed_mul_widen_low;
    auto& needs_final_cleanup = context.needs_final_cleanup;
    const auto operations = editor.Operations();
    const auto unit = editor.UnitId();
    // 2. Each live operation is considered once. Read current constants,
    // one copy edge or a neighboring extract, without a value cache.
    UniformCache uniforms;
    BlockId block{};
    // Only algebra/folding and Uniform opcode changes need a bounded external
    // draft. Collect its post-copy inputs and uses while editing copy
    // edges in the actual word store, without decoding the range again.
    std::array<Operand, local_cells> cells;
    std::array<std::uint32_t, local_cells> old_uses;
    for (std::size_t at = 0; at < operations.size(); ++at) {
        const auto id = NodeId{static_cast<std::uint32_t>(at)};
        const auto op = operations[at];
        if (op.block != block) {
            uniforms.Clear();
            block = op.block;
        }
        const auto meta = op.Metadata();
        const auto effects = EffectsFor(op.opcode, meta);
        // A read, even one that may fault or has acquire ordering, does
        // not mutate cached Uniform values. Stores remain in place and
        // observable; this forwards reads only on the successful path.
        const bool independent_memory =
                HasAnyEffect(effects, Effect::ReadMemory | Effect::WriteMemory) &&
                (policy.guest_memory_disjoint || !HasAnyEffect(effects, Effect::WriteMemory)) &&
                !HasAnyEffect(effects, Effect::WriteGuestState | Effect::WriteFlags);
        if (HasAnyEffect(effects, Effect::Unknown | Effect::External | Effect::Control) ||
            (!policy.guest_memory_disjoint && HasAnyEffect(effects, Effect::WriteMemory)) ||
            (!independent_memory && HasAnyEffect(effects, Effect::MayFault | Effect::Ordered)) ||
            (HasAnyEffect(effects, Effect::WriteGuestState) && op.opcode != OpCode::StoreUniform))
            uniforms.Clear();
        else if (HasAnyEffect(effects, Effect::WriteFlags)) {
            if (policy.flags)
                uniforms.Invalidate(policy.flags->offset, policy.flags->bytes);
            else
                uniforms.Clear();
        }
        if (HasAnyEffect(effects, Effect::WriteHostState)) {
            if (policy.numeric_control)
                uniforms.Invalidate(policy.numeric_control->offset, policy.numeric_control->bytes);
            else
                uniforms.Clear();
        }
        if (!(rows[at] & live_bit)) continue;
        // These records have no rewritable value inputs or fold to do.
        // Effects above still apply; no decoded Operand draft is needed.
        if (op.opcode == OpCode::LoadImm || op.opcode == OpCode::FaultLocation ||
            op.opcode == OpCode::Nop)
            continue;
        const bool has_metadata = meta != OperationMetadata{};
        // Flags metadata consumes the same logical input bits after a
        // strict copy substitution. Keep the producer and its metadata;
        // only metadata-free nodes may change opcode or fold a result.
        const bool editable = !Sensitive(op.opcode, effects) && !(rows[at] & pin_bit);
        if (!editable && op.opcode != OpCode::LoadUniform && op.opcode != OpCode::StoreUniform)
            continue;
        bool folding = editable && !has_metadata && Foldable(op);
        const bool vector_draft = editable && !has_metadata && signed_mul_widen_low &&
                                  op.opcode == OpCode::VecMadd16 && op.type == Type::V128;
        const bool address_draft = editable && !has_metadata &&
                                   op.opcode == OpCode::LoadMemoryTSO && op.operand_count == 1 &&
                                   (op.type == Type::U8 || op.type == Type::U16 ||
                                    op.type == Type::U32 || op.type == Type::U64);
        const bool uniform_op =
                op.opcode == OpCode::LoadUniform || op.opcode == OpCode::StoreUniform;
        const bool draft = folding || uniform_op || vector_draft || address_draft;
        std::size_t count{}, old_use_count{};
        bool complete = true;
        const auto inputs = editor.Operands(id);
        for (auto edge = inputs.begin(); edge != inputs.end(); ++edge) {
            const auto cell = *edge;
            const auto kind = cell.Kind();
            if (!draft && kind != OperandKind::Value) continue;
            auto payload = cell.Payload();
            auto view = cell.TypeView();
            if (editable && kind == OperandKind::Value) {
                const auto old_id = static_cast<std::uint32_t>(payload);
                auto replacement = Operand::ValueOf({unit, NodeId{old_id}}, view);
                if (ForwardCopy(editor, operations, replacement, inputs, id, op, rows)) {
                    const auto new_id = static_cast<std::uint32_t>(replacement.payload);
                    if (!(rows[old_id] & count_mask)) return Error::InvalidValue;
                    if ((rows[new_id] & count_mask) == count_mask) return Error::CapacityOverflow;
                    if (const auto error = editor.ReplaceValueUse(
                                id, edge, {unit, NodeId{new_id}}, replacement.type);
                        error != Error::None)
                        return error;
                    needs_final_cleanup = true;  // Includes a changed effective view.
                    --rows[old_id];
                    rows[new_id] = (rows[new_id] | live_bit) + 1;
                    ++stats.copy_forwarded;
                    payload = replacement.payload;
                    view = replacement.type;
                }
            }
            if (!draft) continue;
            // Finish every copy even when the bounded draft cannot hold
            // this range; never structurally rewrite a truncated draft.
            if (count == cells.size()) {
                complete = false;
                continue;
            }
            if (kind == OperandKind::Value)
                old_uses[old_use_count++] = static_cast<std::uint32_t>(payload);
            cells[count++] = {payload,
                              kind == OperandKind::Value ? unit : 0,
                              cell.Count(),
                              cell.Code(),
                              kind,
                              view};
        }
        if (!draft || !complete) continue;
        const auto uniform = count && cells[0].kind == OperandKind::Uniform ? cells[0] : Operand{};
        bool uniform_forward{};
        auto opcode = op.opcode;
        auto new_count = count;
        if (editable) {
            if (!has_metadata && op.opcode == OpCode::LoadUniform && count == 1 &&
                uniform.kind == OperandKind::Uniform && uniform.type == op.type &&
                CachedBytes(op.type)) {
                if (const auto* previous =
                            uniforms.Find(static_cast<std::uint32_t>(uniform.payload),
                                          CachedBytes(op.type),
                                          op.type)) {
                    cells[0] = *previous;
                    opcode = op.type == Type::V128  ? OpCode::BitCast
                             : op.type == Type::U32 ? OpCode::ZeroExtend32
                                                    : OpCode::ZeroExtend64;
                    uniform_forward = true;
                    folding = Scalar(op.type);
                }
            }
            const bool combined =
                    (folding &&
                     ((opcode == OpCode::BitInsert &&
                       FoldLowByteMask(editor, operations, id, cells, new_count, opcode)) ||
                      ((opcode == OpCode::BitExtract || opcode == OpCode::LsrImm) &&
                       FoldExtractShift(
                               editor, operations, id, rows, cells, new_count, opcode)))) ||
                    (vector_draft &&
                     FuseSignedMulWidenLow(
                             editor, operations, id, rows, cells, new_count, opcode)) ||
                    (address_draft &&
                     FoldOrderedLoadAddress(editor, operations, id, rows, cells, new_count));
            u64 bits{};
            const auto candidate =
                    Operation{op.flag_outputs, 0, 0, op.block, opcode, op.type, op.properties};
            const bool folded =
                    folding && Fold(editor, operations, candidate, {cells.data(), new_count}, bits);
            if (folded) {
                opcode = OpCode::LoadImm;
                cells[0] = Operand::Immediate(bits, op.type);
                new_count = 1;
            }
            if (uniform_forward || folded || combined) {
                const auto replacement = std::span<const Operand>{cells.data(), new_count};
                if (const auto error = editor.Rewrite(id, opcode, op.type, replacement, meta);
                    error != Error::None)
                    return error;
                needs_final_cleanup = true;
                for (std::size_t i = 0; i < old_use_count; ++i)
                    if (!DropUse(rows, old_uses[i])) return Error::InvalidValue;
                for (const auto& cell : replacement)
                    if (cell.kind == OperandKind::Value &&
                        !AddUse(rows, static_cast<std::uint32_t>(cell.payload)))
                        return Error::CapacityOverflow;
                stats.uniform_forwarded += uniform_forward;
                // Includes constant folds and bounded scalar/vector/address algebra.
                stats.folded += folded || combined;
            }
        }
        if (op.opcode == OpCode::StoreUniform && count == 2 &&
            uniform.kind == OperandKind::Uniform) {
            const auto bytes = TypeBits(uniform.type) / 8;
            if (!bytes) {
                uniforms.Clear();
                continue;
            }
            const auto view = ReadType(cells[1], operations);
            if (cells[1].kind == OperandKind::Value && CachedBytes(uniform.type) &&
                ((Scalar(uniform.type) && Scalar(view) &&
                  TypeBits(view) <= TypeBits(uniform.type)) ||
                 (uniform.type == Type::V128 && view == Type::V128)))
                uniforms.Add(
                        static_cast<std::uint32_t>(uniform.payload), bytes, uniform.type, cells[1]);
            else
                uniforms.Invalidate(static_cast<std::uint32_t>(uniform.payload), bytes);
        } else if (op.opcode == OpCode::LoadUniform && count == 1 &&
                   uniform.kind == OperandKind::Uniform && uniform.type == op.type &&
                   CachedBytes(op.type)) {
            // A forwarded read still denotes the cached logical value.
            // Keep its actual source/view, rather than build a chain of
            // normalization wrappers for successive reads of one slot.
            uniforms.Add(static_cast<std::uint32_t>(uniform.payload),
                         CachedBytes(op.type),
                         op.type,
                         uniform_forward && opcode != OpCode::LoadImm
                                 ? cells[0]
                                 : Operand::ValueOf({unit, id}, op.type));
        }
    }
    return Error::None;
}

}  // namespace swift::runtime::ir::passes
