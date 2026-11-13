#include "detail.h"
#include "runtime/ir/uniform_effects.h"

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
    NodeId value{}, store{};
    std::uint16_t valid_epoch{}, pending_epoch{};
    Type type{Type::Void}, value_type{Type::Void};
};

static_assert(sizeof(UniformEntry) == 24);

class UniformCache {
public:
    void Clear() {
        Advance(valid_epoch);
        Advance(scalar_pending_epoch);
        Advance(vector_pending_epoch);
    }

    struct UniformMatch {
        Operand value{};
        int v128_lane{-1};
        bool found{};
    };

    UniformMatch Find(std::uint32_t offset,
                      std::uint32_t bytes,
                      Type type,
                      u64 unit) const {
        const unsigned idx = (offset >> 3) & (capacity - 1);
        if ((occupied[idx >> 6] >> (idx & 63)) & 1u) {
            const auto& entry = entries[idx];
            if (entry.valid_epoch == valid_epoch && entry.offset == offset) {
                if ((entry.bytes == bytes && entry.type == type) ||
                    (type == Type::U32 && bytes == 4 && entry.type == Type::U64 &&
                     entry.bytes == 8))
                    return {Operand::ValueOf({unit, entry.value}, entry.value_type), -1, true};
                if (type == Type::U64 && bytes == 8 && entry.type == Type::V128 &&
                    entry.bytes == 16)
                    return {Operand::ValueOf({unit, entry.value}, entry.value_type), 0, true};
            }
        }
        if (type == Type::U64 && bytes == 8 && offset >= 8) {
            const unsigned low_idx = ((offset - 8) >> 3) & (capacity - 1);
            if ((occupied[low_idx >> 6] >> (low_idx & 63)) & 1u) {
                const auto& low = entries[low_idx];
                if (low.valid_epoch == valid_epoch && low.offset == offset - 8 &&
                    low.type == Type::V128 && low.bytes == 16)
                    return {Operand::ValueOf({unit, low.value}, low.value_type), 1, true};
            }
        }
        return {};
    }

    void Invalidate(std::uint32_t offset, std::uint32_t bytes) {
        Visit({offset, bytes},
              [&](UniformEntry& entry) { entry.valid_epoch = entry.pending_epoch = 0; });
    }

    void ClearPending() {
        Advance(scalar_pending_epoch);
        Advance(vector_pending_epoch);
    }

    void ClearScalarPending() { Advance(scalar_pending_epoch); }

    void Observe(UniformRange range) {
        Visit(range, [](UniformEntry& entry) { entry.pending_epoch = 0; });
    }

    unsigned Cover(UniformRange range, std::array<NodeId, 8>& stores) {
        unsigned count{};
        Visit(range, [&](UniformEntry& entry) {
            if (entry.pending_epoch == PendingEpoch(entry.bytes) && range.offset <= entry.offset &&
                std::uint64_t{range.offset} + range.bytes >=
                        std::uint64_t{entry.offset} + entry.bytes)
                stores[count++] = entry.store;
            entry.pending_epoch = 0;
        });
        return count;
    }

    void Add(std::uint32_t offset,
             std::uint32_t bytes,
             Type type,
             Operand value,
             NodeId store = {},
             bool cacheable = true) {
        const unsigned idx = (offset >> 3) & (capacity - 1);
        const bool was_occupied = ((occupied[idx >> 6] >> (idx & 63)) & 1u) != 0;
        auto& entry = entries[idx];
        const auto target_pending = PendingEpoch(bytes);
        const bool same =
                was_occupied && entry.offset == offset && entry.bytes == bytes && entry.type == type;
        const auto previous =
                same && entry.pending_epoch == target_pending ? entry.store : NodeId{};
        Invalidate(offset, bytes);
        occupied[idx >> 6] |= u64{1} << (idx & 63);
        entry = {offset,
                 bytes,
                 cacheable ? NodeId{static_cast<std::uint32_t>(value.payload)} : NodeId{},
                 store ? store : previous,
                 cacheable ? valid_epoch : std::uint16_t{0},
                 store || previous ? target_pending : std::uint16_t{0},
                 type,
                 cacheable ? value.type : Type::Void};
    }

private:
    static constexpr unsigned capacity = 128;
    std::array<u64, 2> occupied{};
    std::array<UniformEntry, capacity> entries;
    std::uint16_t valid_epoch{1}, scalar_pending_epoch{1}, vector_pending_epoch{1};

