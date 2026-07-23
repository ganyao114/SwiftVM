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

// Direct RSB frame: the JIT push/pop operates on 16-byte entries via
// pre-decrement / post-increment of the rsb_ptr register (x25).
//   offset 0: guest_location — the guest return address (validation key)
//   offset 8: dispatch_index — L2 dispatch-table slot for the return target
// On ret the JIT pops a frame, compares guest_location with the actual
// return target in state->current_loc, and on a hit loads the compiled
// code pointer from the L2 dispatch table slot and branches directly —
// skipping the full trampoline dispatcher round-trip.
struct RSBFrame {
    u64 guest_location{};
    u64 dispatch_index{};
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
    // RSB bounds for the JIT overflow/underflow guards (JitContext::EmitRSBPush/
    // EmitRSBPop). The stack grows downward from rsb_top (the empty position,
    // entry [rsb_stack_size]) toward rsb_bottom (entry [0]).
    //   rsb_bottom = &rsb_frames[0]              — push skips once rsb_ptr <= here (full)
    //   rsb_top    = &rsb_frames[rsb_stack_size] — pop falls back once rsb_ptr >= here (empty)
    // Without these guards an imbalance of guest ret over call would walk
    // rsb_ptr past the buffer and the speculative RSB load would read/branch
    // on garbage (SIGSEGV).
    RSBFrame* rsb_bottom{};
    RSBFrame* rsb_top{};
    ir::Location current_loc{0};
    ir::Location prev_loc{0};
    void* pt{};
    void* local_buffer{};
    u64 host_cpu_flags{};
    void *blocking_linkage_address{};
    // Guest address space upper bound (== Config::loc_end). The interpreter
    // checks every LoadMemory/StoreMemory guest address against this limit
    // before dereferencing, converting a wild guest pointer into a clean
    // PageFatal halt instead of a host SIGSEGV. UINT64_MAX = unchecked
    // (default, safe for JIT-only paths where the signal handler covers this).
    u64 guest_addr_limit{UINT64_MAX};
    // Optional precise range check for the interpreter: if non-null, called
    // with (interp_range_check_ctx, guest_addr, size) before every memory
    // access. Returns false → PageFatal. Wired by the translator layer to
    // GuestMemory::RangeIsMapped. nullptr = fall back to guest_addr_limit only.
    bool (*interp_range_check)(void* ctx, u64 addr, u64 size){nullptr};
    void* interp_range_check_ctx{};
    // Spill area for RegAlloc::MEM values (linear-scan register allocator):
    // fixed u64 slots addressed from JIT code as
    // [state, state_offset_spill_area + slot * 8]; a spilled SIMD value
    // occupies two consecutive slots (16 bytes, hence alignas(16)). Kept
    // inside State, right before the flexible uniform buffer, so no extra
    // allocation or Config plumbing is needed and every uniform-buffer
    // offset keeps resolving through offsetof. The size must match
    // backend::kMaxSpillSlots (backend/reg_alloc.h) — enforced by a
    // static_assert in arm64/jit/jit_context.cpp; the allocator panics
    // rather than hand out a slot beyond it.
    alignas(16) std::array<u64, 64> spill_area{};
    u8 uniform_buffer_begin[];
};

constexpr u32 state_offset_uniform_buffer = offsetof(State, uniform_buffer_begin);
constexpr u32 state_offset_spill_area = offsetof(State, spill_area);
constexpr u32 state_offset_local_buffer = offsetof(State, local_buffer);
constexpr u32 state_offset_l1_code_cache = offsetof(State, l1_code_cache);
constexpr u32 state_offset_l2_code_cache = offsetof(State, l2_code_cache);
constexpr u32 state_offset_halt_reason = offsetof(State, halt_reason);
constexpr u32 state_offset_current_loc = offsetof(State, current_loc);
constexpr u32 state_offset_prev_loc = offsetof(State, prev_loc);
constexpr u32 state_offset_pt = offsetof(State, pt);
constexpr u32 state_offset_rsb_pointer = offsetof(State, rsb_pointer);
constexpr u32 state_offset_rsb_bottom = offsetof(State, rsb_bottom);
constexpr u32 state_offset_rsb_top = offsetof(State, rsb_top);
constexpr u32 state_offset_host_flags = offsetof(State, host_cpu_flags);
constexpr u32 state_offset_blocking_linkage_address = offsetof(State, blocking_linkage_address);

}  // namespace swift::runtime::backend