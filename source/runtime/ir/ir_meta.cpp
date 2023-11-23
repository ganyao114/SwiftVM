//
// Created by 甘尧 on 2023/9/8.
//

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

static const std::array ir_meta_infos {
IRMeta{OpCode::Void, "Void", Void, {}},
#define INST(name, ret, ...) IRMeta{OpCode::name, #name, ret, {__VA_ARGS__}},
#include "ir.inc"
#undef INST
IRMeta{OpCode::BASE_COUNT, "BASE_COUNT", Void, {}},
IRMeta{OpCode::SetLocation, "SetLocation", Void, {Imm}},
IRMeta{OpCode::COUNT, "COUNT", Void, {}}
};

const IRMeta &GetIRMetaInfo(OpCode op_code) {
    ASSERT(op_code < OpCode::COUNT);
    return ir_meta_infos[static_cast<u8>(op_code)];
}

}
