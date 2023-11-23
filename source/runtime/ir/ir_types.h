//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include "runtime/common/types.h"
#include "runtime/ir/opcodes.h"

namespace swift::runtime::ir {

enum class ArgType : u8 { Void = 0, Value, Imm, Uniform, Local, Cond, Flags, Operand, Lambda };
enum class ValueType : u8 {
    VOID = 0,
    BOOL,
    U8,
    U16,
    U32,
    U64,
    U128,
    S8,
    S16,
    S32,
    S64,
    S128,
    V8,
    V16,
    V32,
    V64,
    V128,
    V256
};

struct IRMeta {
    const OpCode op_code;
    const char* name;
    const ArgType return_type;
    const std::vector<ArgType> arg_types;
};

const IRMeta& GetIRMetaInfo(OpCode op_code);

}  // namespace swift::runtime::ir