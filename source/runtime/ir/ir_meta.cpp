//
// Created by 甘尧 on 2023/9/8.
//

#include <cmath>
#include "runtime/common/logging.h"
#include "runtime/ir/ir_types.h"

namespace swift::runtime::ir {

constexpr ArgType BOOL = ArgType::Value;
constexpr ArgType U8 = ArgType::Value;
constexpr ArgType U16 = ArgType::Value;
constexpr ArgType U32 = ArgType::Value;
constexpr ArgType U64 = ArgType::Value;
constexpr ArgType Void = ArgType::Void;
constexpr ArgType Value = ArgType::Value;
constexpr ArgType Imm = ArgType::Imm;
constexpr ArgType Uniform = ArgType::Uniform;
constexpr ArgType Local = ArgType::Local;
constexpr ArgType Cond = ArgType::Cond;
constexpr ArgType Flags = ArgType::Flags;
constexpr ArgType Operand = ArgType::Operand;
constexpr ArgType Lambda = ArgType::Lambda;
constexpr ArgType Params = ArgType::Params;

static const std::array ir_meta_infos{IRMeta{OpCode::Void, "Void", Void, {}},
#define INST(name, ret, ...) IRMeta{OpCode::name, #name, ret, {__VA_ARGS__}},
#include "ir.inc"
#undef INST
IRMeta{OpCode::BASE_COUNT, "BASE_COUNT", Void, {}},
IRMeta{OpCode::COUNT, "COUNT", Void, {}}};

u8 GetValueSizePow(ValueType type) {
    if (type >= ValueType::U8 && type <= ValueType::U64) {
        return (u32)type - (u32)ValueType::U8;
    } else if (type >= ValueType::S8 && type <= ValueType::S64) {
        return (u32)type - (u32)ValueType::S8;
    } else if (type >= ValueType::V8 && type <= ValueType::V256) {
        return (u32)type - (u32)ValueType::V8;
    } else {
        PANIC();
    }
}

ValueType GetIRValueType(u32 size_byte) {
    switch (size_byte) {
        case 1:
            return ValueType::U8;
        case 2:
            return ValueType::U16;
        case 4:
            return ValueType::U32;
        case 8:
            return ValueType::U64;
        default:
            return ValueType::VOID;
    }
}

ValueType GetVecIRValueType(u32 size_byte) {
    switch (size_byte) {
        case 1:
            return ValueType::V8;
        case 2:
            return ValueType::V16;
        case 4:
            return ValueType::V32;
        case 8:
            return ValueType::V64;
        case 16:
            return ValueType::V128;
        case 32:
            return ValueType::V256;
        default:
            return ValueType::VOID;
    }
}

ValueType GetSignedIRValueType(u32 size_byte) {
    switch (size_byte) {
        case 1:
            return ValueType::S8;
        case 2:
            return ValueType::S16;
        case 4:
            return ValueType::S32;
        case 8:
            return ValueType::S64;
        default:
            return ValueType::VOID;
    }
}

u8 GetValueSizeByte(ValueType type) { return std::pow(2u, GetValueSizePow(type)); }

bool IsFloatValueType(ValueType type) { return type >= ValueType::V8 && type <= ValueType::V256; }

bool IsSignValueType(ValueType type) { return type >= ValueType::S8 && type <= ValueType::S64; }

const IRMeta& GetIRMetaInfo(OpCode op_code) {
    ASSERT(op_code < OpCode::COUNT);
    return ir_meta_infos[static_cast<u8>(op_code)];
}

const char* ArgTypeString(ArgType arg_type) {
#define ENUM_CLASS ArgType
    switch (arg_type) { ARG_TYPE_ENUM(ENUM_TO_STRING_CASE) }
    return "Unk";
#undef ENUM_CLASS
}

const char* ValueTypeString(ValueType value_type) {
#define ENUM_CLASS ValueType
    switch (value_type) { VALUE_TYPE_ENUM(ENUM_TO_STRING_CASE) }
    return "Unk";
#undef ENUM_CLASS
}

}  // namespace swift::runtime::ir
