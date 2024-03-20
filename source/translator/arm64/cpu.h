//
// Created by SwiftGan on 2024/1/2.
//

#pragma once

#include <array>
#include "base/types.h"

namespace swift::arm64 {

struct ThreadContext64 {
    std::array<u64, 29> r;
    u64 fp;
    u64 lr;
    u64 sp;
    u64 pc;
    u32 pstate;
    u32 padding;
    std::array<u128, 32> v;
    u32 fpcr;
    u32 fpsr;
    u64 tpidr;
};

}  // namespace swift::x86
