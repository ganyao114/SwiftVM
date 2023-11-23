//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include "runtime/common/types.h"

namespace swift::runtime::ir {

enum class OpCode : u8 {
    Void = 0,
#define INST(OP, ...) OP,
#include "ir.inc"
#undef INST
    BASE_COUNT,
    SetLocation,
    COUNT
};

}  // namespace swift::runtime::ir