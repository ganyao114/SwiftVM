//
// Created by 甘尧 on 2023/9/7.
//

#pragma once

#include <span>

namespace swift::runtime {

struct UniformDesc {
    uint32_t offset;
    uint32_t size;
};

enum ISA : uint8_t {
    kNone = 0,
    kArm,
    kArm64,
    kX86,
    kX86_64,
    kRiscv32,
    kRiscv64,
    kLoongArch
};

struct Config {
    bool enable_jit;
    bool enable_asm_interp;
    u32 uniform_buffer_size;
    ISA backend_isa;
    std::vector<UniformDesc> buffers_static_alloc; // 静态分配建议
};

}