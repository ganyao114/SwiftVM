//
// Created by SwiftGan on 2024/1/2.
//

#pragma once

#include <array>
#include "base/types.h"

namespace swift::arm64 {

// Guest CPU context, laid out at the start of the runtime uniform buffer
// (Config::uniform_buffer_size == sizeof(ThreadContext64)).
//
// Linux syscall (svc) convention used by the translator + linux loader:
//   - syscall number: x8  (r[8])
//   - arguments:      x0-x5 (r[0]..r[5])
//   - return value:   written back to x0 (r[0]) by the host handler
//   - on svc, pc must point to the instruction *after* the svc when the
//     runtime halts with HaltReason::CallHost; execution resumes from pc.
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
