//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include <array>
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

enum class ValueType : u8 { VALUE_TYPE_ENUM(ENUM_DEFINE) };

DECLARE_ENUM_FLAG_OPERATORS(ValueType)

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
    const std::vector<ArgType> arg_types;
    const IREffect effects;
};

u8 GetValueSizeByte(ValueType type);

// The metadata table lives in ir_meta.cpp; the accessor is inlined
// because it sits on every host-compile hot path (simpleperf,
// 2026-10-20). The table carries one trailing COUNT sentinel entry.
extern const std::array<IRMeta, static_cast<size_t>(OpCode::COUNT) + 1>
        ir_meta_infos;

inline const IRMeta& GetIRMetaInfo(OpCode op_code) {
    ASSERT(op_code < OpCode::COUNT);
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
