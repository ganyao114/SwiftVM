//
// Created by 甘尧 on 2023/9/27.
//
#pragma once

#include "runtime/common/types.h"
#include "runtime/include/config.h"
#include "runtime/common/logging.h"

namespace swift::runtime::backend {

// ARM instruction alignment. ARM processors require code to be 4-byte aligned,
// but ARM ELF requires 8..
static constexpr size_t kArmAlignment = 8;

// ARM64 instruction alignment. This is the recommended alignment for maximum performance.
static constexpr size_t kArm64Alignment = 16;

// X86 instruction alignment. This is the recommended alignment for maximum performance.
static constexpr size_t kX86Alignment = 16;

// Different than code alignment since code alignment is only first instruction of method.
static constexpr size_t kThumb2InstructionAlignment = 2;
static constexpr size_t kArm32InstructionAlignment = 4;
static constexpr size_t kArm64InstructionAlignment = 4;
static constexpr size_t kRiscv32InstructionAlignment = 4;
static constexpr size_t kRiscv64InstructionAlignment = 4;
static constexpr size_t kLoongArchInstructionAlignment = 4;
static constexpr size_t kX86InstructionAlignment = 1;
static constexpr size_t kX86_64InstructionAlignment = 1;

constexpr size_t GetInstructionSetInstructionAlignment(ISA isa) {
    switch (isa) {
        case kArm:
            return kArm32InstructionAlignment;
        case kArm64:
            return kArm64InstructionAlignment;
        case kX86:
            return kX86InstructionAlignment;
        case kX86_64:
            return kX86_64InstructionAlignment;
        case kRiscv32:
            return kRiscv32InstructionAlignment;
        case kRiscv64:
            return kRiscv64InstructionAlignment;
        case kLoongArch:
            return kLoongArchInstructionAlignment;
        default:
            ASSERT_MSG(false, "kNone");
            return 0;
    }
}

}  // namespace swift::runtime::backend