//
// Created by 甘尧 on 2023/9/7.
//
#pragma once

#include "runtime/common/types.h"
#include "runtime/common/bit_fields.h"
#include "runtime/common/fpcr_tax_prof.h"
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

// Default-off execution-side measurement counters. Generated code only
// touches these when SVM_EXEC_PROF is enabled at translation time.
struct ExecProfileCounters {
    u64 exit_direct{};
    u64 exit_indirect{};
    u64 exit_call{};
    u64 exit_ret{};
    u64 exit_syscall{};
    u64 link_hit{};
    u64 link_miss{};
    u64 rsb_hit{};
    u64 rsb_miss{};
    u64 dispatch_entries{};
    u64 dispatch_l1_hit{};
    u64 dispatch_l2_hit{};
    u64 dispatch_miss{};
    u64 gpr_uniform_accesses{};
    u64 xmm_uniform_accesses{};
    u64 access_pad{};
    u64 region_edges{};
    u64 region_cycle_polls{};
    u64 region_fallthroughs{};
};

// Both execution-side probes share State::interface. ExecProfileCounters is
// deliberately first so SVM_EXEC_PROF's existing offsets and generated code
// remain byte-identical when the W71 probe is off.
struct RuntimeProfileInterface {
    ExecProfileCounters exec{};
    u64* hot_coalesce_counters{};
    // SVM_BACKEDGE_LATCH reuses State's first word for the request, so the
    // runtime-private L1 pointer lives here in that mode. Appending this field
    // preserves all existing profile-counter offsets.
    void* l1_code_cache{};
    // W90 probe counters are Runtime-private so generated guest threads never
    // contend on process-global atomics. Keep this last to preserve every
    // existing execution/hot-coalesce/backedge offset.
    std::array<u64, kFpcrTaxCounterCount> fpcr_tax{};
    // Diagnostic-only direct-helper timing buffer. It is allocated per
    // Runtime, so the sampled JIT path never contends with another thread.
    FpcrTimingBuffer* fpcr_timing{};
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
    // Preserve the legacy State layout exactly. With the default-OFF latch,
    // this is the historical L1 pointer. With the latch enabled, the same
    // first word is an atomic request (Signal in bit 63, counted SMC requests
    // below it), while RuntimeProfileInterface carries the L1 pointer. Keeping
    // the request at offset zero makes the hot poll exactly LDAR+CBNZ without
    // shifting any existing State/uniform address in the OFF build.
    union {
        void* l1_code_cache{};
        alignas(8) u64 exit_request;
    };
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
    // Preserve State layout after removing the untracked linkage patcher.
    void *reserved_linkage_address{};
    // Guest address space upper bound (== Config::loc_end). The interpreter
    // checks every LoadMemory/StoreMemory guest address against this limit
    // before dereferencing, converting a wild guest pointer into a clean
    // PageFatal halt instead of a host SIGSEGV. UINT64_MAX = unchecked
    // (default, safe for JIT-only paths where the signal handler covers this).
    u64 guest_addr_limit{UINT64_MAX};
    // Bounded guest window (Config::guest_addr_mask): every guest address is
    // truncated to this mask before `pt` is added, so no guest address — wild
    // pointer, 0xFFFF'FFFF'FFFF'FFFF, signed wraparound — can name host memory
    // outside the embedder's window. UINT64_MAX = disabled.
    u64 guest_addr_mask{UINT64_MAX};
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
constexpr u32 state_offset_exit_request = offsetof(State, exit_request);
static_assert(state_offset_exit_request == 0);
static_assert(sizeof(State::l1_code_cache) == sizeof(State::exit_request));
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
constexpr u32 state_offset_exec_profile_ptr = offsetof(State, interface);
constexpr u32 exec_offset_exit_direct = offsetof(RuntimeProfileInterface, exec) +
                                        offsetof(ExecProfileCounters, exit_direct);
constexpr u32 exec_offset_exit_indirect = offsetof(RuntimeProfileInterface, exec) +
                                          offsetof(ExecProfileCounters, exit_indirect);
constexpr u32 exec_offset_exit_call = offsetof(RuntimeProfileInterface, exec) +
                                      offsetof(ExecProfileCounters, exit_call);
constexpr u32 exec_offset_exit_ret = offsetof(RuntimeProfileInterface, exec) +
                                     offsetof(ExecProfileCounters, exit_ret);
constexpr u32 exec_offset_exit_syscall = offsetof(RuntimeProfileInterface, exec) +
                                         offsetof(ExecProfileCounters, exit_syscall);
constexpr u32 exec_offset_link_hit = offsetof(RuntimeProfileInterface, exec) +
                                     offsetof(ExecProfileCounters, link_hit);
constexpr u32 exec_offset_link_miss = offsetof(RuntimeProfileInterface, exec) +
                                      offsetof(ExecProfileCounters, link_miss);
constexpr u32 exec_offset_rsb_hit = offsetof(RuntimeProfileInterface, exec) +
                                    offsetof(ExecProfileCounters, rsb_hit);
constexpr u32 exec_offset_rsb_miss = offsetof(RuntimeProfileInterface, exec) +
                                     offsetof(ExecProfileCounters, rsb_miss);
constexpr u32 exec_offset_dispatch_entries = offsetof(RuntimeProfileInterface, exec) +
                                             offsetof(ExecProfileCounters, dispatch_entries);
constexpr u32 exec_offset_dispatch_l1_hit = offsetof(RuntimeProfileInterface, exec) +
                                            offsetof(ExecProfileCounters, dispatch_l1_hit);
constexpr u32 exec_offset_dispatch_l2_hit = offsetof(RuntimeProfileInterface, exec) +
                                            offsetof(ExecProfileCounters, dispatch_l2_hit);
constexpr u32 exec_offset_dispatch_miss = offsetof(RuntimeProfileInterface, exec) +
                                          offsetof(ExecProfileCounters, dispatch_miss);
constexpr u32 exec_offset_gpr_uniform_accesses =
        offsetof(RuntimeProfileInterface, exec) +
        offsetof(ExecProfileCounters, gpr_uniform_accesses);
constexpr u32 exec_offset_xmm_uniform_accesses =
        offsetof(RuntimeProfileInterface, exec) +
        offsetof(ExecProfileCounters, xmm_uniform_accesses);
constexpr u32 exec_offset_access_pad = offsetof(RuntimeProfileInterface, exec) +
                                       offsetof(ExecProfileCounters, access_pad);
constexpr u32 exec_offset_region_edges = offsetof(RuntimeProfileInterface, exec) +
                                         offsetof(ExecProfileCounters, region_edges);
constexpr u32 exec_offset_region_cycle_polls =
        offsetof(RuntimeProfileInterface, exec) +
        offsetof(ExecProfileCounters, region_cycle_polls);
constexpr u32 exec_offset_region_fallthroughs =
        offsetof(RuntimeProfileInterface, exec) +
        offsetof(ExecProfileCounters, region_fallthroughs);
constexpr u32 profile_offset_hot_coalesce_counters =
        offsetof(RuntimeProfileInterface, hot_coalesce_counters);
constexpr u32 profile_offset_l1_code_cache =
        offsetof(RuntimeProfileInterface, l1_code_cache);
constexpr u32 profile_offset_fpcr_tax =
        offsetof(RuntimeProfileInterface, fpcr_tax);
constexpr u32 profile_offset_fpcr_timing =
        offsetof(RuntimeProfileInterface, fpcr_timing);

}  // namespace swift::runtime::backend
