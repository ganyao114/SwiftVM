//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

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

enum class ValueType : u8 { VALUE_TYPE_ENUM(ENUM_DEFINE) };

DECLARE_ENUM_FLAG_OPERATORS(ValueType)

struct IRMeta {
    const OpCode op_code;
    const char* name;
    const ArgType return_type;
    const std::vector<ArgType> arg_types;
};

u8 GetValueSizeByte(ValueType type);

const IRMeta& GetIRMetaInfo(OpCode op_code);

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