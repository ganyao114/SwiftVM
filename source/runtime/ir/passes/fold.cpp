#include "detail.h"

#include <algorithm>

namespace swift::runtime::ir::passes {
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
    const bool store = operations[id.value].opcode == OpCode::StoreMemoryTSO;
    if (count != (store ? 2u : 1u) || cells[0].kind != OperandKind::Value ||
        ReadType(cells[0], operations) != Type::U64 ||
        (store && (cells[1].kind != OperandKind::Value || !Scalar(ReadType(cells[1], operations)))))
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
    // The access plus its exclusive Add/Sub cover four edit words. Stores
    // keep their value cell and turn negative wide literals into narrow Sub.
    // No other structural rewrite creates or grows these address producers.
    const auto stored = store ? cells[1] : Operand{};
    auto op = address.opcode == OpCode::Add ? OperandOp::Add : OperandOp::Sub;
    auto amount = literal.Payload();
    if (store && amount > UINT32_MAX) {
        op = op == OperandOp::Add ? OperandOp::Sub : OperandOp::Add;
        amount = u64{} - amount;
    }
    cells[0] = Operand::Expression(op, 2);
    cells[1] = Operand::ValueOf(
            {editor.UnitId(), NodeId{static_cast<std::uint32_t>(base.Payload())}}, base.TypeView());
    cells[2] = Operand::Immediate(amount, Type::U64);
    if (store) cells[3] = stored;
    count = store ? 4 : 3;
    return true;
}

bool FoldIdentity(const Builder& editor,
                  std::span<const Operation> operations,
                  const Operation& op,
                  std::span<Operand> cells,
                  std::size_t& count,
                  OpCode& opcode) {
    if (count != 2 || Metadata(op) || !Scalar(op.type)) return false;
    Constant right;
    if (!ReadConstant(editor, operations, cells[1], right)) return false;
    if (opcode == OpCode::And && !right.bits) {
        opcode = OpCode::LoadImm;
        cells[0] = Operand::Immediate(0, op.type);
        count = 1;
        return true;
    }
    if ((op.type != Type::U32 && op.type != Type::U64) || ReadType(cells[0], operations) != op.type)
        return false;
    bool identity{};
    switch (opcode) {
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::Or:
        case OpCode::Xor:
            identity = !right.bits;
            break;
        case OpCode::And:
            identity = right.bits == Mask(TypeBits(op.type));
            break;
        case OpCode::LslImm:
        case OpCode::LsrImm:
        case OpCode::AsrImm:
        case OpCode::RorImm:
            identity = !(right.bits & (TypeBits(op.type) - 1));
            break;
        default:
            break;
    }
    if (!identity) return false;
    opcode = op.type == Type::U32 ? OpCode::ZeroExtend32 : OpCode::ZeroExtend64;
    count = 1;
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

}  // namespace swift::runtime::ir::passes
