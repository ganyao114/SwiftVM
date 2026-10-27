#include "value_bits.h"

#include <algorithm>
#include <array>
#include <bit>
#include "runtime/ir/instr.h"

namespace swift::runtime::compile {
namespace {
u64 Mask(u32 width) { return width >= 64 ? UINT64_MAX : width ? (u64{1} << width) - 1 : 0; }
// Each input uses bit 0/1 to name its possible Boolean values. The low pair
// describes the sum bit, the high pair the carry. Build the full-adder truth
// table at compile time, with no per-query allocation or initialization.
constexpr auto kAdderTransitions = [] {
    std::array<u8, 64> table{};
    for (u32 a = 1; a < 4; ++a)
        for (u32 b = 1; b < 4; ++b)
            for (u32 carry = 1; carry < 4; ++carry)
                for (u32 values = 0; values < 8; ++values) {
                    const auto x = values & 1, y = (values >> 1) & 1, c = values >> 2;
                    if (!(a & (1u << x)) || !(b & (1u << y)) || !(carry & (1u << c)))
                        continue;
                    const auto sum = x + y + c;
                    table[a | (b << 2) | (carry << 4)] |=
                            (1u << (sum & 1)) | (1u << (2 + (sum >> 1)));
                }
    return table;
}();
}  // namespace

IntegerValueBits::IntegerValueBits(u32 bits, u64 zeros, u64 ones, u32 sign)
        : zero(zeros & Mask(bits)), one(ones & Mask(bits)), width(bits) {
    ASSERT(bits <= 64 && !(zero & one));
    sign_from = sign && sign < bits ? sign : 0;
    if (!bits) return;
    const u64 sign_bit = u64{1} << (bits - 1);
    const auto same = zero & sign_bit ? zero : one & sign_bit ? one : 0;
    if (!same) return;
    const auto leading = std::countl_one(same << (64 - bits));
    const u32 implied = bits - leading + 1;
    if (implied < bits && (!sign_from || implied < sign_from)) sign_from = implied;
}
IntegerValueBits IntegerValueBits::Unknown(u32 width) { return {width, 0, 0}; }
IntegerValueBits IntegerValueBits::Constant(u32 width, u64 value) { return {width, ~value, value}; }
std::optional<u64> IntegerValueBits::Constant() const {
    if (!width || (zero | one) != Mask(width)) return std::nullopt;
    return one;
}

std::optional<IntegerValueBits> ApplyIntegerWidth(
        ir::OpCode op, ir::ValueType result, IntegerValueBits input, u64 left, u64 bits) {
    const auto width = ir::GetScalarIntegerBits(result);
    if (!width || !input.width) return std::nullopt;
    using O = ir::OpCode;
    switch (op) {
        case O::ZeroExtend32:
        case O::ZeroExtend32To64:
        case O::ZeroExtend64: {
            const u32 copied = ir::IntegerConversionInputBits(op, input.width);
            return IntegerValueBits{
                    width, (input.zero & Mask(copied)) | ~Mask(copied), input.one & Mask(copied)};
        }
        case O::SignExtend: {
            u64 zero = input.zero, one = input.one;
            if (width > input.width) {
                const u64 sign = u64{1} << (input.width - 1);
                if (zero & sign) zero |= ~Mask(input.width);
                if (one & sign) one |= ~Mask(input.width);
            }
            const u32 sign = input.sign_from ? input.sign_from : input.width;
            return IntegerValueBits{width, zero, one, sign};
        }
        case O::BitExtract: {
            if (left >= 64 || !bits || bits > 64) return std::nullopt;
            const u32 available = left < input.width ? input.width - left : 0;
            const u32 copied = std::min<u64>(bits, available);
            u32 sign = 0;
            if (bits >= width && available >= width && input.sign_from &&
                input.sign_from <= left + width)
                sign = input.sign_from > left ? input.sign_from - left : 1;
            return IntegerValueBits{width,
                                    ((input.zero >> left) & Mask(copied)) | ~Mask(copied),
                                    (input.one >> left) & Mask(copied),
                                    sign};
        }
        default:
            return std::nullopt;
    }
}

std::optional<IntegerValueBits> ApplyIntegerPredicate(
        ir::IntegerPredicateKind kind, ir::ValueType result, IntegerValueBits input, u64 bit) {
    const u32 width = ir::GetScalarIntegerBits(result);
    if (!width || !input.width || kind == ir::IntegerPredicateKind::None) return std::nullopt;
    std::optional<bool> value;
    if (kind == ir::IntegerPredicateKind::Bit) {
        if (bit >= 64) return std::nullopt;
        if (bit >= input.width || (input.zero & (u64{1} << bit))) value = false;
        else if (input.one & (u64{1} << bit)) value = true;
    } else {
        if (input.one) value = kind == ir::IntegerPredicateKind::Nonzero;
        else if (input.zero == Mask(input.width)) value = kind == ir::IntegerPredicateKind::Zero;
    }
    if (value) return IntegerValueBits::Constant(width, *value);
    return IntegerValueBits{width, Mask(width) & ~u64{1}, 0};
}

class IntegerValueAnalysis final {
public:
    IntegerValueBits Run(ir::Value value) {
        const auto facts = Query(value);
        return exhausted || invalid ? IntegerValueBits::Unknown(0) : facts;
    }

private:
    IntegerValueBits View(IntegerValueBits facts, u32 width) {
        // A widening view is not a zero extension. Preserve only facts about
        // existing bits, and discard sign-copy claims across the new upper bits.
        return {width, facts.zero, facts.one, width <= facts.width ? facts.sign_from : 0u};
    }
    IntegerValueBits Query(ir::Value value) {
        const auto* inst = value.Def();
        if (!inst || !inst->HasValue()) {
            invalid = true;
            return IntegerValueBits::Unknown(0);
        }
        const auto width = ir::GetScalarIntegerBits(value.Type());
        if (!width) {
            invalid = true;
            return IntegerValueBits::Unknown(0);
        }
        if (!remaining) {
            exhausted = true;
            return IntegerValueBits::Unknown(width);
        }
        --remaining;
        // Literal bit patterns use the requested operand view, as width
        // folding did before this analysis. No physical-register fact is read.
        if (inst->GetOp() == ir::OpCode::LoadImm) {
            if (!inst->ArgAt(0).IsImm()) {
                invalid = true;
                return IntegerValueBits::Unknown(0);
            }
            return IntegerValueBits::Constant(width, inst->GetArg<ir::Imm>(0).Get());
        }
        const auto defined_width = ir::GetScalarIntegerBits(inst->ReturnType());
        if (!defined_width) return IntegerValueBits::Unknown(width);
        return View(Definition(*inst, defined_width), width);
    }
    IntegerValueBits Add(IntegerValueBits left, IntegerValueBits right, bool subtract) {
        const auto width = left.Width();
        if (const auto b = right.Constant(); b && !*b) return left;
        if (const auto a = left.Constant()) {
            if (const auto b = right.Constant())
                return IntegerValueBits::Constant(width, subtract ? *a - *b : *a + *b);
            if (!*a && !subtract) return right;
        }
        if (subtract) std::swap(right.zero, right.one);
        u8 carry = subtract ? 2 : 1;
        u64 zero = 0, one = 0;
        for (u32 bit = 0; bit < width; ++bit) {
            const auto mask = u64{1} << bit;
            const u32 a = (left.zero & mask) ? 1 : (left.one & mask) ? 2 : 3;
            const u32 b = (right.zero & mask) ? 1 : (right.one & mask) ? 2 : 3;
            const auto state = kAdderTransitions[a | (b << 2) | (carry << 4)];
            if ((state & 3) == 1) zero |= mask;
            if ((state & 3) == 2) one |= mask;
            carry = state >> 2;
        }
        return {width, zero, one};
    }
    IntegerValueBits Shift(const ir::Inst& inst, u32 width, ir::IntegerShiftKind kind) {
        if (!inst.ArgAt(0).IsValue()) {
            invalid = true;
            return IntegerValueBits::Unknown(width);
        }
        const auto input = Query(inst.GetArg<ir::Value>(0));
        const u32 copied = ir::IntegerShiftInputBits(input.width, width);
        if (!copied) {
            invalid = true;
            return IntegerValueBits::Unknown(width);
        }
        u64 zero = input.zero & Mask(copied), one = input.one & Mask(copied);
        u32 sign_from = 0;
        if (kind == ir::IntegerShiftKind::ArithmeticRight) {
            const auto sign = u64{1} << (copied - 1);
            if (zero & sign) zero |= ~Mask(copied);
            if (one & sign) one |= ~Mask(copied);
            sign_from = input.sign_from ? std::min<u32>(input.sign_from, copied) : copied;
        } else {
            zero |= ~Mask(copied);
            if (copied == width && input.sign_from)
                sign_from = std::min<u32>(input.sign_from, copied);
        }
        const IntegerValueBits operand{width, zero, one, sign_from};
        std::optional<u32> amount;
        if (inst.ArgAt(1).IsImm()) {
            const auto immediate = inst.GetArg<ir::Imm>(1);
            if (!ir::IsScalarIntegerType(immediate.GetType())) {
                invalid = true;
                return IntegerValueBits::Unknown(width);
            }
            amount = ir::NormalizeIntegerShiftCount(immediate.Get(), width);
        } else if (inst.ArgAt(1).IsValue()) {
            const auto count = Query(inst.GetArg<ir::Value>(1));
            const u32 mask = width - 1;
            if (((count.zero | count.one) & mask) == mask)
                amount = ir::NormalizeIntegerShiftCount(count.one, width);
        } else {
            invalid = true;
            return IntegerValueBits::Unknown(width);
        }
        if (!amount) {
            if (const auto value = operand.Constant(); value &&
                (!*value || ((kind == ir::IntegerShiftKind::ArithmeticRight ||
                               kind == ir::IntegerShiftKind::RotateRight) && *value == Mask(width))))
                return operand;
            return IntegerValueBits::Unknown(width);
        }
        if (!*amount) return operand;
        if (kind == ir::IntegerShiftKind::Left)
            return {width, (operand.zero << *amount) | Mask(*amount),
                    operand.one << *amount,
                    operand.sign_from ? operand.sign_from + *amount : 0};
        const auto high = Mask(width) & ~Mask(width - *amount);
        if (kind == ir::IntegerShiftKind::Right)
            return {width, (operand.zero >> *amount) | high, operand.one >> *amount};
        if (kind == ir::IntegerShiftKind::ArithmeticRight) {
            const auto sign = u64{1} << (width - 1);
            const auto from = operand.sign_from ? operand.sign_from : width;
            return {width, (operand.zero >> *amount) | ((operand.zero & sign) ? high : 0),
                    (operand.one >> *amount) | ((operand.one & sign) ? high : 0),
                    from > *amount ? from - *amount : 1};
        }
        return {width, (operand.zero >> *amount) | (operand.zero << (width - *amount)),
                (operand.one >> *amount) | (operand.one << (width - *amount))};
    }
    IntegerValueBits Definition(const ir::Inst& inst, u32 width) {
        using O = ir::OpCode;
        const auto op = inst.GetOp();
        if (!TransfersIntegerValueBits(op)) return IntegerValueBits::Unknown(width);
        if (const auto kind = inst.GetIntegerPredicateKind(); kind != ir::IntegerPredicateKind::None) {
            if (!inst.ArgAt(0).IsValue() || (kind == ir::IntegerPredicateKind::Bit &&
                (!inst.ArgAt(1).IsImm() ||
                 !ir::IsScalarIntegerType(inst.GetArg<ir::Imm>(1).GetType())))) {
                invalid = true;
                return IntegerValueBits::Unknown(width);
            }
            const auto facts = ApplyIntegerPredicate(kind, inst.ReturnType(),
                    Query(inst.GetArg<ir::Value>(0)),
                    kind == ir::IntegerPredicateKind::Bit ? inst.GetArg<ir::Imm>(1).Get() : 0);
            if (!facts) {
                invalid = true;
                return IntegerValueBits::Unknown(width);
            }
            return *facts;
        }
        // The optional Operand form complements bits and has a separate
        // contract. It still carries rewrite notifications through the graph.
        if (op == O::Not) return IntegerValueBits::Unknown(width);
        if (op == O::Select || op == O::SelectZero) {
            if (!inst.ArgAt(0).IsValue() || !inst.ArgAt(1).IsValue() || !inst.ArgAt(2).IsValue()) {
                invalid = true;
                return IntegerValueBits::Unknown(width);
            }
            if (!ir::IsScalarIntegerType(inst.GetArg<ir::Value>(1).Type()) ||
                !ir::IsScalarIntegerType(inst.GetArg<ir::Value>(2).Type()))
                return IntegerValueBits::Unknown(width);
            const auto condition = Query(inst.GetArg<ir::Value>(0));
            std::optional<bool> zero;
            if (condition.one) zero = false;
            else if (condition.width && condition.zero == Mask(condition.width)) zero = true;
            if (zero) {
                const bool first = op == O::SelectZero ? *zero : !*zero;
                return View(Query(inst.GetArg<ir::Value>(first ? 1 : 2)), width);
            }
            const auto first = View(Query(inst.GetArg<ir::Value>(1)), width);
            const auto second = View(Query(inst.GetArg<ir::Value>(2)), width);
            return {width, first.zero & second.zero, first.one & second.one};
        }
        if (op == O::BitCast) {
            if (!inst.ArgAt(0).IsValue()) {
                invalid = true;
                return IntegerValueBits::Unknown(width);
            }
            return View(Query(inst.GetArg<ir::Value>(0)), width);
        }
        if (op == O::ZeroExtend32 || op == O::ZeroExtend32To64 || op == O::ZeroExtend64 ||
            op == O::SignExtend || op == O::BitExtract) {
            if (!inst.ArgAt(0).IsValue() ||
                (op == O::BitExtract && (!inst.ArgAt(1).IsImm() || !inst.ArgAt(2).IsImm()))) {
                invalid = true;
                return IntegerValueBits::Unknown(width);
            }
            const auto input = Query(inst.GetArg<ir::Value>(0));
            const auto facts =
                    ApplyIntegerWidth(op,
                                      inst.ReturnType(),
                                      input,
                                      op == O::BitExtract ? inst.GetArg<ir::Imm>(1).Get() : 0,
                                      op == O::BitExtract ? inst.GetArg<ir::Imm>(2).Get() : 0);
            if (!facts) {
                invalid = true;
                return IntegerValueBits::Unknown(width);
            }
            return *facts;
        }
        if (const auto kind = ir::ClassifyIntegerShift(op); kind != ir::IntegerShiftKind::None)
            return Shift(inst, width, kind);
        if (op == O::Neg) {
            if (!inst.ArgAt(0).IsValue()) {
                invalid = true;
                return IntegerValueBits::Unknown(width);
            }
            return Add(IntegerValueBits::Constant(width, 0),
                       View(Query(inst.GetArg<ir::Value>(0)), width),
                       true);
        }
        if (!inst.ArgAt(0).IsValue() || !inst.ArgAt(1).IsOperand()) {
            invalid = true;
            return IntegerValueBits::Unknown(width);
        }
        const auto operand = inst.GetArg<ir::Operand>(1);
        if (operand.GetOp() != ir::OperandPlus || !operand.GetRight().Null())
            return IntegerValueBits::Unknown(width);
        const auto first = inst.GetArg<ir::Value>(0);
        const auto left = View(Query(first), width);
        IntegerValueBits right = IntegerValueBits::Unknown(width);
        const auto second = operand.GetLeft();
        if (second.IsImm())
            right = IntegerValueBits::Constant(width, second.imm.Get());
        else if (second.IsValue()) {
            if ((op == O::Xor || op == O::Sub || op == O::AndNot) && first.Defined() &&
                second.value.Defined() &&
                first.Def() == second.value.Def() && first.Type() == second.value.Type())
                return IntegerValueBits::Constant(width, 0);
            right = View(Query(second.value), width);
        } else {
            invalid = true;
            return IntegerValueBits::Unknown(width);
        }
        if (op == O::And) return {width, left.zero | right.zero, left.one & right.one};
        if (op == O::Or) return {width, left.zero & right.zero, left.one | right.one};
        if (op == O::AndNot) return {width, left.zero | right.one, left.one & right.zero};
        if (op == O::Add || op == O::Sub) return Add(left, right, op == O::Sub);
        return {width,
                (left.zero & right.zero) | (left.one & right.one),
                (left.zero & right.one) | (left.one & right.zero)};
    }
    u32 remaining{kIntegerValueVisitLimit};
    bool exhausted{}, invalid{};
};

IntegerValueBits AnalyzeIntegerValue(ir::Value value) { return IntegerValueAnalysis{}.Run(value); }

}  // namespace swift::runtime::compile
