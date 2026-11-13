#include "emit.h"

#define __ masm.exact.

namespace swift::runtime::backend::arm64 {

namespace detail {
unsigned Writer::Expression(Cursor& cursor,
                            unsigned first,
                            unsigned second,
                            unsigned native_width) {
    const auto input = cursor.Take();
    if (input.Kind() != OperandKind::Expression) return Read(input, first, 64, false, native_width);
    if (input.Count() != 2) Reject(CompileError::UnsupportedOperand);
    const auto expression = input.ExpressionOp();
    if ((expression != OperandOp::Add && expression != OperandOp::Sub &&
         expression != OperandOp::AddShift) ||
        (input.ShiftAmount() && expression != OperandOp::AddShift) || input.ShiftAmount() >= 64)
        Reject(CompileError::UnsupportedOperand);
    // A W consumer needs only the expression's low 32 bits. Large shifts
    // retain the X expression and let that consumer take its ordinary view.
    if (input.ShiftAmount() >= 32) native_width = 64;
    const auto base = cursor.Take(), operand = cursor.Take();
    if ((expression == OperandOp::Add || expression == OperandOp::Sub) &&
        operand.Kind() == OperandKind::Immediate) {
        auto literal = IntegerImmediate(operand);
        const auto bits = Width(operand.TypeView());
        if (bits < 64) literal &= (u64{1} << bits) - 1;
        if (native_width == 32) literal &= UINT32_MAX;
        bool subtract = expression == OperandOp::Sub;
        if ((native_width == 32 && (literal >> 31)) || (bits == 64 && (literal >> 63))) {
            literal = u64{} - literal;
            if (native_width == 32) literal &= UINT32_MAX;
            subtract = !subtract;
        }
        const bool shifted = literal > 4095 && !(literal & 4095);
        const auto field = shifted ? literal >> 12 : literal;
        if (field <= 4095) {
            const auto left = Read(base, first, 64, false, native_width);
            if (subtract)
                __ sub(R(first, native_width), R(left, native_width), Operand(literal));
            else
                __ add(R(first, native_width), R(left, native_width), Operand(literal));
            return first;
        }
        if (literal <= 0xffffff && (literal >> 16) && (literal & 0xffff) &&
            (literal & 0xffff) != 0xffff) {
            // Two immediate additions beat materializing both nonzero 16-bit
            // halves before a register ADD. Both steps leave guest flags alone.
            const auto left = Read(base, first, 64, false, native_width);
            const auto result = R(first, native_width);
            if (subtract) {
                __ sub(result, R(left, native_width), Operand(literal & ~u64{4095}));
                __ sub(result, result, Operand(literal & 4095));
            } else {
                __ add(result, R(left, native_width), Operand(literal & ~u64{4095}));
                __ add(result, result, Operand(literal & 4095));
            }
            return first;
        }
    }
    if (native_width == 64 && operand.Kind() == OperandKind::Value && input.ShiftAmount() <= 4) {
        const auto bits =
                std::min(Width(View(operand)), allocation.homes[operand.Payload()].CarrierBits());
        if (bits == 32) {
            const auto left = Read(base, first),
                       right = RawValue(static_cast<u32>(operand.Payload()), second, true);
            // The actual W index performs the logical truncation. Do not
            // materialize a zero-extended X temporary before address ADD.
            const Operand shifted_index{W(right), v::UXTW, input.ShiftAmount()};
            if (expression == OperandOp::Sub)
                __ sub(X(first), X(left), shifted_index);
            else
                __ add(X(first), X(left), shifted_index);
            return first;
        }
    }
    const auto left = Read(base, first, 64, false, native_width),
               right = Read(operand, second, 64, false, native_width);
    const Operand shifted_index{R(right, native_width), v::LSL, input.ShiftAmount()};
    if (expression == OperandOp::Sub)
        __ sub(R(first, native_width), R(left, native_width), shifted_index);
    else
        __ add(R(first, native_width), R(left, native_width), shifted_index);
    return first;
}

unsigned Writer::OrderedAddress(Cursor& cursor, std::int32_t& offset, bool store) {
    if (!store && !RcpcImm9()) return Expression(cursor, 16, 17);
    auto probe = cursor;
    const auto expression = probe.Take();
    if (expression.Kind() != OperandKind::Expression || expression.Count() != 2 ||
        expression.ShiftAmount() ||
        (expression.ExpressionOp() != OperandOp::Add &&
         expression.ExpressionOp() != OperandOp::Sub))
        return Expression(cursor, 16, 17);
    const auto base = probe.Take(), literal = probe.Take();
    if (base.Kind() != OperandKind::Value || literal.Kind() != OperandKind::Immediate ||
        literal.TypeView() != Type::U64)
        return Expression(cursor, 16, 17);
    auto displacement = literal.Payload();
    if (expression.ExpressionOp() == OperandOp::Sub) displacement = u64{} - displacement;
    const auto end = probe.at;
    if (store) {
        const auto value = probe.Take();
        if (value.Kind() == OperandKind::Value &&
            source.Operations()[value.Payload()].type == Type::V128) {
            // STR Q uses an unsigned imm12 scaled by 16, independently of RCpc.
            if (displacement > 4095u * 16u || (displacement & 15))
                return Expression(cursor, 16, 17);
            offset = static_cast<std::int32_t>(displacement);  // Keep the shared output in bytes.
            cursor.at = end;
            return Read(base, 16);
        }
    }
    if (!RcpcImm9() || (displacement > 255 && displacement < u64{} - 256))
        return Expression(cursor, 16, 17);
    offset = displacement <= 255 ? static_cast<std::int32_t>(displacement)
                                 : -static_cast<std::int32_t>(u64{} - displacement);
    cursor.at = end;
    return Read(base, 16);
}

unsigned Writer::VectorAddress(Cursor& cursor, u32& scaled) {
    auto look = cursor;
    const auto expression = look.Take();
    if (expression.Kind() == OperandKind::Expression && expression.Count() == 2 &&
        !expression.ShiftAmount() &&
        (expression.ExpressionOp() == OperandOp::Add ||
         expression.ExpressionOp() == OperandOp::Sub)) {
        const auto base = look.Take(), literal = look.Take();
        if (base.Kind() == OperandKind::Value && literal.Kind() == OperandKind::Immediate &&
            literal.TypeView() == Type::U64) {
            auto displacement = literal.Payload();
            if (expression.ExpressionOp() == OperandOp::Sub) displacement = u64{} - displacement;
            if (displacement <= 4095u * 16u && !(displacement & 15)) {
                scaled = static_cast<u32>(displacement / 16);
                cursor.at = look.at;
                v128_anchor_pending = false;
                return Read(base, 16);
            }
            const u64 anchor = displacement & ~u64{0xfff0};
            if (!(displacement & 15) && anchor != 0 && (anchor >> 12) <= 4095u) {
                scaled = static_cast<u32>((displacement & 0xfff0u) / 16u);
                cursor.at = look.at;
                const bool full_u64 = View(base) == Type::U64 &&
                                      allocation.homes[base.Payload()].CarrierBits() == 64;
                if (full_u64 && v128_anchor_words == output.words.size() &&
                    v128_anchor_base == base.Payload() && v128_anchor_disp == anchor) {
                    v128_anchor_pending = true;
                    return 16;
                }
                const auto left = Read(base, 16);
                __ add(v::x16, X(left), Operand(anchor));
                if (full_u64) {
                    v128_anchor_base = static_cast<u32>(base.Payload());
                    v128_anchor_disp = anchor;
                    v128_anchor_pending = true;
                } else {
                    v128_anchor_pending = false;
                }
                return 16;
            }
        }
    }
    scaled = 0;
    v128_anchor_pending = false;
    return Expression(cursor, 16, 17);
}

unsigned Writer::VectorValue(CellView cell, unsigned scratch) {
    if (cell.Kind() != OperandKind::Value || cell.Payload() >= allocation.homes.size() ||
        source.Operations()[cell.Payload()].type != Type::V128)
        Reject(CompileError::UnsupportedType);
    const auto id = static_cast<u32>(cell.Payload());
    const auto& home = allocation.homes[id];
    if (!home.spilled) return home.reg;
    if (home.stack_offset == UINT32_MAX) {
        const auto& op = source.Operations()[id];
        if (op.opcode == OpCode::VecLoadConst &&
            source.Operands(NodeId{id}).front().Payload() == UINT64_MAX)
            __ cmeq(Lanes(scratch, 64), Lanes(scratch, 64), Lanes(scratch, 64));
        else
            __ eor(Lanes(scratch, 8), Lanes(scratch, 8), Lanes(scratch, 8));
        return scratch;
    }
    VectorMemory(true, scratch, 31, home.stack_offset / 16);
    return scratch;
}

}  // namespace detail
}  // namespace swift::runtime::backend::arm64

#undef __
