#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "dispatch_cache.h"
#include "runtime/arch/arm64/state.h"
#include "runtime/arch/x86/state.h"
#include "runtime/atomic.h"
#include "runtime/backend/code.h"
#include "runtime/backend/dispatch_layout.inc"
#include "runtime/exit.h"

namespace swift::runtime::execution {
struct FaultContext;
using x86::GuestState;
using NativeCode = backend::NativeCode;

constexpr std::size_t DirectoryIndex(std::uint64_t pc, std::size_t mask) noexcept {
    return (pc ^ (pc >> SVM_DISPATCH_HASH_SHIFT)) & mask;
}

// The VM/domain's authoritative L2 directory, shared directly with native dispatch.
// Public pending slots may have null entry. The Process-owned callable
// directory instead fills pending/empty payloads with a cold thunk; only its
// valid code_id indexes borrowed NativeCode metadata in that same owner. Thread
// L1 entries borrow ready payloads from this directory and own no code.
struct alignas(32) DispatchSlot {
    std::uint64_t pc{};
    NativeCode::Function entry{};
    std::uint32_t code_id{UINT32_MAX};
    std::uint32_t pending_head{UINT32_MAX};
    bool occupied{};
    // Cold policy belongs to this same PC/domain directory. Ready entries
    // bypass it; native lookup still reads only the existing public prefix.
    std::uint16_t cold_entries{};
    bool jit_disabled{};
};

static_assert(std::is_standard_layout_v<DispatchSlot> && sizeof(DispatchSlot) == 32 &&
              alignof(DispatchSlot) == 32);
static_assert(offsetof(DispatchSlot, pc) == 0 && offsetof(DispatchSlot, entry) == 8);
static_assert(offsetof(NativeCode, entry) == 0 && sizeof(NativeCode::Function) == 8);
static_assert(offsetof(DispatchSlot, code_id) == 16 && offsetof(DispatchSlot, pending_head) == 20 &&
              offsetof(DispatchSlot, occupied) == 24);
static_assert(offsetof(DispatchSlot, cold_entries) == 26 &&
              offsetof(DispatchSlot, jit_disabled) == 28);
static_assert(sizeof(DispatchSlot) == (1u << SVM_DISPATCH_SLOT_SHIFT) &&
              offsetof(DispatchSlot, occupied) == SVM_DISPATCH_OCCUPIED_OFFSET);

// One guest thread owns its architectural state in this stable context. The resolver is a
// normal ABI function: it catches every exception, refreshes slots/mask after
// directory growth, and returns zero on failure. The host ABI retains x20..x29;
// the thunks reload caller-saved pins after the callback: x12..x15 in pin12,
// or x8..x15 in pin16. The default GPR pin8 mapping has no caller-saved GPRs;
// an enabled XMM bank is always reloaded after the callback.
struct NativeRunContext {
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
    // retain guest_fpcr; guest exception status survives every native boundary.
    std::uint64_t host_fpcr{}, host_fpsr{}, guest_fpcr{}, fp_environment{};
    // Diagnostic-only actual directory slot reads, including the first probe.
    std::uint64_t probe_steps{};
    // Linked XMM0..15 live in v16..v31. Cold C++ boundaries publish/reload
    // the complete bank; UINT32_MAX retains the ordinary memory ABI.
    std::uint64_t pinned_vector_offset{UINT32_MAX};
    AtomicService atomic_service{};

    union {
        GuestState state{};
        arm64::GuestState arm_state;
    };

    // Raw host exception status survives cold lookup and numerical helpers.
    // Backends translate it only at architectural status observers.
    std::uint64_t guest_fpsr{};
    DispatchEntry* l1{};

    // Root ABI recovery storage. Unit spills and helper frames never change
    // the saved root frame. Only Guest phase permits a native exception exit.
    std::uintptr_t root_sp{};
    std::atomic<std::uint64_t> phase{SVM_NATIVE_PHASE_RUNTIME};
    std::atomic<std::uint64_t> pending_events{};
    FaultContext* faults{};
};

static_assert(std::is_standard_layout_v<NativeRunContext> && sizeof(NativeRunContext) == 1072);
static_assert(sizeof(std::atomic<const NativeCode*>) == 8 &&
              std::atomic<const NativeCode*>::is_always_lock_free);
static_assert(offsetof(NativeRunContext, opaque) == 0 && offsetof(NativeRunContext, resolve) == 8);
static_assert(offsetof(NativeRunContext, slots) == 16 && offsetof(NativeRunContext, mask) == 24);
static_assert(offsetof(NativeRunContext, active) == 32 && offsetof(NativeRunContext, anchor) == 40);
static_assert(offsetof(NativeRunContext, dispatcher_entries) == 48 &&
              offsetof(NativeRunContext, cache_hits) == 56);
static_assert(offsetof(NativeRunContext, pinned_uniform_offset) == 64 &&
              offsetof(NativeRunContext, flags_offset) == 72);
static_assert(offsetof(NativeRunContext, pc_offset) == 80 &&
              offsetof(NativeRunContext, pinned_uniform_slots) == 88);
static_assert(offsetof(NativeRunContext, native_nzcv) == 96);
static_assert(offsetof(NativeRunContext, host_fpcr) == 104 &&
              offsetof(NativeRunContext, host_fpsr) == 112);
static_assert(offsetof(NativeRunContext, guest_fpcr) == 120 &&
              offsetof(NativeRunContext, fp_environment) == 128);
static_assert(offsetof(NativeRunContext, probe_steps) == 136);
static_assert(offsetof(NativeRunContext, pinned_vector_offset) == 144);
static_assert(offsetof(NativeRunContext, atomic_service) == 152);
static_assert(offsetof(NativeRunContext, state) == 160);
static_assert(offsetof(NativeRunContext, guest_fpsr) == 1024);
static_assert(offsetof(NativeRunContext, l1) == SVM_DISPATCH_CTX_L1);
static_assert(offsetof(NativeRunContext, root_sp) == 1040);
static_assert(offsetof(NativeRunContext, phase) == 1048);
static_assert(offsetof(NativeRunContext, pending_events) == 1056);
static_assert(offsetof(NativeRunContext, faults) == 1064);

// Linked units enter/leave through these thunks. Only SvmNativeExit returns to C++;
// the root frame owns the one save of x19..x30 and host d8..d15 across all
// unit-to-unit jumps. Guest XMM pins remain live on warm edges.
extern "C" std::uint64_t SvmRunNative(NativeRunContext*) noexcept;
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
// ARM64 inline L1 misses carry the already selected slot in x7. These
// entries probe only L2 and do not count the same dispatch a second time.
extern "C" void SvmNativeL2Dispatch() noexcept;
extern "C" void SvmNativeL2DispatchCounted() noexcept;
extern "C" void SvmNativeL2DispatchNZCV() noexcept;
extern "C" void SvmNativeL2DispatchNZCVCounted() noexcept;
// Callable-directory tag matches only. x0 is the real target PC; the counted
// entry cancels exactly the hit increment just made before this BR.
extern "C" void SvmNativePending() noexcept;
extern "C" void SvmNativePendingCounted() noexcept;
extern "C" void SvmNativeExit() noexcept;
extern "C" void SvmNativeExceptionExit() noexcept;

}  // namespace swift::runtime::execution
