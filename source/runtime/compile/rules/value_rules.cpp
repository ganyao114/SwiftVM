#include "value_rules.h"

#include "runtime/compile/analysis/value_bits.h"
#include "runtime/ir/instr.h"

namespace swift::runtime::compile {
namespace {
u64 WidthMask(u32 bytes) {
    return bytes >= sizeof(u64) ? UINT64_MAX : (u64{1} << (bytes * 8)) - 1;
}
ir::Imm ResultImmediate(u64 bits, u32 bytes) {
    return bytes <= sizeof(u32) ? ir::Imm{static_cast<u32>(bits)} : ir::Imm{bits};
}
}  // namespace

std::optional<ir::Imm> FoldIntegerImmediate(
        ir::OpCode op, ir::ValueType result, const ir::Imm& source,
        FlagsIntent flags, const BuilderCapabilities& target) {
    if (!ir::IsScalarIntegerType(result)) return std::nullopt;
    const u32 bytes = ir::GetValueSizeByte(result);
    const u32 reg_bits = bytes <= sizeof(u32) ? 32 : 64;
    const u64 value = source.Get() & WidthMask(bytes);
    if (op == ir::OpCode::Add || op == ir::OpCode::Sub) {
        if (!target.add_sub_immediate ||
            (bytes <= sizeof(u16) && flags.MayRequire(ir::Flags::NZCV))) return std::nullopt;
        if (target.add_sub_immediate(value)) return ResultImmediate(value, bytes);
        const s64 signed_value = reg_bits == 32
                ? static_cast<s32>(static_cast<u32>(value)) : static_cast<s64>(value);
        if (signed_value < 0 && signed_value != INT64_MIN &&
            target.add_sub_immediate(static_cast<u64>(-signed_value))) {
            return reg_bits == 32 ? ir::Imm{static_cast<s32>(signed_value)}
                                  : ir::Imm{signed_value};
        }
    } else if ((op == ir::OpCode::And || op == ir::OpCode::Or || op == ir::OpCode::Xor) &&
               target.logical_immediate && target.logical_immediate(value, reg_bits)) {
        return ResultImmediate(value, bytes);
    }
    return std::nullopt;
}

std::optional<ir::Imm> FoldWidthValue(ir::OpCode op,
                                      ir::ValueType result,
                                      ir::Value input,
                                      FlagsIntent flags,
                                      u64 left,
                                      u64 bits) {
    if (!flags.HasNoOutputs() || !ir::IsScalarIntegerType(result) || !input.Defined()) return std::nullopt;
    const auto facts = ApplyIntegerWidth(op, result, AnalyzeIntegerValue(input), left, bits);
    if (!facts) return std::nullopt;
    const auto value = facts->Constant();
    if (!value) return std::nullopt;
    return ResultImmediate(*value, ir::GetValueSizeByte(result));
}

std::optional<ir::Imm> FoldConditionalValue(const ir::Inst& inst, FlagsIntent flags) {
    if (!flags.HasNoOutputs() || !IsConditionalValueOperation(inst.GetOp()) ||
        !ir::IsScalarIntegerType(inst.ReturnType())) return std::nullopt;
    const auto facts = AnalyzeIntegerValue(ir::Value{&inst});
    const auto value = facts.Constant();
    // Do not let a constant outcome erase a malformed, unused condition or
    // arm. The same schema/domain check owns admission and this final rewrite.
    if (!value || !inst.HasValidArguments()) return std::nullopt;
    return ResultImmediate(*value, ir::GetValueSizeByte(inst.ReturnType()));
}

std::optional<ir::Value> FoldSameTypeCast(
        ir::ValueType result, ir::Value input, FlagsIntent flags) {
    if (!flags.HasNoOutputs() || !input.Defined() || result == ir::ValueType::VOID ||
        result != input.Type() || result != input.Def()->ReturnType()) return std::nullopt;
    return input.SetCastType(result);
}

std::optional<ir::Value> ZeroMinusOperand(ir::Value left, ir::Operand right) {
    const auto* zero = left.Def();
    if (!zero || zero->GetOp() != ir::OpCode::LoadImm ||
        zero->GetArg<ir::Imm>(0).Get() != 0 || !right.GetRight().Null() ||
        !right.GetLeft().IsValue() || right.GetOp() != ir::OperandPlus) return std::nullopt;
    return right.GetLeft().value;
}

}  // namespace swift::runtime::compile
