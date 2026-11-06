#include "optimize.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <utility>
#include <vector>
#include "runtime/ir/builder.h"

namespace swift::runtime::compile {
namespace {
using namespace ir;
using u64 = std::uint64_t;
constexpr u64 live_bit = u64{1} << 62, pin_bit = u64{1} << 63, count_mask = live_bit - 1;
constexpr std::size_t local_cells = 16;

bool Metadata(const Operation& op) { return op.Metadata() != OperationMetadata{}; }
constexpr u64 all_flags = FlagBits(Flag::All);
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
bool FlagsUniform(OpCode opcode, CellRange inputs, const UniformPolicy& policy) {
    if (opcode != OpCode::LoadUniform && opcode != OpCode::StoreUniform &&
        opcode != OpCode::StoreUniformSelectImm)
        return true;
    if (!policy.flags) return true;
    if (inputs.empty() || inputs.front().Kind() != OperandKind::Uniform) return true;
    const auto field = inputs.front();
    const auto bytes = TypeBits(field.TypeView()) / 8;
    return !bytes || (field.Payload() < u64{policy.flags->offset} + policy.flags->bytes &&
                      u64{policy.flags->offset} < field.Payload() + bytes);
}
u64 FlagReads(OpCode opcode, Effect effects, CellRange inputs, const UniformPolicy& policy) {
    if (HasAnyEffect(effects, flag_observers) ||
        (HasAnyEffect(effects, uniform_access) && FlagsUniform(opcode, inputs, policy)))
        return all_flags;
    if (!HasAnyEffect(effects, Effect::ReadFlags)) return 0;
    switch (opcode) {
        case OpCode::Adc:
        case OpCode::Sbb:
        case OpCode::InvertCarry:
            return FlagBits(Flag::Carry);
        case OpCode::TestFlags:
        case OpCode::TestNotFlags: {
            const auto flags = inputs.front();
            return flags.Kind() == OperandKind::Flags && !(flags.Payload() & ~all_flags)
                           ? flags.Payload()
                           : all_flags;
        }
        case OpCode::CondSet:
        case OpCode::LocalCondSet:
        case OpCode::CondSelect: {
            const auto condition = inputs.front();
            if (condition.Kind() != OperandKind::Condition) return all_flags;
            switch (static_cast<Condition>(condition.Payload())) {
                case Condition::EQ:
                case Condition::NE:
                    return FlagBits(Flag::Zero);
                case Condition::CS:
                case Condition::CC:
                    return FlagBits(Flag::Carry);
                case Condition::MI:
                case Condition::PL:
                    return FlagBits(Flag::Negate);
                case Condition::VS:
                case Condition::VC:
                    return FlagBits(Flag::Overflow);
                case Condition::HI:
                case Condition::LS:
                    return FlagBits(Flag::Carry | Flag::Zero);
                case Condition::GE:
                case Condition::LT:
                    return FlagBits(Flag::Negate | Flag::Overflow);
                case Condition::GT:
                case Condition::LE:
                    return FlagBits(Flag::Zero | Flag::Negate | Flag::Overflow);
                case Condition::AL:
                case Condition::NV:
                    return 0;
            }
            return all_flags;
        }
        // GetFlags returns the whole opaque carrier, regardless of its mask.
        // Unclassified readers do not acquire a narrower implicit contract.
        default:
            return all_flags;
    }
}

struct FlagBlock {
    std::array<NodeId, 6> exit_writers{};
    std::uint8_t gen{}, kill{}, input{static_cast<std::uint8_t>(all_flags)},
            output{static_cast<std::uint8_t>(all_flags)};
    bool queued{};
};

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
bool Sensitive(OpCode opcode, Effect effects) {
    switch (opcode) {
        case OpCode::GetFlags:
        case OpCode::SaveFlags:
        case OpCode::BranchOnlyFlags:
        case OpCode::PublishFCmpFlags:
        case OpCode::PublishSse42StrFlags:
        case OpCode::FCmpCondSet:
        case OpCode::Div128Remainder:
        case OpCode::CpuidUpper:
        case OpCode::GetResult:
        case OpCode::LocalParitySet:
        case OpCode::BindLabel:
            return true;
        default:
            return HasAnyEffect(effects, Effect::Unknown);
    }
}
bool AddUse(std::vector<u64>& rows, std::uint32_t id, bool pin = false) {
    if (id >= rows.size() || (rows[id] & count_mask) == count_mask) return false;
    rows[id] = (rows[id] | live_bit | (pin ? pin_bit : 0)) + 1;
    return true;
}
bool DropUse(std::vector<u64>& rows, std::uint32_t id) {
    if (id >= rows.size() || !(rows[id] & count_mask)) return false;
    --rows[id];
    return true;
}
u64 Mask(unsigned bits) { return bits == 64 ? ~u64{} : bits ? (u64{1} << bits) - 1 : 0; }
Type ReadType(const Operand& cell, std::span<const Operation> operations) {
    return cell.kind == OperandKind::Value && cell.type == Type::Void
                   ? operations[static_cast<std::size_t>(cell.payload)].type
                   : cell.type;
}
bool Scalar(Type type) { return IntegerType(type) && TypeBits(type) <= 64; }

struct Constant {
    u64 bits{};
    Type type{Type::Void};
};
bool ReadConstant(const Builder& editor,
                  std::span<const Operation> operations,
                  const Operand& input,
                  Constant& value) {
    value.type = ReadType(input, operations);
    if (!Scalar(value.type)) return false;
    if (input.kind == OperandKind::Immediate) {
        value.bits = input.payload & Mask(TypeBits(value.type));
        return true;
    }
    if (input.kind != OperandKind::Value || input.payload >= operations.size()) return false;
    const auto& source = operations[static_cast<std::size_t>(input.payload)];
    if (source.opcode != OpCode::LoadImm || Metadata(source) || !Scalar(source.type) ||
        TypeBits(value.type) > TypeBits(source.type))
        return false;
    const auto cells = editor.Operands(NodeId{static_cast<std::uint32_t>(input.payload)});
    auto at = cells.begin();
    if (at == cells.end()) return false;
    const auto literal = *at++;
    if (at != cells.end() || literal.Kind() != OperandKind::Immediate ||
        literal.TypeView() != source.type)
        return false;
    value.bits = literal.Payload() & Mask(TypeBits(value.type));
    return true;
}

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
                 u64 unit,
                 NodeId consumer_id,
                 const Operation& consumer,
                 const std::vector<u64>& rows) {
    if (input.kind != OperandKind::Value || input.payload >= operations.size()) return false;
    const auto& source = operations[static_cast<std::size_t>(input.payload)];
    if (source.opcode == OpCode::BitExtract) {
        const auto meta = consumer.Metadata();
        if (source.type != Type::U8 || Metadata(source) || source.operand_count != 3 ||
            consumer.opcode != OpCode::And || consumer.type != Type::U8 ||
            consumer.operand_count != 2 || source.block != consumer.block ||
            consumer_id.value - input.payload != 1 || ReadType(input, operations) != Type::U8 ||
            (rows[input.payload] & count_mask) != 1 || (rows[input.payload] & pin_bit) ||
            meta.profile != FlagProfile::LogicalZeroCVA || meta.branch_only ||
            (meta.flag_outputs != FlagBits(Flag::Zero) &&
             meta.flag_outputs != FlagBits(Flag::Carry | Flag::Zero)))
            return false;
        const auto outer_inputs = editor.Operands(consumer_id);
        auto outer_at = outer_inputs.begin();
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
        (!Scalar(source.type) && source.type != Type::V128) ||
        ReadType(input, operations) != source.type)
        return false;
    const auto cells = editor.Operands(NodeId{static_cast<std::uint32_t>(input.payload)});
    auto at = cells.begin();
    if (at == cells.end()) return false;
    const auto original = *at++;
    if (at != cells.end() || original.Kind() != OperandKind::Value) return false;
    const auto original_id = static_cast<std::uint32_t>(original.Payload());
    const Operand replacement = Operand::ValueOf({unit, NodeId{original_id}}, original.TypeView());
    const auto view = ReadType(replacement, operations);
    if (raw_copy) {
        if (view != source.type) return false;
    } else {
        if (!Scalar(view)) return false;
        const auto bits =
                std::min(TypeBits(view), source.opcode == OpCode::ZeroExtend64 ? 64u : 32u);
        const auto& producer = operations[original_id];
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
    input = Operand::ValueOf({unit, NodeId{static_cast<std::uint32_t>(original.Payload())}},
                             source.type);
    return true;
}

bool Foldable(const Operation& op) {
    if (!Scalar(op.type)) return false;
    switch (op.opcode) {
        case OpCode::Zero:
        case OpCode::Neg:
        case OpCode::Not:
        case OpCode::TestZero:
        case OpCode::TestNotZero:
        case OpCode::ZeroExtend32:
        case OpCode::ZeroExtend32To64:
        case OpCode::ZeroExtend64:
        case OpCode::SignExtend:
        case OpCode::TestBit:
        case OpCode::BitExtract:
        case OpCode::BitInsert:
        case OpCode::LslImm:
        case OpCode::LslValue:
        case OpCode::LsrImm:
        case OpCode::LsrValue:
        case OpCode::AsrImm:
        case OpCode::AsrValue:
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::Mul:
        case OpCode::And:
        case OpCode::Or:
        case OpCode::Xor:
            return true;
        default:
            return false;
    }
}

bool FoldLowByteMask(const Builder& editor,
                     std::span<const Operation> operations,
                     NodeId id,
                     std::span<Operand> cells,
                     std::size_t& count,
                     OpCode& opcode) {
    const auto& op = operations[id.value];
    if (opcode != OpCode::BitInsert || op.type != Type::U32 || count != 4 ||
        cells[0].kind != OperandKind::Value || cells[1].kind != OperandKind::Value ||
        operations[cells[0].payload].type != Type::U32 ||
        ReadType(cells[0], operations) != Type::U32 || ReadType(cells[1], operations) != Type::U8 ||
        cells[2].kind != OperandKind::Immediate || cells[2].payload ||
        cells[3].kind != OperandKind::Immediate || cells[3].payload != 8)
        return false;
    const auto child_id = static_cast<std::uint32_t>(cells[1].payload);
    const auto& child = operations[child_id];
    if (child.opcode != OpCode::And || child.type != Type::U8 || Metadata(child) ||
        child.block != op.block)
        return false;
    const auto inputs = editor.Operands(NodeId{child_id});
    auto at = inputs.begin();
    if (at == inputs.end()) return false;
    const auto extracted = *at++;
    if (at == inputs.end()) return false;
    const auto mask = *at++;
    if (at != inputs.end() || extracted.Kind() != OperandKind::Value ||
        mask.Kind() != OperandKind::Immediate || !Scalar(mask.TypeView()))
        return false;
    const auto extract_id = static_cast<std::uint32_t>(extracted.Payload());
    const auto& extract = operations[extract_id];
    if (extract.opcode != OpCode::BitExtract || extract.type != Type::U8 || Metadata(extract) ||
        extract.block != op.block ||
        (extracted.TypeView() != Type::Void && extracted.TypeView() != Type::U8))
        return false;
    const auto fields = editor.Operands(NodeId{extract_id});
    auto field = fields.begin();
    if (field == fields.end()) return false;
    const auto parent = *field++;
    if (field == fields.end()) return false;
    const auto first = *field++;
    if (field == fields.end()) return false;
    const auto length = *field++;
    if (field != fields.end() || parent.Kind() != OperandKind::Value ||
        parent.Payload() != cells[0].payload || parent.TypeView() != cells[0].type ||
        first.Kind() != OperandKind::Immediate || first.Payload() ||
        length.Kind() != OperandKind::Immediate || length.Payload() != 8)
        return false;
    const auto mask_bits = std::min(8u, TypeBits(mask.TypeView()));
    const auto byte_mask = mask.Payload() & ((u64{1} << mask_bits) - 1);
    // Preserve the whole U32 parent, including bits 8..31. Only its low
    // byte is replaced; all other child consumers keep the original value.
    cells[1] = Operand::Immediate(0xffffff00u | byte_mask, Type::U32);
    count = 2;
    opcode = OpCode::And;
    return true;
}

bool FoldExtractShift(const Builder& editor,
                      std::span<const Operation> operations,
                      NodeId id,
                      const std::vector<u64>& rows,
                      std::span<Operand> cells,
                      std::size_t& count,
                      OpCode& opcode) {
    const auto& op = operations[id.value];
    if (opcode == OpCode::BitExtract && op.type == Type::U8 && count == 3 &&
        cells[0].kind == OperandKind::Value && cells[1].kind == OperandKind::Immediate &&
        !cells[1].payload && cells[2].kind == OperandKind::Immediate && cells[2].payload == 8) {
        const auto inner_id = static_cast<std::uint32_t>(cells[0].payload);
        const auto& inner = operations[inner_id];
        if (inner.opcode != OpCode::BitExtract || inner.type != Type::U32 || Metadata(inner) ||
            inner.block != op.block || inner.operand_count != 3 ||
            (rows[inner_id] & count_mask) != 1 || (rows[inner_id] & pin_bit))
            return false;
        const auto inputs = editor.Operands(NodeId{inner_id});
        auto at = inputs.begin();
        const auto parent = *at++, first = *at++, length = *at;
        if (parent.Kind() != OperandKind::Value || first.Kind() != OperandKind::Immediate ||
            length.Kind() != OperandKind::Immediate || !length.Payload() || length.Payload() > 8 ||
            first.Payload() >= 64 || length.Payload() > 64 - first.Payload())
            return false;
        // The inner's canonical U32 result already fits in low8. Preserve
        // its raw parent view and let the existing reverse walk retire it.
        cells[0] = Operand::ValueOf(
                {editor.UnitId(), NodeId{static_cast<std::uint32_t>(parent.Payload())}},
                parent.TypeView());
        cells[1] = Operand::Immediate(first.Payload(), first.TypeView());
        cells[2] = Operand::Immediate(length.Payload(), length.TypeView());
        return true;
    }
    if (opcode != OpCode::LsrImm || op.type != Type::U32 || count != 2 ||
        cells[0].kind != OperandKind::Value || ReadType(cells[0], operations) != Type::U32 ||
        cells[1].kind != OperandKind::Immediate)
        return false;
    const auto inner_id = static_cast<std::uint32_t>(cells[0].payload);
    const auto& inner = operations[inner_id];
    if (inner.opcode != OpCode::BitExtract || inner.type != Type::U8 || Metadata(inner) ||
        inner.block != op.block || inner.operand_count != 3 || (rows[inner_id] & pin_bit))
        return false;
    u64 uses = 1;
    const auto distance = id.value - inner_id;
    if (distance != 1) {
        if (distance != 2) return false;
        const auto& gap = operations[inner_id + 1];
        if (Metadata(gap) || gap.block != op.block) return false;
        if (gap.opcode != OpCode::Nop) {
            if (gap.opcode != OpCode::ZeroExtend32 || gap.type != Type::U32 ||
                gap.operand_count != 1 || (rows[inner_id + 1] & (count_mask | pin_bit)))
                return false;
            const auto edge = editor.Operands(NodeId{inner_id + 1}).front();
            if (edge.Kind() != OperandKind::Value || edge.Payload() != inner_id ||
                !Scalar(ReadType(
                        Operand::ValueOf({editor.UnitId(), NodeId{inner_id}}, edge.TypeView()),
                        operations)))
                return false;
            // ForwardCopy removed the outer use of this normalizer. Its
            // own inner edge remains until the existing reverse DCE walk.
            uses = 2;
        }
    }
    if ((rows[inner_id] & count_mask) != uses) return false;
    Constant amount;
    if (!ReadConstant(editor, operations, cells[1], amount)) return false;
    const auto shift = amount.bits & 31;
    if (!shift || shift >= 8) return false;
    const auto inputs = editor.Operands(NodeId{inner_id});
    auto at = inputs.begin();
    const auto parent = *at++, first = *at++, length = *at;
    if (parent.Kind() != OperandKind::Value || first.Kind() != OperandKind::Immediate ||
        length.Kind() != OperandKind::Immediate || first.Payload() > 56 || length.Payload() != 8)
        return false;
    // BitExtract reads the raw parent carrier, then produces canonical U8.
    // Preserve its parent view; only the outer result remains explicitly U32.
    cells[0] = Operand::ValueOf(
            {editor.UnitId(), NodeId{static_cast<std::uint32_t>(parent.Payload())}},
            parent.TypeView());
    cells[1] = Operand::Immediate(first.Payload() + shift, Type::U8);
    cells[2] = Operand::Immediate(8 - shift, Type::U8);
    count = 3;
    opcode = OpCode::BitExtract;
    return true;
}

bool FuseSignedMulWidenLow(const Builder& editor,
                           std::span<const Operation> operations,
                           NodeId id,
                           const std::vector<u64>& rows,
                           std::span<Operand> cells,
                           std::size_t& count,
                           OpCode& opcode) {
    const auto& op = operations[id.value];
    if (count != 2) return false;
    std::array<Operand, 2> parents;
    bool exclusive{};
    for (std::size_t i = 0; i < parents.size(); ++i) {
        if (cells[i].kind != OperandKind::Value || ReadType(cells[i], operations) != Type::V128)
            return false;
        const auto inner_id = static_cast<std::uint32_t>(cells[i].payload);
        const auto& inner = operations[inner_id];
        if (inner.opcode != OpCode::VecExtend || inner.type != Type::V128 || Metadata(inner) ||
            inner.block != op.block || inner.operand_count != 4)
            return false;
        const auto inputs = editor.Operands(NodeId{inner_id});
        auto at = inputs.begin();
        const auto parent = *at++, source_bits = *at++, result_bits = *at++, sign = *at;
        if (parent.Kind() != OperandKind::Value ||
            operations[parent.Payload()].type != Type::V128 ||
            (parent.TypeView() != Type::Void && parent.TypeView() != Type::V128) ||
            source_bits.Kind() != OperandKind::Immediate || !Scalar(source_bits.TypeView()) ||
            source_bits.Payload() != 16 || result_bits.Kind() != OperandKind::Immediate ||
            !Scalar(result_bits.TypeView()) || result_bits.Payload() != 32 ||
            sign.Kind() != OperandKind::Immediate || !Scalar(sign.TypeView()) || sign.Payload())
            return false;
        parents[i] = Operand::ValueOf(
                {editor.UnitId(), NodeId{static_cast<std::uint32_t>(parent.Payload())}},
                parent.TypeView());
        exclusive |= !(rows[inner_id] & pin_bit) && (rows[inner_id] & count_mask) == 1;
    }
    // A non-tail 2->3 rewrite appends all three words. The product plus one
    // exclusive Extend provide four words of the existing 2N edit allowance.
    // No structural fold creates/grows VecExtend, so this charge is unique.
    if (!exclusive) return false;
    cells[0] = parents[0];
    cells[1] = parents[1];
    cells[2] = Operand::Immediate(16, Type::U8);
    count = 3;
    opcode = OpCode::VecSignedMulWidenLow;
    return true;
}

bool FoldOrderedLoadAddress(const Builder& editor,
                            std::span<const Operation> operations,
                            NodeId id,
                            const std::vector<u64>& rows,
                            std::span<Operand> cells,
                            std::size_t& count) {
    if (count != 1 || cells[0].kind != OperandKind::Value ||
        ReadType(cells[0], operations) != Type::U64)
        return false;
    const auto address_id = static_cast<std::uint32_t>(cells[0].payload);
    const auto& address = operations[address_id];
    if ((address.opcode != OpCode::Add && address.opcode != OpCode::Sub) ||
        address.type != Type::U64 || Metadata(address) ||
        address.block != operations[id.value].block || (rows[address_id] & count_mask) != 1 ||
        (rows[address_id] & pin_bit))
        return false;
    const auto inputs = editor.Operands(NodeId{address_id});
    auto at = inputs.begin();
    const auto base = *at++, literal = *at++;
    if (at != inputs.end() || base.Kind() != OperandKind::Value ||
        operations[base.Payload()].type != Type::U64 ||
        (base.TypeView() != Type::Void && base.TypeView() != Type::U64) ||
        literal.Kind() != OperandKind::Immediate || literal.TypeView() != Type::U64)
        return false;
    const auto displacement =
            address.opcode == OpCode::Sub ? u64{} - literal.Payload() : literal.Payload();
    if (displacement > 255 && displacement < u64{} - 256) return false;
    // The load plus its exclusive Add/Sub cover four edit words. Even a
    // negative wide literal needs only Expression + Value + two Imm words.
    // No other structural rewrite creates or grows these address producers.
    cells[0] =
            Operand::Expression(address.opcode == OpCode::Add ? OperandOp::Add : OperandOp::Sub, 2);
    cells[1] = Operand::ValueOf(
            {editor.UnitId(), NodeId{static_cast<std::uint32_t>(base.Payload())}}, base.TypeView());
    cells[2] = Operand::Immediate(literal.Payload(), Type::U64);
    count = 3;
    return true;
}

bool Fold(const Builder& editor,
          std::span<const Operation> operations,
          const Operation& op,
          std::span<const Operand> cells,
          u64& result) {
    const auto bits = TypeBits(op.type);
    const auto mask = Mask(bits);
    Constant first, second, third;
    if (op.opcode == OpCode::Zero && cells.empty()) {
        result = 0;
        return true;
    }
    if (cells.empty() || !ReadConstant(editor, operations, cells[0], first)) return false;
    const auto same = first.type == op.type;
    if (cells.size() == 1) {
        switch (op.opcode) {
            // BitCast/GetOperand are raw transport. Their consumer view must
            // not truncate an underlying carrier before a later wider use.
            case OpCode::Neg:
                if (!same) return false;
                result = u64{} - first.bits;
                break;
            case OpCode::Not:
            case OpCode::TestZero:
                result = first.bits == 0;
                break;
            case OpCode::TestNotZero:
                result = first.bits != 0;
                break;
            case OpCode::ZeroExtend32:
            case OpCode::ZeroExtend32To64:
                result = first.bits & UINT32_MAX;
                break;
            case OpCode::ZeroExtend64:
                result = first.bits;
                break;
            case OpCode::SignExtend: {
                const auto width = TypeBits(first.type);
                result = first.bits;
                if (width < bits && (result & (u64{1} << (width - 1)))) result |= ~Mask(width);
                break;
            }
            default:
                return false;
        }
    } else if (cells.size() == 2 && ReadConstant(editor, operations, cells[1], second)) {
        if (op.opcode == OpCode::TestBit) {
            if (second.bits >= 64) return false;
            result = second.bits < TypeBits(first.type) ? (first.bits >> second.bits) & 1 : 0;
        } else {
            if (!same) return false;
            switch (op.opcode) {
                case OpCode::LslImm:
                case OpCode::LslValue:
                    result = first.bits << (second.bits & (bits - 1));
                    break;
                case OpCode::LsrImm:
                case OpCode::LsrValue:
                    result = first.bits >> (second.bits & (bits - 1));
                    break;
                case OpCode::AsrImm:
                case OpCode::AsrValue: {
                    const auto shift = static_cast<unsigned>(second.bits & (bits - 1));
                    result = first.bits >> shift;
                    if (shift && (first.bits & (u64{1} << (bits - 1))))
                        result |= mask ^ Mask(bits - shift);
                    break;
                }
                default:
                    if (second.type != op.type) return false;
                    switch (op.opcode) {
                        case OpCode::Add:
                            result = first.bits + second.bits;
                            break;
                        case OpCode::Sub:
                            result = first.bits - second.bits;
                            break;
                        case OpCode::Mul:
                            result = first.bits * second.bits;
                            break;
                        case OpCode::And:
                            result = first.bits & second.bits;
                            break;
                        case OpCode::Or:
                            result = first.bits | second.bits;
                            break;
                        case OpCode::Xor:
                            result = first.bits ^ second.bits;
                            break;
                        default:
                            return false;
                    }
            }
        }
    } else if (op.opcode == OpCode::BitExtract && cells.size() == 3 &&
               ReadConstant(editor, operations, cells[1], second) &&
               ReadConstant(editor, operations, cells[2], third)) {
        const auto width = TypeBits(first.type);
        if (second.bits >= width || !third.bits || third.bits > width - second.bits) return false;
        result = (first.bits >> second.bits) & Mask(static_cast<unsigned>(third.bits));
    } else
        return false;
    result &= mask;
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

OptimizeResult Finish(Builder& editor,
                      Error error,
                      OptimizeStats stats,
                      bool allocation_failed = false) {
    auto submitted = editor.Submit();
    if (submitted.error != Error::None) error = submitted.error;
    stats.words_after = submitted.program.OperandArena().size();
    return {error, std::move(submitted.program), stats, allocation_failed};
}
}  // namespace

OptimizeResult Optimize(ir::Program&& input, std::size_t max_operand_words, UniformPolicy policy) {
    std::vector<u64> rows;
    return Optimize(std::move(input), rows, max_operand_words, policy);
}

OptimizeResult Optimize(ir::Program&& input,
                        std::vector<u64>& rows,
                        std::size_t max_operand_words,
                        UniformPolicy policy) {
    if (!input) return {Error::Consumed, std::move(input), {}};
    OptimizeStats stats;
    stats.nodes = input.Operations().size();
    stats.words_before = input.OperandArena().size();
    rows.assign(stats.nodes, 0);  // One actual per-node table; caller still owns input.
    stats.dense_bytes = rows.size() * sizeof(u64);
    constexpr auto max_size = std::numeric_limits<std::size_t>::max();
    const auto hard_limit = stats.nodes > (max_size - stats.words_before) / 2
                                    ? max_size
                                    : stats.words_before + 2 * stats.nodes;
    const bool function_scope = input.ScopeKind() == Scope::Function;
    const bool signed_mul_widen_low = input.Target().Has(TargetFeature::VectorSignedMulWidenLow);
    auto editor = Builder::Edit(std::move(input), std::min(hard_limit, max_operand_words));
    try {
        const auto operations = editor.Operations();
        const auto blocks = editor.Blocks();
        const auto unit = editor.UnitId();
        // A single external exit has the same All-flags boundary as a Block.
        // Keep its Function scope; only the unused CFG summaries are omitted.
        const bool function =
                function_scope &&
                (blocks.size() != 1 || blocks.front().terminal.kind == TerminalKind::Jump ||
                 blocks.front().terminal.kind == TerminalKind::Branch);
        std::vector<FlagBlock> flags;
        std::vector<std::size_t> predecessor_offsets, pending;
        std::vector<BlockId> predecessors;
        if (function) {
            flags.resize(blocks.size());
            predecessor_offsets.resize(blocks.size() + 1);
            pending.resize(blocks.size());
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
            predecessors.resize(predecessor_offsets.back());
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
                    flags.capacity() * sizeof(FlagBlock) +
                    (predecessor_offsets.capacity() + pending.capacity()) * sizeof(std::size_t) +
                    predecessors.capacity() * sizeof(BlockId);
        }
        for (const auto& block : blocks)
            if (block.terminal.HasValue()) {
                if (!AddUse(rows, block.terminal.value.value))
                    return Finish(editor, Error::CapacityOverflow, stats);
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
            const auto effects = EffectsFor(op.opcode, meta);
            const auto write =
                    meta != OperationMetadata{} || HasAnyEffect(effects, Effect::WriteFlags)
                            ? FlagWrites(editor, id, op, meta)
                            : FlagWrite{};
            const auto keep = KeptFlags(write, needed_flags);
            const bool dead_setter =
                    (write.kind == FlagWriteKind::Mask || write.kind == FlagWriteKind::Fixed) &&
                    !keep;
            if (keep != write.mask || dead_setter)
                if (const auto error = TrimFlags(editor, id, op, write, keep); error != Error::None)
                    return Finish(editor, error, stats);
            if (dead_setter) {
                ++stats.erased;
                continue;
            }
            if (Observable(effects, write) || keep) rows[at] |= live_bit;
            if (!(rows[at] & live_bit)) continue;
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
                        return Finish(editor, Error::CapacityOverflow, stats);
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
                info.output = static_cast<std::uint8_t>(output);
                const auto input = static_cast<std::uint8_t>(info.gen | (output & ~info.kill));
                if (input == info.input) continue;
                info.input = input;
                for (auto edge = predecessor_offsets[b]; edge < predecessor_offsets[b + 1];
                     ++edge) {
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
                    const bool dead_setter = write.kind != FlagWriteKind::Metadata && !keep;
                    const bool dead_value =
                            !keep && !(rows[id.value] & count_mask) && !Observable(effects, write);
                    if ((dead_setter || dead_value) && (rows[id.value] & live_bit))
                        for (const auto cell : editor.Operands(id))
                            if (cell.Kind() == OperandKind::Value &&
                                !DropUse(rows, static_cast<std::uint32_t>(cell.Payload())))
                                return Finish(editor, Error::InvalidValue, stats);
                    if (const auto error = TrimFlags(editor, id, op, write, keep);
                        error != Error::None)
                        return Finish(editor, error, stats);
                    if (dead_value && !dead_setter)
                        if (const auto error = editor.EraseUnused(id); error != Error::None)
                            return Finish(editor, error, stats);
                    if (dead_setter || dead_value) {
                        rows[id.value] = 0;
                        ++stats.erased;
                    }
                }
            }
        }

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
                (!independent_memory &&
                 HasAnyEffect(effects, Effect::MayFault | Effect::Ordered)) ||
                (HasAnyEffect(effects, Effect::WriteGuestState) &&
                 op.opcode != OpCode::StoreUniform))
                uniforms.Clear();
            else if (HasAnyEffect(effects, Effect::WriteFlags)) {
                if (policy.flags)
                    uniforms.Invalidate(policy.flags->offset, policy.flags->bytes);
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
                                       op.opcode == OpCode::LoadMemoryTSO &&
                                       op.operand_count == 1 &&
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
                    if (ForwardCopy(editor, operations, replacement, unit, id, op, rows)) {
                        const auto new_id = static_cast<std::uint32_t>(replacement.payload);
                        if (!(rows[old_id] & count_mask))
                            return Finish(editor, Error::InvalidValue, stats);
                        if ((rows[new_id] & count_mask) == count_mask)
                            return Finish(editor, Error::CapacityOverflow, stats);
                        if (const auto error = editor.ReplaceValueUse(
                                    id, edge, {unit, NodeId{new_id}}, replacement.type);
                            error != Error::None)
                            return Finish(editor, error, stats);
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
            const auto uniform =
                    count && cells[0].kind == OperandKind::Uniform ? cells[0] : Operand{};
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
                         (FoldLowByteMask(editor, operations, id, cells, new_count, opcode) ||
                          FoldExtractShift(
                                  editor, operations, id, rows, cells, new_count, opcode))) ||
                        (vector_draft &&
                         FuseSignedMulWidenLow(
                                 editor, operations, id, rows, cells, new_count, opcode)) ||
                        (address_draft &&
                         FoldOrderedLoadAddress(editor, operations, id, rows, cells, new_count));
                u64 bits{};
                const auto candidate =
                        Operation{op.flag_outputs, 0, 0, op.block, opcode, op.type, op.properties};
                const bool folded =
                        folding &&
                        Fold(editor, operations, candidate, {cells.data(), new_count}, bits);
                if (folded) {
                    opcode = OpCode::LoadImm;
                    cells[0] = Operand::Immediate(bits, op.type);
                    new_count = 1;
                }
                if (uniform_forward || folded || combined) {
                    const auto replacement = std::span<const Operand>{cells.data(), new_count};
                    if (const auto error = editor.Rewrite(id, opcode, op.type, replacement, meta);
                        error != Error::None)
                        return Finish(editor, error, stats);
                    for (std::size_t i = 0; i < old_use_count; ++i)
                        if (!DropUse(rows, old_uses[i]))
                            return Finish(editor, Error::InvalidValue, stats);
                    for (const auto& cell : replacement)
                        if (cell.kind == OperandKind::Value &&
                            !AddUse(rows, static_cast<std::uint32_t>(cell.payload)))
                            return Finish(editor, Error::CapacityOverflow, stats);
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
                    uniforms.Add(static_cast<std::uint32_t>(uniform.payload),
                                 bytes,
                                 uniform.type,
                                 cells[1]);
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

        // 3. Recompute flags demand from the current graph, coupled to DCE.
        // A now-dead flag reader must not resurrect older flag writers.
        // Deleting a formerly live node releases its current inputs, so both
        // data and flag-only chains close in this same reverse walk.
        // Function outputs stay frozen. A reader killed here can leave
        // excess demand in a predecessor; no partially updated global
        // solution is consumed while nodes and uses are being removed.
        flags_block = {};
        needed_flags = all_flags;
        for (std::size_t at = operations.size(); at--;) {
            const auto id = NodeId{static_cast<std::uint32_t>(at)};
            const auto& op = operations[at];
            if (op.block != flags_block) {
                needed_flags = function ? flags[op.block.value].output : all_flags;
                flags_block = op.block;
            }
            if (op.opcode == OpCode::Nop) continue;
            const auto meta = op.Metadata();
            const auto effects = EffectsFor(op.opcode, meta);
            const auto write =
                    meta != OperationMetadata{} || HasAnyEffect(effects, Effect::WriteFlags)
                            ? FlagWrites(editor, id, op, meta)
                            : FlagWrite{};
            const auto keep = KeptFlags(write, needed_flags);
            const bool dead_setter =
                    (write.kind == FlagWriteKind::Mask || write.kind == FlagWriteKind::Fixed) &&
                    !keep;
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
                        return Finish(editor, Error::InvalidValue, stats);
            if (keep != write.mask || dead_setter ||
                (discard_omitted && !meta.discard_omitted_flags))
                if (const auto error = TrimFlags(editor, id, op, write, keep, discard_omitted);
                    error != Error::None)
                    return Finish(editor, error, stats);
            if (dead_setter) {
                ++stats.erased;
                continue;
            }
            if (live) {
                const auto reads =
                        write.kind == FlagWriteKind::Opaque ? all_flags
                        : HasAnyEffect(effects, flag_inputs)
                                ? FlagReads(op.opcode, effects, editor.Operands(id), policy)
                                : 0;
                needed_flags = (needed_flags & ~keep) | reads;
                continue;
            }
            if (const auto error = editor.EraseUnused(id); error != Error::None)
                return Finish(editor, error, stats);
            ++stats.erased;
        }
        return Finish(editor, Error::None, stats);
    } catch (const std::bad_alloc&) {
        return Finish(editor, Error::CapacityOverflow, stats, true);
    }
}

}  // namespace swift::runtime::compile