    std::uint16_t PendingEpoch(std::uint32_t bytes) const {
        return bytes > 8 ? vector_pending_epoch : scalar_pending_epoch;
    }

    void Advance(std::uint16_t& epoch) {
        if (++epoch) return;
        occupied = {};
        valid_epoch = scalar_pending_epoch = vector_pending_epoch = 1;
    }

    template <class F> void Visit(UniformRange range, F visit) {
        if (!(occupied[0] | occupied[1]) || !range.bytes) return;
        // Cached values and pending stores span at most 32 bytes. Include their possible
        // start before the queried interval and verify the full byte range.
        constexpr auto preceding = TypeBits(Type::V256) / 8 - 1;
        const auto first = (range.offset > preceding ? range.offset - preceding : 0) >> 3;
        const auto last = (std::uint64_t{range.offset} + range.bytes - 1) >> 3;
        const auto count = std::min<std::uint64_t>(capacity, last - first + 1);
        for (unsigned i = 0; i < count; ++i) {
            const unsigned idx = static_cast<unsigned>((first + i) & (capacity - 1));
            if (!((occupied[idx >> 6] >> (idx & 63)) & 1u)) continue;
            auto& entry = entries[idx];
            if ((entry.valid_epoch == valid_epoch ||
                 entry.pending_epoch == PendingEpoch(entry.bytes)) &&
                Overlaps(range, {entry.offset, entry.bytes}))
                visit(entry);
        }
    }
};

std::uint32_t CachedBytes(Type type) {
    return type == Type::U32 || type == Type::U64 || type == Type::V128 ? TypeBits(type) / 8 : 0;
}

struct ReadableSpan {
    NodeId base{};
    std::uint32_t min_offset{}, max_end{};
};

class ReadableSpanCache {
public:
    void Clear() {
        spans = {};
        next_slot = 0;
    }

    bool CheckAndRecord(NodeId base, std::uint32_t offset, std::uint32_t bytes) {
        if (!base || !bytes || offset > 4096u - bytes) return false;
        const std::uint32_t end = offset + bytes;
        for (auto& span : spans) {
            if (span.base == base) {
                if (span.min_offset <= offset && end <= span.max_end) return true;
                span.min_offset = std::min(span.min_offset, offset);
                span.max_end = std::max(span.max_end, end);
                return false;
            }
        }
        spans[next_slot] = {base, offset, end};
        next_slot = (next_slot + 1) & (spans.size() - 1);
        return false;
    }

private:
    std::array<ReadableSpan, 4> spans{};
    unsigned next_slot{};
};

