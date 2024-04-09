//
// Created by 甘尧 on 2023/9/7.
//
#pragma once

#include "runtime/common/types.h"
#include "runtime/common/bit_fields.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/location.h"
#include "runtime/ir/args.h"

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

union CPUFlags {
    u64 flags{};
    BitField<ir::FlagsBit::Carry, 1, u64> carry;
    BitField<ir::FlagsBit::Overflow, 1, u64> overflow;
    BitField<ir::FlagsBit::Zero, 1, u64> zero;
    BitField<ir::FlagsBit::Negate, 1, u64> negate;
    BitField<ir::FlagsBit::Parity, 1, u64> parity;
};

struct State {
    void* l1_code_cache{};
    void* l2_code_cache{};
    void* interface{};
    HaltReason halt_reason{HaltReason::None};
    RSBFrame* rsb_pointer{};
    ir::Location current_loc{0};
    void* pt{};
    void* local_buffer{};
    CPUFlags cpu_flags{};
    u8 uniform_buffer_begin[];
};

}  // namespace swift::runtime::backend