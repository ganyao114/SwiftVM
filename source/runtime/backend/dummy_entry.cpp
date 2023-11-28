//
// Created by 甘尧 on 2023/11/27.
//
#include "entrypoint.h"

namespace swift::runtime::backend {

extern "C" u32 swift_runtime_entry(void *state, void *code) {
    return 0;
}

extern "C" void swift_code_dispatcher() {}

}