bool DecodeLoadMemorySpan(const Builder& editor,
                          std::span<const Operation> operations,
                          NodeId id,
                          const Operation& op,
                          NodeId& base,
                          std::uint32_t& offset,
                          std::uint32_t& bytes) {
    if (op.opcode != OpCode::LoadMemory || Metadata(op)) return false;
    bytes = (TypeBits(op.type) + 7) / 8;
    if (!bytes || bytes > 4096u) return false;
    const auto unwrap_u64 = [&](std::uint32_t node_id) -> std::uint32_t {
        const auto& candidate = operations[node_id];
        if ((candidate.opcode == OpCode::ZeroExtend64 || candidate.opcode == OpCode::BitCast) &&
            candidate.type == Type::U64 && !Metadata(candidate)) {
            const auto inner = editor.Operands(NodeId{node_id});
            auto it = inner.begin();
            if (it != inner.end()) {
                const auto src = *it++;
                if (it == inner.end() && src.Kind() == OperandKind::Value &&
                    (src.TypeView() == Type::Void || src.TypeView() == Type::U64) &&
                    operations[src.Payload()].type == Type::U64) {
                    return static_cast<std::uint32_t>(src.Payload());
                }
            }
        }
        return node_id;
    };
    const auto inputs = editor.Operands(id);
    auto at = inputs.begin();
    if (at == inputs.end()) return false;
    const auto first = *at++;
    if (first.Kind() == OperandKind::Value && at == inputs.end()) {
        if (first.TypeView() != Type::Void && first.TypeView() != Type::U64) return false;
        const auto addr_id = unwrap_u64(static_cast<std::uint32_t>(first.Payload()));
        const auto& addr_op = operations[addr_id];
        if (addr_op.type != Type::U64) return false;
        if (addr_op.opcode == OpCode::Add && !Metadata(addr_op)) {
            const auto addr_in = editor.Operands(NodeId{addr_id});
            auto a_it = addr_in.begin();
            if (a_it != addr_in.end()) {
                const auto lhs = *a_it++;
                if (a_it != addr_in.end()) {
                    const auto rhs = *a_it++;
                    if (a_it == addr_in.end() && lhs.Kind() == OperandKind::Value &&
                        (lhs.TypeView() == Type::Void || lhs.TypeView() == Type::U64) &&
                        rhs.Kind() == OperandKind::Immediate && Scalar(rhs.TypeView())) {
                        const auto base_id = unwrap_u64(static_cast<std::uint32_t>(lhs.Payload()));
                        const auto imm = rhs.Payload() & Mask(TypeBits(rhs.TypeView()));
                        if (operations[base_id].type == Type::U64 && imm <= 4096u - bytes) {
                            base = NodeId{base_id};
                            offset = static_cast<std::uint32_t>(imm);
                            return true;
                        }
                    }
                }
            }
        }
        base = NodeId{addr_id};
        offset = 0;
        return true;
    }
    if (first.Kind() == OperandKind::Expression && first.ExpressionOp() == OperandOp::Add &&
        first.Count() == 2 && !first.ShiftAmount() && at != inputs.end()) {
        const auto lhs = *at++;
        if (at != inputs.end()) {
            const auto rhs = *at++;
            if (at == inputs.end() && lhs.Kind() == OperandKind::Value &&
                (lhs.TypeView() == Type::Void || lhs.TypeView() == Type::U64) &&
                rhs.Kind() == OperandKind::Immediate && Scalar(rhs.TypeView())) {
                const auto base_id = unwrap_u64(static_cast<std::uint32_t>(lhs.Payload()));
                const auto imm = rhs.Payload() & Mask(TypeBits(rhs.TypeView()));
                if (operations[base_id].type == Type::U64 && imm <= 4096u - bytes) {
                    base = NodeId{base_id};
                    offset = static_cast<std::uint32_t>(imm);
                    return true;
                }
            }
        }
    }
    return false;
}

}  // namespace

