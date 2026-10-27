//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include <array>
#include <span>
#include <type_traits>
#include "runtime/common/types.h"
#include "runtime/ir/opcodes.h"
#include "base/common_funcs.h"
#include "fmt/format.h"

namespace swift::runtime::ir {

#define ARG_TYPE_ENUM(X) \
    X(Void)     \
    X(Value)    \
    X(Imm)      \
    X(Uniform)  \
    X(Local)    \
    X(Cond)     \
    X(Flags)    \
    X(Operand)  \
    X(Lambda)   \
    X(Params)

#define VALUE_TYPE_ENUM(X) \
    X(VOID) \
    X(U8)   \
    X(U16)  \
    X(U32)  \
    X(U64)  \
    X(S8)   \
    X(S16)  \
    X(S32)  \
    X(S64)  \
    X(V8)   \
    X(V16)  \
    X(V32)  \
    X(V64)  \
    X(V128) \
    X(V256)

enum class ArgType : u8 { ARG_TYPE_ENUM(ENUM_DEFINE) };

// Physical slot width per arg kind. Operand = op+left+right. Change here and only here.
constexpr u8 PhysicalSlots(ArgType t) { return t == ArgType::Operand ? 3 : 1; }

// A signature describes accepted storage, not a new physical Arg kind.
// Optional arguments form a contiguous suffix. Host-call data accepts either
// a Value or an Imm; an optional Operand still occupies all three slots.
enum class ArgSignature : u8 {
#define ARG_SIGNATURE(name) name = static_cast<u8>(ArgType::name),
    ARG_TYPE_ENUM(ARG_SIGNATURE)
#undef ARG_SIGNATURE
    OptionalData = 0x80 | static_cast<u8>(ArgType::Value),
    OptionalOperand = 0x80 | static_cast<u8>(ArgType::Operand),
};

constexpr ArgType StorageType(ArgSignature signature) {
    return static_cast<ArgType>(static_cast<u8>(signature) & 0x7f);
}

constexpr bool IsOptional(ArgSignature signature) {
    return (static_cast<u8>(signature) & 0x80) != 0;
}

constexpr bool AcceptsArgument(ArgSignature signature, ArgType actual) {
    return actual == StorageType(signature) ||
           (signature == ArgSignature::OptionalData && actual == ArgType::Imm);
}

constexpr u8 PhysicalSlots(ArgSignature signature) {
    return PhysicalSlots(StorageType(signature));
}

static_assert(sizeof(ArgSignature) == sizeof(ArgType));

enum class ValueType : u8 { VALUE_TYPE_ENUM(ENUM_DEFINE) };

DECLARE_ENUM_FLAG_OPERATORS(ValueType)

// VOID denotes the absence of a result, never a readable value view.
constexpr bool IsValidValueType(ValueType type) {
    return type > ValueType::VOID && type <= ValueType::V256;
}

constexpr bool IsScalarIntegerType(ValueType type) {
    return type >= ValueType::U8 && type <= ValueType::S64;
}

constexpr u32 GetScalarIntegerBits(ValueType type) {
    switch (type) {
        case ValueType::U8: case ValueType::S8: return 8;
        case ValueType::U16: case ValueType::S16: return 16;
        case ValueType::U32: case ValueType::S32: return 32;
        case ValueType::U64: case ValueType::S64: return 64;
        default: return 0;
    }
}

constexpr bool IsScalarIntegerType(ValueType type, u32 bits) {
    return bits && GetScalarIntegerBits(type) == bits;
}

constexpr bool IsScalarBitWidth(u64 bits) {
    return bits == 8 || bits == 16 || bits == 32 || bits == 64;
}

// Conversion reads observe the input view, even if its raw home contains
// wider bits. The 32-bit forms also truncate a wider view before extending.
// Result width is a separate contract; raw BitCast/GetResult never use this.
constexpr u32 IntegerConversionInputBits(OpCode op, u32 input_bits) {
    if (!input_bits || input_bits > 64) return 0;
    switch (op) {
        case OpCode::ZeroExtend32:
        case OpCode::ZeroExtend32To64: return input_bits < 32 ? input_bits : 32;
        case OpCode::ZeroExtend64:
        case OpCode::SignExtend: return input_bits;
        default: return 0;
    }
}

enum class IntegerShiftKind : u8 { None, Left, Right, ArithmeticRight, RotateRight };

enum class IntegerPredicateKind : u8 { None, Zero, Nonzero, Bit };

constexpr IntegerShiftKind ClassifyIntegerShift(OpCode op) {
    switch (op) {
        case OpCode::LslImm: case OpCode::LslValue: return IntegerShiftKind::Left;
        case OpCode::LsrImm: case OpCode::LsrValue: return IntegerShiftKind::Right;
        case OpCode::AsrImm: case OpCode::AsrValue: return IntegerShiftKind::ArithmeticRight;
        case OpCode::RorImm: case OpCode::RorValue: return IntegerShiftKind::RotateRight;
        default: return IntegerShiftKind::None;
    }
}

constexpr u32 IntegerShiftInputBits(u32 source_bits, u32 result_bits) {
    if (!IsScalarBitWidth(source_bits) || !IsScalarBitWidth(result_bits)) return 0;
    return source_bits < result_bits ? source_bits : result_bits;
}

constexpr u32 NormalizeIntegerShiftCount(u64 count, u32 result_bits) {
    return IsScalarBitWidth(result_bits) ? static_cast<u32>(count & (result_bits - 1)) : 0;
}

// Construction decides the result before argument/use registration. A call
// explicitly declares either a value type or VOID for an ignored result.
enum class ResultTypePolicy : u8 { Void, Fixed, Arg0, Explicit, Call };

enum class IREffect : u32 {
    None = 0,
    ReadMemory = 1u << 0,
    WriteMemory = 1u << 1,
    MayFault = 1u << 2,
    ReadGuestState = 1u << 3,
    WriteGuestState = 1u << 4,
    ReadFlags = 1u << 5,
    WriteFlags = 1u << 6,
    ControlFlow = 1u << 7,
    Barrier = 1u << 8,
    Call = 1u << 9,
    ReadLocal = 1u << 10,
    WriteLocal = 1u << 11,
    ReadFPEnvironment = 1u << 12,
    Unknown = 1u << 31,
};
DECLARE_ENUM_FLAG_OPERATORS(IREffect)

constexpr bool HasObservableEffects(IREffect effects) {
    constexpr auto observable = IREffect::WriteMemory | IREffect::MayFault |
            IREffect::WriteGuestState | IREffect::WriteFlags | IREffect::ControlFlow |
            IREffect::Barrier | IREffect::Call | IREffect::WriteLocal | IREffect::Unknown;
    return (static_cast<u32>(effects) & static_cast<u32>(observable)) != 0;
}

struct IRMeta {
    const OpCode op_code;
    const char* name;
    const ArgType return_type;
    const ValueType result_type;
    const ResultTypePolicy result_policy;
    const std::span<const ArgSignature> arguments;
    const IREffect effects;
};
static_assert(std::is_trivially_destructible_v<IRMeta>);

u8 GetValueSizeByte(ValueType type);

// The metadata table lives in ir_meta.cpp; the accessor is inlined
// because it sits on every host-compile hot path (simpleperf,
// 2026-10-20). The table carries one trailing COUNT sentinel entry.
extern const std::array<IRMeta, static_cast<size_t>(OpCode::COUNT) + 1>
        ir_meta_infos;

namespace detail {
[[noreturn, gnu::cold, gnu::noinline]] void InvalidIROpcode();
[[noreturn, gnu::cold, gnu::noinline]] void MissingIRResultType();
[[noreturn, gnu::cold, gnu::noinline]] void InvalidIRArguments();
}

inline const IRMeta& GetIRMetaInfo(OpCode op_code) {
    if (op_code >= OpCode::COUNT) detail::InvalidIROpcode();
    return ir_meta_infos[static_cast<u8>(op_code)];
}

const char *ArgTypeString(ArgType arg_type);

const char *ValueTypeString(ValueType value_type);

bool IsFloatValueType(ValueType type);

bool IsSignValueType(ValueType type);

ValueType GetIRValueType(u32 size_byte);

ValueType GetVecIRValueType(u32 size_byte);

ValueType GetSignedIRValueType(u32 size_byte);

}  // namespace swift::runtime::ir

// formatters
template <> struct fmt::formatter<swift::runtime::ir::ArgType> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(swift::runtime::ir::ArgType arg_type, FormatContext& ctx) const {
        return formatter<std::string>::format(swift::runtime::ir::ArgTypeString(arg_type), ctx);
    }
};

template <> struct fmt::formatter<swift::runtime::ir::ValueType> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(swift::runtime::ir::ValueType value_type, FormatContext& ctx) const {
        return formatter<std::string>::format(swift::runtime::ir::ValueTypeString(value_type), ctx);
    }
};
