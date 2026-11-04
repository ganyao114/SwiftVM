#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "guest_state.h"
#include "native_code.h"
#include "runtime/backend/arm64/abi.h"

namespace swift::runtime::execution {

// The runtime's sole PC directory, shared directly with native dispatch.
// Public pending slots may have null entry. The Process-owned callable
// directory instead fills pending/empty payloads with a cold thunk; only its
// valid code_id indexes borrowed NativeCode metadata in that same owner.
struct alignas(32) DispatchSlot {
    std::uint64_t pc{};
    NativeCode::Function entry{};
    std::uint32_t code_id{UINT32_MAX};
    std::uint32_t pending_head{UINT32_MAX};
    bool occupied{};
};
static_assert(std::is_standard_layout_v<DispatchSlot> && sizeof(DispatchSlot) == 32 && alignof(DispatchSlot) == 32);
static_assert(offsetof(DispatchSlot, pc) == 0 && offsetof(DispatchSlot, entry) == 8);
static_assert(offsetof(NativeCode, entry) == 0 && sizeof(NativeCode::Function) == 8);
static_assert(offsetof(DispatchSlot, code_id) == 16 && offsetof(DispatchSlot, pending_head) == 20 &&
        offsetof(DispatchSlot, occupied) == 24);
static_assert(sizeof(DispatchSlot) == (1u << backend::arm64::abi::DispatchSlotShift) &&
              offsetof(DispatchSlot, occupied) == backend::arm64::abi::DispatchOccupiedOffset);

// One native run borrows this stable Process-owned context. The resolver is a
// normal ABI function: it catches every exception, refreshes slots/mask after
// directory growth, and returns zero on failure. The host ABI retains x20..x29;
// the thunks reload caller-saved pins after the callback: x12..x15 in pin12,
// or x8..x15 in pin16. The default GPR pin8 mapping has no caller-saved GPRs;
// an enabled XMM bank is always reloaded after the callback.
struct NativeRunContext {
    GuestState* state{};
    void* opaque{};
    std::uintptr_t (*resolve)(void*, NativeRunContext*) noexcept {};
    DispatchSlot* slots{};
    std::uint64_t mask{};
    std::atomic<const NativeCode*>* active{};
    const NativeCode* anchor{};
    std::uint64_t dispatcher_entries{}, cache_hits{};
    std::uint64_t pinned_uniform_offset{UINT32_MAX}, flags_offset{UINT32_MAX}, pc_offset{128};
    std::uint64_t pinned_uniform_slots{8};
    // Native linked flags: inverted hardware C, raw PF in x1, and
    // AF=(x29^x1).bit4. Reserved packed bits stay in the actual state field.
    std::uint64_t native_nzcv{};
    // Root/cold/helper boundaries own the FP environment. Warm native links
    // retain guest_fpcr; FPSR is isolated host state, not guest MXCSR sticky.
    std::uint64_t host_fpcr{}, host_fpsr{}, guest_fpcr{}, fp_environment{};
    // Diagnostic-only actual directory slot reads, including the first probe.
    std::uint64_t probe_steps{};
    // Linked XMM0..15 live in v16..v31. Cold C++ boundaries publish/reload
    // the complete bank; UINT32_MAX retains the ordinary memory ABI.
    std::uint64_t pinned_vector_offset{UINT32_MAX};
};
static_assert(std::is_standard_layout_v<NativeRunContext> && sizeof(NativeRunContext) == 160);
static_assert(sizeof(std::atomic<const NativeCode*>) == 8 && std::atomic<const NativeCode*>::is_always_lock_free);
static_assert(offsetof(NativeRunContext, state) == 0 && offsetof(NativeRunContext, opaque) == 8);
static_assert(offsetof(NativeRunContext, resolve) == 16 && offsetof(NativeRunContext, slots) == 24);
static_assert(offsetof(NativeRunContext, mask) == 32 && offsetof(NativeRunContext, active) == 40);
static_assert(offsetof(NativeRunContext, anchor) == 48 && offsetof(NativeRunContext, dispatcher_entries) == 56);
static_assert(offsetof(NativeRunContext, cache_hits) == 64 && offsetof(NativeRunContext, pinned_uniform_offset) == 72);
static_assert(offsetof(NativeRunContext, flags_offset) == 80 && offsetof(NativeRunContext, pc_offset) == 88);
static_assert(offsetof(NativeRunContext, pinned_uniform_slots) == 96);
static_assert(offsetof(NativeRunContext, native_nzcv) == 104);
static_assert(offsetof(NativeRunContext, host_fpcr) == 112 && offsetof(NativeRunContext, host_fpsr) == 120);
static_assert(offsetof(NativeRunContext, guest_fpcr) == 128 && offsetof(NativeRunContext, fp_environment) == 136);
static_assert(offsetof(NativeRunContext, probe_steps) == 144);
static_assert(offsetof(NativeRunContext, pinned_vector_offset) == 152);
static_assert(offsetof(NativeRunContext, slots) == backend::arm64::abi::ContextSlotsOffset &&
              offsetof(NativeRunContext, dispatcher_entries) == backend::arm64::abi::ContextDispatchesOffset &&
              offsetof(NativeRunContext, cache_hits) == backend::arm64::abi::ContextHitsOffset &&
              offsetof(NativeRunContext, probe_steps) == backend::arm64::abi::ContextProbesOffset);

// Linked units enter/leave through these thunks. Only SvmNativeExit returns to C++;
// the root frame owns the one save of x19..x30 and host d8..d15 across all
// unit-to-unit jumps. Guest XMM pins remain live on warm edges.
extern "C" std::uint64_t SvmRunNative(GuestState*, NativeRunContext*) noexcept;
// All dispatcher entries take the actual GuestPC in x0. Deferred-PC callers
// may leave state.pc stale on ready hits; a cold miss commits x0 before the
// C++ resolver. Eager callers remain valid. Native variants retain raw PF/AF
// and leave NZCV untouched on ready hits; only a cold miss captures and packs
// them. The shared root entry and NativeExit have their own boundary contract.
extern "C" void SvmNativeDispatch() noexcept;
// Chosen once when compiling a diagnostic run. The ordinary lookup executes
// no lookup/probe/hit counters and has no runtime branch to decide whether to count.
extern "C" void SvmNativeDispatchCounted() noexcept;
extern "C" void SvmNativeDispatchNZCV() noexcept;
extern "C" void SvmNativeDispatchNZCVCounted() noexcept;
// Callable-directory tag matches only. x0 is the real target PC; the counted
// entry cancels exactly the hit increment just made before this BR.
extern "C" void SvmNativePending() noexcept;
extern "C" void SvmNativePendingCounted() noexcept;
extern "C" void SvmNativeExit() noexcept;

} // namespace swift::runtime::execution
