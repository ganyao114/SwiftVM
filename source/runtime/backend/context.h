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
    ir::Location prev_loc{0};
    void* pt{};
    void* local_buffer{};
    u64 host_cpu_flags{};
    void *blocking_linkage_address{};
    u8 uniform_buffer_begin[];
};

constexpr u32 state_offset_uniform_buffer = offsetof(State, uniform_buffer_begin);
constexpr u32 state_offset_local_buffer = offsetof(State, local_buffer);
constexpr u32 state_offset_l1_code_cache = offsetof(State, l1_code_cache);
constexpr u32 state_offset_l2_code_cache = offsetof(State, l2_code_cache);
constexpr u32 state_offset_halt_reason = offsetof(State, halt_reason);
constexpr u32 state_offset_current_loc = offsetof(State, current_loc);
constexpr u32 state_offset_prev_loc = offsetof(State, prev_loc);
constexpr u32 state_offset_pt = offsetof(State, pt);
constexpr u32 state_offset_rsb_pointer = offsetof(State, rsb_pointer);
constexpr u32 state_offset_host_flags = offsetof(State, host_cpu_flags);
constexpr u32 state_offset_blocking_linkage_address = offsetof(State, blocking_linkage_address);

}  // namespace swift::runtime::backend