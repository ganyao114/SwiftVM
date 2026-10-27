#pragma once

#include <optional>

#include "runtime/ir/args.h"
#include "runtime/ir/opcodes.h"

namespace swift::runtime::compile {
// Unknown preserves flags unless the caller supplies an explicit output contract.
// Declared describes outputs promised by the caller, rather than predicting
// which flags a later pass will prove dead.
class FlagsIntent final {
public:
    enum class Kind : u8 { None, Declared, Unknown };
    static constexpr FlagsIntent None() { return {Kind::None, ir::Flags::None}; }
    static constexpr FlagsIntent Declared(ir::Flags mask) { return {Kind::Declared, mask}; }
    static constexpr FlagsIntent Unknown() { return {Kind::Unknown, ir::Flags::None}; }
    [[nodiscard]] constexpr bool IsUnknown() const { return kind == Kind::Unknown; }
    [[nodiscard]] constexpr bool HasNoOutputs() const {
        return kind != Kind::Unknown && mask == ir::Flags::None;
    }
    [[nodiscard]] constexpr bool MayRequire(ir::Flags flags) const {
        return IsUnknown() || True(mask & flags);
    }
    [[nodiscard]] constexpr bool Allows(ir::Flags outputs) const {
        return IsUnknown() || !True(outputs & ~mask);
    }

private:
    constexpr FlagsIntent(Kind kind, ir::Flags mask) : kind(kind), mask(mask) {}
    Kind kind;
    ir::Flags mask;
};

struct BuilderCapabilities {
    bool (*add_sub_immediate)(u64){};
    bool (*logical_immediate)(u64, u32){};
    bool fixed_guest_state{};
};

inline constexpr u32 kIntegerImmediateUseWindow = 5;

[[nodiscard]] constexpr bool IsWidthOperation(ir::OpCode op) {
    return op == ir::OpCode::ZeroExtend32 || op == ir::OpCode::ZeroExtend32To64 ||
           op == ir::OpCode::ZeroExtend64 || op == ir::OpCode::SignExtend ||
           op == ir::OpCode::BitExtract;
}

[[nodiscard]] constexpr bool IsConditionalValueOperation(ir::OpCode op) {
    return op == ir::OpCode::TestZero || op == ir::OpCode::TestNotZero ||
           op == ir::OpCode::TestBit || op == ir::OpCode::Not ||
           op == ir::OpCode::Select || op == ir::OpCode::SelectZero;
}

[[nodiscard]] std::optional<ir::Imm> FoldIntegerImmediate(
        ir::OpCode op, ir::ValueType result, const ir::Imm& source,
        FlagsIntent flags, const BuilderCapabilities& target);
[[nodiscard]] std::optional<ir::Imm> FoldWidthValue(ir::OpCode op,
                                                    ir::ValueType result,
                                                    ir::Value input,
                                                    FlagsIntent flags,
                                                    u64 left = 0,
                                                    u64 bits = 0);
[[nodiscard]] std::optional<ir::Imm> FoldConditionalValue(const ir::Inst&, FlagsIntent flags);
[[nodiscard]] std::optional<ir::Value> FoldSameTypeCast(
        ir::ValueType result, ir::Value input, FlagsIntent flags);
[[nodiscard]] std::optional<ir::Value> ZeroMinusOperand(ir::Value left, ir::Operand right);

}  // namespace swift::runtime::compile
