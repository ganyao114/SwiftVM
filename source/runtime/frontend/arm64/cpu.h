//
// Created by SwiftGan on 2024/1/2.
//

#pragma once

#include <cstddef>
#include "translator/arm64/cpu.h"

namespace swift::arm64 {

// Reason why guest execution bailed out to the host. The value is stored
// into the guest context before ReturnToHost() so that the runtime /
// syscall layer can inspect it.
//
// NOTE: ThreadContext64 (translator/arm64/cpu.h) currently has no dedicated
// interrupt field; the unused `padding` slot right after `pstate` is used
// until a proper field is added there.
enum class InterruptReason : u32 {
    SVC = 0,
    HLT,
    BRK,
    ILL_CODE,
    PAGE_FATAL,
    FALLBACK
};

constexpr u32 kInterruptUniformOffset = offsetof(ThreadContext64, padding);

}  // namespace swift::arm64
