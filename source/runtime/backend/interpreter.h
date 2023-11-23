//
// Created by 甘尧 on 2023/9/7.
//
#pragma once

#include "runtime/common/types.h"

namespace swift::runtime {

struct InterpCache {
    std::vector<u8> frame_buffer;
};

struct InterpFrame {
    size_t common_arg;
    size_t handle_ptr;
};

class Interpreter {
public:

private:
    u32 buffer_cursor = 0;
    std::vector<u8> frame_buffer{};
};

}

