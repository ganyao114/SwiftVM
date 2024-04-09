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

static const std::array ir_meta_infos {
IRMeta{OpCode::Void, "Void", Void, {}},
#define INST(name, ret, ...) IRMeta{OpCode::name, #name, ret, {__VA_ARGS__}},
#include "ir.inc"
#undef INST
IRMeta{OpCode::BASE_COUNT, "BASE_COUNT", Void, {}},
IRMeta{OpCode::COUNT, "COUNT", Void, {}}
};

u8 GetValueSizePow(ValueType type) {
    if (type == ValueType::BOOL) {
        return 0;
    } else if (type >= ValueType::U8 && type <= ValueType::U64) {
        return (u32) type - (u32) ValueType::U8;
    } else if (type >= ValueType::S8 && type <= ValueType::S64) {
        return (u32) type - (u32) ValueType::S8;
    } else if (type >= ValueType::V8 && type <= ValueType::V256) {
        return (u32) type - (u32) ValueType::V8;
    } else {
        PANIC();
    }
}

u8 GetValueSizeByte(ValueType type) {
    return std::pow(2, GetValueSizePow(type));
}

const IRMeta &GetIRMetaInfo(OpCode op_code) {
    ASSERT(op_code < OpCode::COUNT);
    return ir_meta_infos[static_cast<u8>(op_code)];
}

}
