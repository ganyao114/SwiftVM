#pragma once

#include <optional>
#include "runtime/ir/args.h"
#include "runtime/ir/opcodes.h"

namespace swift::runtime::compile {

// Facts about the bits of one integer Value view. Bits outside Width() are
// unspecified here: these facts never certify the high half of a physical X
// register, a guest home, or a coalesced alias.
class IntegerValueBits final {
public:
    [[nodiscard]] static IntegerValueBits Unknown(u32 width);
    [[nodiscard]] static IntegerValueBits Constant(u32 width, u64 value);
    [[nodiscard]] u32 Width() const { return width; }
    [[nodiscard]] u64 KnownZero() const { return zero; }
    [[nodiscard]] u64 KnownOne() const { return one; }
    // Zero means no useful sign-copy span. Otherwise all bits above this
    // signed width repeat its sign bit, through Width() only.
    [[nodiscard]] u32 SignExtendedFrom() const { return sign_from; }
    [[nodiscard]] std::optional<u64> Constant() const;

private:
    friend class IntegerValueAnalysis;
    friend std::optional<IntegerValueBits> ApplyIntegerWidth(
            ir::OpCode, ir::ValueType, IntegerValueBits, u64, u64);
    friend std::optional<IntegerValueBits> ApplyIntegerPredicate(
            ir::IntegerPredicateKind, ir::ValueType, IntegerValueBits, u64);
    IntegerValueBits(u32 width, u64 zero, u64 one, u32 sign_from = 0);
    u64 zero{}, one{};
    u8 width{}, sign_from{};
};

// A bounded, allocation-free value query. It builds no consumer graph or
// cache; malformed inputs, cycles and budget exhaustion return Width() == 0.
inline constexpr u32 kIntegerValueVisitLimit = 32;
// Rewrites must propagate changed inputs through the same operations that the
// query understands. This is independent of which nodes have a local fold.
[[nodiscard]] constexpr bool TransfersIntegerValueBits(ir::OpCode op) {
    if (ir::ClassifyIntegerShift(op) != ir::IntegerShiftKind::None) return true;
    using O = ir::OpCode;
    switch (op) {
        case O::BitCast:
        case O::ZeroExtend32:
        case O::ZeroExtend32To64:
        case O::ZeroExtend64:
        case O::SignExtend:
        case O::BitExtract:
        case O::And:
        case O::Or:
        case O::Xor:
        case O::AndNot:
        case O::Add:
        case O::Sub:
        case O::Neg:
        case O::Not:
        case O::TestZero:
        case O::TestNotZero:
        case O::TestBit:
        case O::Select:
        case O::SelectZero:
            return true;
        default:
            return false;
    }
}
[[nodiscard]] IntegerValueBits AnalyzeIntegerValue(ir::Value);
[[nodiscard]] std::optional<IntegerValueBits> ApplyIntegerWidth(
        ir::OpCode, ir::ValueType result, IntegerValueBits input, u64 left = 0, u64 bits = 0);
[[nodiscard]] std::optional<IntegerValueBits> ApplyIntegerPredicate(
        ir::IntegerPredicateKind, ir::ValueType result, IntegerValueBits input, u64 bit = 0);

}  // namespace swift::runtime::compile