Error RewriteValues(Context& context) {
    auto& editor = context.editor;
    auto& rows = context.rows;
    auto& stats = context.stats;
    const auto& policy = context.policy;
    const auto signed_mul_widen_low = context.signed_mul_widen_low;
    const auto fault_locations = context.fault_locations;
    auto& needs_final_cleanup = context.needs_final_cleanup;
    const auto operations = editor.Operations();
    const auto unit = editor.UnitId();
    // 2. Each live operation is considered once. Read current constants,
    // one copy edge or a neighboring extract, without a value cache.
    UniformCache uniforms;
    ReadableSpanCache readable_spans;
    BlockId block{};
    std::array<NodeId, 2> recent_operands{};
    std::uint8_t next_operand_slot{};
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
            readable_spans.Clear();
            recent_operands = {};
            next_operand_slot = 0;
            block = op.block;
        }
        if (op.opcode == OpCode::Goto || op.opcode == OpCode::NotGoto ||
            op.opcode == OpCode::BindLabel) {
            recent_operands = {};
            next_operand_slot = 0;
        }
        const auto meta = op.Metadata();
        const auto effects = EffectsFor(op.opcode, meta);
        if (effects != Effect::None && effects != Effect::ReadLocal) {
            if (HasAnyEffect(effects, Effect::Unknown | Effect::External | Effect::Control))
                readable_spans.Clear();
            // Value forwarding can survive a disjoint faulting read. A pending
            // store must be committed at every possible architectural observer.
            const bool independent_memory =
                    HasAnyEffect(effects, Effect::ReadMemory | Effect::WriteMemory) &&
                    (policy.guest_memory_disjoint || !HasAnyEffect(effects, Effect::WriteMemory)) &&
                    !HasAnyEffect(effects, Effect::WriteGuestState | Effect::WriteFlags);
            if (HasAnyEffect(effects, Effect::Unknown | Effect::External | Effect::Control) ||
                (!policy.guest_memory_disjoint && HasAnyEffect(effects, Effect::WriteMemory)) ||
                (!independent_memory && HasAnyEffect(effects, Effect::MayFault | Effect::Ordered)) ||
                (op.opcode == OpCode::StoreUniform && meta != OperationMetadata{}))
                uniforms.Clear();
            else if (op.opcode != OpCode::StoreUniform) {
                const auto accesses = UniformEffectsFor(op, editor.Operands(id), policy);
                if (accesses.all_write)
                    uniforms.Clear();
                else if (accesses.all_read)
                    uniforms.ClearPending();
                for (unsigned i = 0; i < accesses.count; ++i) {
                    const auto& field = accesses.fields[i];
                    if (field.write)
                        uniforms.Invalidate(field.range.offset, field.range.bytes);
                    else if (field.read && op.opcode != OpCode::LoadUniform)
                        uniforms.Observe(field.range);
                }
            }
            bool proven_non_faulting_load = false;
            if (fault_locations && policy.guest_memory_disjoint && (rows[at] & live_bit) &&
                !HasAnyEffect(effects, Effect::Ordered | Effect::WriteMemory)) {
                NodeId load_base{};
                std::uint32_t load_offset{}, load_bytes{};
                if (DecodeLoadMemorySpan(
                            editor, operations, id, op, load_base, load_offset, load_bytes))
                    proven_non_faulting_load =
                            readable_spans.CheckAndRecord(load_base, load_offset, load_bytes);
            }
            if ((!proven_non_faulting_load &&
                 HasAnyEffect(effects, Effect::MayFault | Effect::Ordered)) ||
                (!policy.guest_memory_disjoint && HasAnyEffect(effects, Effect::ReadMemory)))
                uniforms.ClearPending();
            else if (proven_non_faulting_load)
                uniforms.ClearScalarPending();
        }
        if (!(rows[at] & live_bit)) continue;
        // These records have no rewritable value inputs or fold to do.
        // Effects above still apply; no decoded Operand draft is needed.
        if (op.opcode == OpCode::LoadImm || op.opcode == OpCode::FaultLocation ||
            op.opcode == OpCode::GuestSafepoint || op.opcode == OpCode::GuestDispatchSafepoint ||
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
        const bool vector_identity =
                editable && !has_metadata && op.type == Type::V128 &&
                (op.opcode == OpCode::VecOr || op.opcode == OpCode::VecAnd ||
                 op.opcode == OpCode::VecXor);
        const bool vector_draft = vector_identity ||
                                  (editable && !has_metadata && signed_mul_widen_low &&
                                   op.opcode == OpCode::VecMadd16 && op.type == Type::V128);
        const bool address_draft =
                editable && !has_metadata &&
                ((op.opcode == OpCode::LoadMemoryTSO && op.operand_count == 1 && Scalar(op.type)) ||
                 (op.opcode == OpCode::StoreMemoryTSO && op.operand_count == 2));
        const bool operand_draft = editable && !has_metadata && op.opcode == OpCode::GetOperand &&
                                   (op.type == Type::U32 || op.type == Type::U64);
        const bool uniform_op =
                op.opcode == OpCode::LoadUniform || op.opcode == OpCode::StoreUniform;
        const bool draft = folding || uniform_op || vector_draft || address_draft || operand_draft;
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
        bool uniform_forward{}, operand_forward{};
        auto opcode = op.opcode;
        auto new_count = count;
        if (editable) {
            if (operand_draft && count <= 4) {
                for (const auto candidate_id : recent_operands) {
                    if (!candidate_id || !(rows[candidate_id.value] & live_bit)) continue;
                    const auto& previous = operations[candidate_id.value];
                    if (previous.opcode != OpCode::GetOperand || previous.type != op.type ||
                        Metadata(previous))
                        continue;
                    const auto actual = editor.Operands(candidate_id);
                    auto cell_it = actual.begin();
                    bool same = true;
                    for (std::size_t i = 0; i < count; ++i) {
                        if (cell_it == actual.end()) {
                            same = false;
                            break;
                        }
                        const auto prev_cell = *cell_it++;
                        if (prev_cell.Kind() != cells[i].kind ||
                            prev_cell.TypeView() != cells[i].type ||
                            prev_cell.Payload() != cells[i].payload ||
                            prev_cell.Count() != cells[i].count ||
                            prev_cell.Code() != cells[i].code) {
                            same = false;
                            break;
                        }
                    }
                    if (same && cell_it == actual.end()) {
                        cells[0] = Operand::ValueOf({unit, candidate_id}, op.type);
                        opcode = op.type == Type::U32 ? OpCode::ZeroExtend32 : OpCode::ZeroExtend64;
                        new_count = 1;
                        operand_forward = true;
                        folding = false;
                        break;
                    }
                }
            }
            if (!has_metadata && op.opcode == OpCode::LoadUniform && count == 1 &&
                uniform.kind == OperandKind::Uniform && uniform.type == op.type &&
                CachedBytes(op.type)) {
                const auto previous = uniforms.Find(static_cast<std::uint32_t>(uniform.payload),
                                                    CachedBytes(op.type),
                                                    op.type,
                                                    unit);
                if (previous.found && previous.v128_lane >= 0) {
                    cells[0] = previous.value;
                    cells[1] = Operand::Immediate(
                            static_cast<std::uint64_t>(previous.v128_lane), Type::U32);
                    opcode = OpCode::VecExtract64;
                    new_count = 2;
                    uniform_forward = true;
                    folding = Scalar(op.type);
                } else if (previous.found) {
                    cells[0] = previous.value;
                    opcode = op.type == Type::V128  ? OpCode::BitCast
                             : op.type == Type::U32 ? OpCode::ZeroExtend32
                                                    : OpCode::ZeroExtend64;
                    uniform_forward = true;
                    folding = Scalar(op.type);
                }
            }
            const bool combined =
                    !operand_forward &&
                    ((vector_identity &&
                      FoldIdentity(editor, operations, op, cells, new_count, opcode)) ||
                     (folding &&
                      (FoldIdentity(editor, operations, op, cells, new_count, opcode) ||
                       (opcode == OpCode::BitInsert &&
                        FoldLowByteMask(editor, operations, id, cells, new_count, opcode)) ||
                       (opcode == OpCode::And &&
                        FoldShiftMask(editor, operations, id, rows, cells, new_count, opcode)) ||
                       ((opcode == OpCode::BitExtract || opcode == OpCode::LsrImm) &&
                        FoldExtractShift(
                                editor, operations, id, rows, cells, new_count, opcode)))) ||
                     (vector_draft && !vector_identity &&
                      FuseSignedMulWidenLow(
                              editor, operations, id, rows, cells, new_count, opcode)) ||
                     (address_draft &&
                      FoldOrderedLoadAddress(editor, operations, id, rows, cells, new_count)));
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
            if (uniform_forward || operand_forward || folded || combined) {
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
                stats.folded += operand_forward || folded || combined;
            } else if (operand_draft && count <= 4) {
                recent_operands[next_operand_slot] = id;
                next_operand_slot ^= 1u;
            }
        }
        if (op.opcode == OpCode::StoreUniform && count == 2 &&
            uniform.kind == OperandKind::Uniform) {
            const auto bytes = (TypeBits(uniform.type) + 7) / 8;
            if (!bytes) {
                uniforms.Clear();
                continue;
            }
            if (!has_metadata && !(rows[at] & pin_bit)) {
                const auto previous = uniforms.Find(
                        static_cast<std::uint32_t>(uniform.payload), bytes, uniform.type, unit);
                Constant old_bits, new_bits;
                if (previous.found && previous.v128_lane < 0 &&
                    ReadConstant(editor, operations, previous.value, old_bits) &&
                    ReadConstant(editor, operations, cells[1], new_bits) &&
                    (old_bits.bits & Mask(TypeBits(uniform.type))) ==
                            (new_bits.bits & Mask(TypeBits(uniform.type)))) {
                    for (const auto input : editor.Operands(id))
                        if (input.Kind() == OperandKind::Value && !DropUse(rows, input.Payload()))
                            return Error::InvalidValue;
                    if (const auto error = editor.Rewrite(id, OpCode::Nop, Type::Void, {});
                        error != Error::None)
                        return error;
                    rows[at] = 0;
                    ++stats.erased;
                    ++stats.uniform_stores_erased;
                    needs_final_cleanup = true;
                    continue;
                }
            }
            std::array<NodeId, 8> stores;
            const auto covered =
                    uniforms.Cover({static_cast<std::uint32_t>(uniform.payload), bytes}, stores);
            for (unsigned i = 0; i < covered; ++i) {
                const auto previous = stores[i];
                if ((rows[previous.value] & pin_bit) || Metadata(operations[previous.value]))
                    continue;
                for (const auto input : editor.Operands(previous))
                    if (input.Kind() == OperandKind::Value &&
                        !DropUse(rows, static_cast<std::uint32_t>(input.Payload())))
                        return Error::InvalidValue;
                if (const auto error = editor.Rewrite(previous, OpCode::Nop, Type::Void, {});
                    error != Error::None)
                    return error;
                rows[previous.value] = 0;
                ++stats.erased;
                ++stats.uniform_stores_erased;
                needs_final_cleanup = true;
            }
            const auto view = ReadType(cells[1], operations);
            const bool cacheable = cells[1].kind == OperandKind::Value &&
                                   CachedBytes(uniform.type) &&
                                   ((Scalar(uniform.type) && Scalar(view) &&
                                     TypeBits(view) <= TypeBits(uniform.type)) ||
                                    (uniform.type == Type::V128 && view == Type::V128));
            uniforms.Add(static_cast<std::uint32_t>(uniform.payload),
                         bytes,
                         uniform.type,
                         cells[1],
                         id,
                         cacheable);
        } else if (op.opcode == OpCode::LoadUniform && count == 1 &&
                   uniform.kind == OperandKind::Uniform && uniform.type == op.type &&
                   CachedBytes(op.type)) {
            if (!uniform_forward) {
                uniforms.Observe(
                        {static_cast<std::uint32_t>(uniform.payload), CachedBytes(op.type)});
                uniforms.Add(static_cast<std::uint32_t>(uniform.payload),
                             CachedBytes(op.type),
                             op.type,
                             Operand::ValueOf({unit, id}, op.type));
            }
        } else if (op.opcode == OpCode::LoadUniform && uniform.kind == OperandKind::Uniform) {
            uniforms.Observe({static_cast<std::uint32_t>(uniform.payload),
                              (TypeBits(uniform.type) + 7) / 8});
        }
    }
    return Error::None;
}

}  // namespace swift::runtime::ir::passes
