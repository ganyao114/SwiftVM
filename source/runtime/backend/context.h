//
// Created by 甘尧 on 2023/9/7.
//
#pragma once

#include "runtime/common/types.h"
#include "runtime/ir/location.h"

namespace swift::runtime::backend {

constexpr u32 rsb_init_key = UINT32_MAX;
constexpr size_t rsb_stack_size = 64;

struct RSBFrame {
    u32 location_hash{};
    u32 cache_slot{};
};

struct RSBBuffer {
    std::array<RSBFrame, rsb_stack_size + 2> rsb_frames{};
};

struct State {
    void* l1_code_cache{};
    void* l2_code_cache{};
    void* interface{};
    volatile u32 halt_reason{0};
    RSBFrame* rsb_pointer{};
    ir::Location current_loc{0};
    void* pt{};
    void *local_buffer{};
    u8 uniform_buffer_begin[];
};

}  // namespace swift::runtime::backend