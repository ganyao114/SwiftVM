#pragma once

#include <array>
#include <cstdint>
#include "types.h"

namespace swift::runtime::ir {

enum class OpCode : std::uint16_t {
#define INST(name, ...) name,
#include "opcodes.inc"
#undef INST
    Count,
};

enum class Signature : std::uint8_t {
    Value,
    IntValue,
    Int32Value,
    Int64Value,
    Imm,
    IntImm,
    Uniform,
    Local,
    Cond,
    Flags,
    Operand,
    IntOperand,
    Lambda,
    Params,
    OptionalData,
    OptionalOperand,
    OptionalIntOperand,
};
enum class ResultPolicy : std::uint8_t {
    Void,
    Fixed,
    Arg0,
    Explicit,
    Call,
    IntegerFixed,
    IntegerArg0,
    IntegerExplicit,
};
// Guest-visible semantics, independent of register allocation or host forms.
// Unclassified operations remain conservative until their contract is defined.
enum class Effect : std::uint16_t {
    None = 0,
    ReadMemory = 1,
    WriteMemory = 2,
    ReadGuestState = 4,
    WriteGuestState = 8,
    ReadLocal = 16,
    WriteLocal = 32,
    ReadFlags = 64,
    WriteFlags = 128,
    ReadHostState = 256,
    WriteHostState = 512,
    MayFault = 1024,
    Ordered = 2048,
    Control = 4096,
    External = 8192,
    Location = 16384,
    Unknown = 32768,
};
constexpr Effect operator|(Effect left, Effect right) noexcept {
    return static_cast<Effect>(static_cast<unsigned>(left) | static_cast<unsigned>(right));
}
constexpr bool HasAnyEffect(Effect effects, Effect requested) noexcept {
    return (static_cast<unsigned>(effects) & static_cast<unsigned>(requested)) != 0;
}
struct Schema {
    ResultPolicy result;
    Type fixed_type;
    Effect effects;
    std::uint8_t argument_count;
    std::array<Signature, 5> arguments;
};

namespace detail {
template <class... Args>
constexpr Schema MakeSchema(ResultPolicy result, Type type, Effect effects, Args... args) noexcept {
    static_assert(sizeof...(args) <= 5);
    return {result, type, effects, sizeof...(args), {args...}};
}
}  // namespace detail

#define IR_TYPE_Void Type::Void
#define IR_TYPE_Value Type::Void
#define IR_TYPE_BOOL Type::Bool
#define IR_TYPE_U32 Type::U32
#define IR_TYPE_U64 Type::U64
#define Value Signature::Value
#define IntValue Signature::IntValue
#define Int32Value Signature::Int32Value
#define Int64Value Signature::Int64Value
#define Imm Signature::Imm
#define IntImm Signature::IntImm
#define Uniform Signature::Uniform
#define Local Signature::Local
#define Cond Signature::Cond
#define Flags Signature::Flags
#define Operand Signature::Operand
#define IntOperand Signature::IntOperand
#define Lambda Signature::Lambda
#define Params Signature::Params
#define OptionalData Signature::OptionalData
#define OptionalOperand Signature::OptionalOperand
#define OptionalIntOperand Signature::OptionalIntOperand
#define INST(name, ret, policy, effects, ...)                                                      \
    detail::MakeSchema(ResultPolicy::policy, IR_TYPE_##ret, effects __VA_OPT__(, ) __VA_ARGS__),
inline constexpr std::array<Schema, static_cast<std::size_t>(OpCode::Count)> schemas{{
#include "opcodes.inc"
}};
#undef INST
#undef OptionalIntOperand
#undef OptionalOperand
#undef OptionalData
#undef Params
#undef Lambda
#undef IntOperand
#undef Operand
#undef Flags
#undef Cond
#undef Local
#undef Uniform
#undef IntImm
#undef Imm
#undef Int64Value
#undef Int32Value
#undef IntValue
#undef Value
#undef IR_TYPE_U64
#undef IR_TYPE_U32
#undef IR_TYPE_BOOL
#undef IR_TYPE_Value
#undef IR_TYPE_Void

constexpr const Schema* SchemaFor(OpCode op) noexcept {
    const auto index = static_cast<std::size_t>(op);
    return index < schemas.size() ? &schemas[index] : nullptr;
}

// No graph walk or per-node effects table. Dynamic flag production is part
// of the actual instruction metadata. Opaque helper traits do not grant purity.
constexpr Effect EffectsFor(OpCode op, OperationMetadata metadata = {}) noexcept {
    const auto* schema = SchemaFor(op);
    auto effects = schema ? schema->effects : Effect::Unknown;
    if (HasAnyEffect(effects, Effect::Unknown)) effects = static_cast<Effect>(UINT16_MAX);
    if (metadata.flag_outputs) effects = effects | Effect::WriteFlags;
    return effects;
}
constexpr bool IsPure(Effect effects) noexcept { return effects == Effect::None; }

}  // namespace swift::runtime::ir
