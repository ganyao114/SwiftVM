#include "emit.h"

namespace swift::runtime::backend::arm64 {
std::uintptr_t DivisionHelper(unsigned, bool) noexcept;
std::uintptr_t CpuidHelper() noexcept;
std::uintptr_t X87Helper() noexcept;
namespace detail {
unsigned Writer::Expression(Cursor& cursor, unsigned first, unsigned second, unsigned native_width) {
    const auto input = cursor.Take();
    if (input.Kind() != OperandKind::Expression) return Read(input, first, 64, false, native_width);
    if (input.Count() != 2) Reject(CompileError::UnsupportedOperand);
    const auto expression = input.ExpressionOp();
    if ((expression != OperandOp::Add && expression != OperandOp::Sub && expression != OperandOp::AddShift) ||
        (input.ShiftAmount() && expression != OperandOp::AddShift) || input.ShiftAmount() >= 64)
        Reject(CompileError::UnsupportedOperand);
    // A W consumer needs only the expression's low 32 bits. Large shifts
    // retain the X expression and let that consumer take its ordinary view.
    if (input.ShiftAmount() >= 32) native_width = 64;
    const auto base = cursor.Take(), operand = cursor.Take();
    if ((expression == OperandOp::Add || expression == OperandOp::Sub) && operand.Kind() == OperandKind::Immediate) {
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
            if (subtract) masm.exact.sub(R(first, native_width), R(left, native_width), Operand(literal));
            else masm.exact.add(R(first, native_width), R(left, native_width), Operand(literal));
            return first;
        }
    }
    if (native_width == 64 && operand.Kind() == OperandKind::Value && input.ShiftAmount() <= 4) {
        const auto bits = std::min(Width(View(operand)), allocation.homes[operand.Payload()].CarrierBits());
        if (bits == 32) {
            const auto left = Read(base, first), right = RawValue(static_cast<u32>(operand.Payload()), second, true);
            // The actual W index performs the logical truncation. Do not
            // materialize a zero-extended X temporary before address ADD.
            const Operand shifted_index{W(right), v::UXTW, input.ShiftAmount()};
            if (expression == OperandOp::Sub) masm.exact.sub(X(first), X(left), shifted_index);
            else masm.exact.add(X(first), X(left), shifted_index);
            return first;
        }
    }
    const auto left = Read(base, first, 64, false, native_width),
            right = Read(operand, second, 64, false, native_width);
    const Operand shifted_index{R(right, native_width), v::LSL, input.ShiftAmount()};
    if (expression == OperandOp::Sub) masm.exact.sub(R(first, native_width), R(left, native_width), shifted_index);
    else masm.exact.add(R(first, native_width), R(left, native_width), shifted_index);
    return first;
}

unsigned Writer::OrderedAddress(Cursor& cursor, std::int32_t& offset, bool store) {
    if (!store && !RcpcImm9()) return Expression(cursor, 16, 17);
    auto probe = cursor;
    const auto expression = probe.Take();
    if (expression.Kind() != OperandKind::Expression || expression.Count() != 2 || expression.ShiftAmount() ||
            (expression.ExpressionOp() != OperandOp::Add && expression.ExpressionOp() != OperandOp::Sub))
        return Expression(cursor, 16, 17);
    const auto base = probe.Take(), literal = probe.Take();
    if (base.Kind() != OperandKind::Value || literal.Kind() != OperandKind::Immediate || literal.TypeView() != Type::U64)
        return Expression(cursor, 16, 17);
    auto displacement = literal.Payload();
    if (expression.ExpressionOp() == OperandOp::Sub) displacement = u64{} - displacement;
    const auto end = probe.at;
    if (store) {
        const auto value = probe.Take();
        if (value.Kind() == OperandKind::Value && source.Operations()[value.Payload()].type == Type::V128) {
            // STR Q uses an unsigned imm12 scaled by 16, independently of RCpc.
            if (displacement > 4095u * 16u || (displacement & 15)) return Expression(cursor, 16, 17);
            offset = static_cast<std::int32_t>(displacement); // Keep the shared output in bytes.
            cursor.at = end;
            return Read(base, 16);
        }
    }
    if (!RcpcImm9() || (displacement > 255 && displacement < u64{} - 256)) return Expression(cursor, 16, 17);
    offset = displacement <= 255 ? static_cast<std::int32_t>(displacement) :
            -static_cast<std::int32_t>(u64{} - displacement);
    cursor.at = end;
    return Read(base, 16);
}

unsigned Writer::VectorValue(CellView cell, unsigned scratch) {
    if (cell.Kind() != OperandKind::Value || cell.Payload() >= allocation.homes.size() ||
        source.Operations()[cell.Payload()].type != Type::V128)
        Reject(CompileError::UnsupportedType);
    const auto& home = allocation.homes[cell.Payload()];
    if (!home.spilled) return home.reg;
    VectorMemory(true, scratch, 31, home.stack_offset / 16); return scratch;
}

} // namespace detail
} // namespace swift::runtime::backend::arm64
