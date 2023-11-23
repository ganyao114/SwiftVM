//
// Created by 甘尧 on 2023/9/7.
//

#pragma once

#include "runtime/common/types.h"

namespace swift::runtime::backend {

extern "C" u32 swift_runtime_entry(void *state, void *code);
extern "C" void swift_code_dispatcher();

}
