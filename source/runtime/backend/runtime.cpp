#include "base/logging.h"
//
// Created by 甘尧 on 2023/9/7.
//
#pragma once

#include <algorithm>
#include "runtime/common/signal_diagnostic.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <utility>
#include <unistd.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/constant.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/arm64/fpcr_mode.h"
#include "runtime/backend/arm64/jit/function_code_object_emitter.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/atomic_fallback.h"
#include "runtime/backend/context.h"
#include "runtime/backend/function_entry_contract.h"
#include "runtime/backend/guarded_return_stack.h"
#include "runtime/backend/guest_memory_scope.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/interrupt_l1_mapping.h"
#include "runtime/backend/interrupt_poll_state.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/signal_handler.h"
#include "runtime/backend/translate_table.h"
#include "runtime/common/backedge_control.h"
#include "runtime/common/hot_coalesce_prof.h"
#include "runtime/common/perf_stats.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/function.h"
#include "runtime/ir/opts/loop_invariant_hoist_pass.h"
#include "runtime/ir/opts/pass_pipeline.h"
#include "runtime/ir/opts/register_alloc_pass.h"

namespace swift::runtime {

static_assert(std::atomic<u64>::is_always_lock_free,
              "signal-time FPCR handoff must not call a locking atomic runtime");

constexpr static auto l1_cache_bits = L1_CODE_CACHE_BITS;

std::unique_ptr<Instance> Instance::Make(const Config& config) {
    return std::make_unique<backend::AddressSpace>(config);
}

// Thread-local pointer to the Runtime::Impl currently executing guest code
// on this thread; the host signal handler uses it to route faults to the
// right State. void* because Runtime::Impl is a private nested type.
static thread_local void* tls_active_runtime{};

namespace {

// Runtime 私有的只读向量常量前缀。它不属于 guest State，也不进入
// uniform/signal/xsave 布局；构造后没有可变访问入口。
struct alignas(16) RuntimeNamedVectorConstants {
    std::array<u64, 2> byte_movmask_weights{
            0x8040201008040201ULL,
            0x8040201008040201ULL,
    };
    std::array<u64, 2> aes_keygen_swizzle{
            0x040B0E010B0E0104ULL,
            0x0C0306090306090CULL,
    };
};

backend::InterruptL1Mapping& GetInterruptL1Mapping() {
    static backend::InterruptL1Mapping mapping{
            (size_t{1} << l1_cache_bits) * sizeof(TranslateEntry)};
    return mapping;
}

static_assert(sizeof(RuntimeNamedVectorConstants) == 32);
static_assert(sizeof(RuntimeNamedVectorConstants) <= 256);
static_assert(alignof(RuntimeNamedVectorConstants) == alignof(backend::State));
static_assert(backend::state_offset_named_vector_constants ==
              -static_cast<s32>(sizeof(RuntimeNamedVectorConstants)));
static_assert(backend::state_offset_byte_movmask_weights ==
              backend::state_offset_named_vector_constants +
                      offsetof(RuntimeNamedVectorConstants, byte_movmask_weights));
static_assert(backend::state_offset_aes_keygen_swizzle ==
              backend::state_offset_named_vector_constants +
                      offsetof(RuntimeNamedVectorConstants, aes_keygen_swizzle));

}  // namespace

// Thread-local pointer to the Runtime::Impl this thread *owns*, valid from
// construction to destruction rather than only while guest code runs.
//
// Host code outside JitRun writes guest memory too: syscall emulation copying
// results into a guest buffer, and the clone-thread teardown store that
// CLONE_CHILD_CLEARTID requires. Such a write lands on a *write-protected*
// guest page whenever the target shares a page with translated code (freestanding
// guests routinely put .bss in the same page as .text). That is an ordinary SMC
// write-protect fault, but tls_active_runtime is null there — Runtime::Run has
// already returned — so the SMC handler used to decline it and the process died
// on an "unhandled host fault". Faulting host code is *outside* guest execution,
// so opening the write window and retrying the store is exactly right: a host
// store into a code page is self-modifying code and must invalidate.
//
// Indirected through a shared slot rather than stored as a raw pointer: a
// Runtime may be destroyed by a thread other than the one that created it, and
// the slot (not the Impl) is what the owning thread's TLS keeps alive, so the
// handler can never observe a dangling Impl.
struct OwnerSlot {
    std::atomic<void*> impl{nullptr};
};
static thread_local std::shared_ptr<OwnerSlot> tls_owner_slot{};

struct Runtime::Impl final {
    explicit Impl(backend::AddressSpace* address_space)
            : state_storage(sizeof(RuntimeNamedVectorConstants) +
                            sizeof(backend::State) +
                            address_space->GetConfig().uniform_buffer_size),
              address_space(address_space) {
        auto* named = state_storage.Data();
        state = reinterpret_cast<backend::State*>(
                named + sizeof(RuntimeNamedVectorConstants));
        ASSERT_MSG(reinterpret_cast<std::uintptr_t>(named) %
                                   alignof(RuntimeNamedVectorConstants) == 0,
                   "runtime named-vector prefix is not 16-byte aligned");
        ASSERT_MSG(reinterpret_cast<std::uintptr_t>(state) % alignof(backend::State) == 0,
                   "runtime State is not 16-byte aligned after named-vector prefix");
        ASSERT_MSG(reinterpret_cast<u8*>(state) + backend::state_offset_interrupt_poll ==
                           state_storage.PollAddress(),
                   "runtime interrupt poll offset does not match State placement");
        constexpr RuntimeNamedVectorConstants constants{};
        std::memcpy(named, &constants, sizeof(constants));
        const auto& svm_config = GetSvmConfig();
        const bool exec_profile_enabled = svm_config.exec_prof;
        execution_trace_enabled = svm_config.exec_trace;
        if (execution_trace_enabled) {
            profile_interface.execution_trace = &execution_trace;
        }
        hot_coalesce_enabled = HotCoalesceProfEnabled();
        indirect_l1_prof_enabled = IndirectL1ProfEnabled();
        flags_regs_audit_enabled = FlagsRegsAuditEnabled();
        hot_counter_storage_enabled =
                hot_coalesce_enabled || indirect_l1_prof_enabled ||
                flags_regs_audit_enabled;
        if (hot_counter_storage_enabled) {
            hot_coalesce_counters.resize(
                    static_cast<size_t>(kHotCoalesceMaxUnits) *
                    kHotCoalesceCounterCount);
            profile_interface.hot_coalesce_counters = hot_coalesce_counters.data();
        }
        profile_interface.l1_code_cache = l1_code_cache.Data();
        state->indirect_l1_code_cache = l1_code_cache.Data();
        state->indirect_call_l1_code_cache =
                address_space->GetCallCodeCacheTable().Data();
        state->pending_call_l1_code_cache =
                address_space->GetPendingCallCodeCacheTable().Data();
        ASSERT_MSG(reinterpret_cast<std::uintptr_t>(l1_code_cache.Data()) %
                                   l1_code_cache.DataAlignment() == 0,
                   "runtime L1 cache does not satisfy its address-formation alignment");
        // Production inline checks turn an invalidated key hit into a branch
        // to the dispatcher's L2 continuation. The diagnostic form keeps zero
        // so it can distinguish this fallback as a miss.
        if (indirect_l1_prof_enabled) {
            l1_code_cache.SetInvalidValue(0);
        } else {
            l1_code_cache.SetIndexedInvalidValue(
                    GetInterruptL1Mapping().Data());
        }
        // Inline indirect-L1 faults use this request word even when the
        // optional backedge/SMC latch is disabled.
        state->exit_request = 0;
        state->interface = &profile_interface;
        // Wire the dispatcher's code-cache tables: L1 is per-runtime, L2 is the
        // address-space wide translate table that PushCodeCache writes to.
        state->l2_code_cache = address_space->GetCodeCacheTable().Data();
        // Guest address virtualization: Config::memory_base carries the
        // guest->host bias (host = guest + bias); the JIT keeps it in the
        // reserved pt register and the interpreter reads it from here.
        // nullptr (direct) keeps the zero-overhead fast path.
        state->pt = address_space->GetConfig().memory_base;
        // Bounded guest window: truncate every guest address to the window
        // before pt is added. 0 in the config = disabled.
        state->guest_addr_mask = address_space->GetConfig().guest_addr_mask
                                         ? address_space->GetConfig().guest_addr_mask
                                         : UINT64_MAX;
        // Interpreter wild-pointer guard: any guest address >= loc_end is
        // definitionally invalid; the interpreter checks this before every
        // memory access and raises PageFatal instead of crashing the host.
        state->guest_addr_limit = static_cast<u64>(address_space->GetConfig().loc_end);
        state->unaligned_atomic_lock_address =
                &backend::unaligned_atomic_lock;
        if (True(address_space->GetConfig().global_opts & Optimizations::ReturnStackBuffer)) {
            return_stack.emplace();
            state->rsb_pointer = return_stack->Empty();
            state->rsb_empty = return_stack->Empty();
        }
        smc_epoch = address_space->GetSmcTracker().RegisterRuntime(
                l1_code_cache,
                &state->exit_request,
                &state_storage,
                &state->rsb_pointer,
                state->rsb_empty);
        jit_entry = address_space->GetTrampolines().GetRuntimeEntry();
        // Claim this thread for host-side SMC fault recovery (see OwnerSlot).
        // A thread that constructs a second Runtime keeps the newest; either
        // resolves to the same AddressSpace and SmcTracker.
        if (!tls_owner_slot) {
            tls_owner_slot = std::make_shared<OwnerSlot>();
        }
        owner_slot = tls_owner_slot;
        owner_slot->impl.store(this, std::memory_order_release);
    }

    ~Impl() {
        if (GetSvmConfig().exec_prof) {
            const auto elapsed_ns = exec_profile_started
                                            ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                      std::chrono::steady_clock::now() -
                                                      exec_profile_start)
                                                      .count()
                                            : 0;
            const auto& p = profile_interface.exec;
            const u64 exits = p.exit_direct + p.exit_indirect + p.exit_call +
                              p.exit_ret + p.exit_syscall;
            const double seconds = static_cast<double>(elapsed_ns) / 1.0e9;
            SVM_DIAG_PRINT(Runtime,
                    "[svm-exec] elapsed_s=%.9f exits=%llu exits_per_s=%.3f "
                    "direct=%llu indirect=%llu call=%llu ret=%llu syscall=%llu "
                    "link_hit=%llu link_miss=%llu rsb_hit=%llu rsb_miss=%llu "
                    "dispatch=%llu l1_hit=%llu l2_hit=%llu miss=%llu "
                    "gpr_uniform=%llu xmm_uniform=%llu region_edges=%llu "
                    "region_cycle_polls=%llu region_fallthroughs=%llu pad=%s\n",
                    seconds,
                    static_cast<unsigned long long>(exits),
                    seconds > 0 ? static_cast<double>(exits) / seconds : 0.0,
                    static_cast<unsigned long long>(p.exit_direct),
                    static_cast<unsigned long long>(p.exit_indirect),
                    static_cast<unsigned long long>(p.exit_call),
                    static_cast<unsigned long long>(p.exit_ret),
                    static_cast<unsigned long long>(p.exit_syscall),
                    static_cast<unsigned long long>(p.link_hit),
                    static_cast<unsigned long long>(p.link_miss),
                    static_cast<unsigned long long>(p.rsb_hit),
                    static_cast<unsigned long long>(p.rsb_miss),
                    static_cast<unsigned long long>(p.dispatch_entries),
                    static_cast<unsigned long long>(p.dispatch_l1_hit),
                    static_cast<unsigned long long>(p.dispatch_l2_hit),
                    static_cast<unsigned long long>(p.dispatch_miss),
                    static_cast<unsigned long long>(p.gpr_uniform_accesses),
                    static_cast<unsigned long long>(p.xmm_uniform_accesses),
                    static_cast<unsigned long long>(p.region_edges),
                    static_cast<unsigned long long>(p.region_cycle_polls),
                    static_cast<unsigned long long>(p.region_fallthroughs),
                    std::to_string(GetSvmConfig().exec_access_pad).c_str());
        }
        if (hot_counter_storage_enabled) {
            HotCoalesceSubmitThread(hot_coalesce_counters);
        }
        // NOTE: a write window opened by host code on a thread that then exits
        // is deliberately NOT closed here. Closing it looked prudent, but a
        // mutation test (delete the call, run the suites) could not tell the
        // two versions apart -- and it is not needed: SmcTracker already
        // collects translations published during another thread's open window
        // (see RegisterNode's !rec.dirty guard and CloseWriteWindow's retry
        // loop), and any thread that goes on running closes the window after
        // its next JitRun. Unverified defensive work is not kept.
        address_space->GetSmcTracker().UnregisterRuntime(smc_epoch);
        if (owner_slot) {
            void* expected = this;
            owner_slot->impl.compare_exchange_strong(
                    expected, nullptr, std::memory_order_acq_rel, std::memory_order_relaxed);
        }
    }

    // Host-fault recovery (SignalHandler chain, priority kFaultPriority).
    // Handles SIGSEGV/SIGBUS raised *inside this runtime's JIT code* by a
    // wild guest memory access: the host PC is looked up in the fault table
    // to recover the guest block address, then the interrupted context is
    // rewound to the trampoline's label_return_host so the block "returns"
    // HaltReason::PageFatal to the Runtime::Run loop — the faulting
    // instruction is never re-executed.
    static constexpr int kFaultPriority = 100;  // SMC handler will take 0.

    // SMC write-protect fault handler (SignalHandler chain, priority 0 —
    // ahead of the JIT guest-fault recovery). A guest store to a guest page
    // holding translated code faults on the write protection installed by
    // SmcTracker::RegisterNode. Direct-link generations are deactivated and
    // incoming branches restored synchronously before the page becomes RW;
    // dispatch clearing also happens here. Node detach and epoch reclamation
    // remain deferred to CloseWriteWindow after the current JitRun returns.
    static constexpr int kSmcPriority = 0;

    void RestoreHostFPCRForSignal() const {
        if (!jit_guest_fpcr_active.load(std::memory_order_acquire)) {
            return;
        }
        backend::arm64::WriteNativeFPCR(jit_host_fpcr.load(std::memory_order_relaxed));
    }

    static bool HandleSmcFault(void* ctx, ucontext_t* uctx, int sig, siginfo_t* info) {
        (void) ctx;
        // The owner slot covers host code between JitRuns (syscall emulation,
        // thread teardown); tls_active_runtime is the same object while guest
        // code runs. Both resolve to this thread's AddressSpace, which is all
        // HandleWriteFault needs.
        auto* self = static_cast<Impl*>(tls_active_runtime);
        if (!self && tls_owner_slot) {
            self = static_cast<Impl*>(tls_owner_slot->impl.load(std::memory_order_acquire));
        }
        if (!self) return false;
        // Signal callbacks are C++ host code. A guest/SMC fault may interrupt
        // generated code, a conservative helper already using host FPCR, or
        // an FPCR-transparent helper deliberately still using guest FPCR.
        // Restore is idempotent for the conservative helper and mandatory for
        // the transparent one; sigreturn restores the interrupted ucontext.
        self->RestoreHostFPCRForSignal();
        if (sig != SIGSEGV && sig != SIGBUS) {
            return false;
        }
        const auto fault_addr = reinterpret_cast<std::uintptr_t>(info->si_addr);
        const bool handled = self->address_space->GetSmcTracker().HandleWriteFault(
                *self->address_space, self->l1_code_cache, fault_addr);
        if (handled && self == tls_active_runtime && self->return_stack) {
            (void)self->return_stack->Reset(uctx);
        }
        return handled;
    }

    static bool HandleFault(void* ctx, ucontext_t* uctx, int sig, siginfo_t* info) {
        (void) ctx;
        auto* self = static_cast<Impl*>(tls_active_runtime);
        if (!self) return false;
        self->RestoreHostFPCRForSignal();
        if (sig != SIGSEGV && sig != SIGBUS) {
            return false;  // SIGILL in JIT code is a host codegen bug: crash
        }
        if (self->execution_trace_enabled) {
            self->DumpExecutionTrace(uctx, sig, info);
        }
        const auto host_pc = reinterpret_cast<u8*>(backend::SignalHandler::GetContextPC(uctx));
        const auto fault_addr = reinterpret_cast<std::uintptr_t>(info->si_addr);
        backend::FaultEntry entry{};
        if (self->state_storage.Contains(fault_addr)) {
            const auto request = std::atomic_ref<u64>(self->state->exit_request)
                                         .load(std::memory_order_acquire);
            if (request == 0 ||
                !self->address_space->LookupFault(host_pc, entry) ||
                !entry.recovery) {
                return false;
            }
            self->state->current_loc = ir::Location(entry.guest_loc);
            backend::SignalHandler::SetContextPC(
                    uctx, reinterpret_cast<std::uintptr_t>(entry.recovery));
            return true;
        }
        if (GetInterruptL1Mapping().Contains(fault_addr)) {
            const auto request = std::atomic_ref<u64>(self->state->exit_request)
                                         .load(std::memory_order_acquire);
            if (self->execution_trace_enabled) {
                SignalDiagnostic diagnostic("[svm-il1]");
                diagnostic.Field("fa", fault_addr);
                diagnostic.Field("pc", reinterpret_cast<std::uintptr_t>(host_pc));
                diagnostic.Field("req", request);
                diagnostic.Field("l1", reinterpret_cast<std::uintptr_t>(self->state->indirect_l1_code_cache));
                diagnostic.Field("il1", reinterpret_cast<std::uintptr_t>(GetInterruptL1Mapping().Data()));
                diagnostic.Field("i_call", reinterpret_cast<std::uintptr_t>(self->state->indirect_call_l1_code_cache));
                diagnostic.Field("p_call", reinterpret_cast<std::uintptr_t>(self->state->pending_call_l1_code_cache));
                diagnostic.Write();
            }
            if ((request & kBackedgeSignalRequest) != 0) {
                const bool has_entry =
                        self->address_space->LookupFault(host_pc, entry);
                const auto recovery_pc = has_entry && entry.recovery
                        ? reinterpret_cast<std::uintptr_t>(entry.recovery)
                        : reinterpret_cast<std::uintptr_t>(
                                  self->address_space->GetTrampolines()
                                          .GetReturnHost());
                backend::SignalHandler::SetContextPC(uctx, recovery_pc);
                return true;
            }
            if (self->state->indirect_l1_code_cache ==
                        GetInterruptL1Mapping().Data()) {
                return false;
            }
            const auto offset = fault_addr - reinterpret_cast<std::uintptr_t>(
                    GetInterruptL1Mapping().Data());
            if (offset % sizeof(TranslateEntry) != 0) {
                return false;
            }
            const auto index = offset / sizeof(TranslateEntry);
            if (index >= (size_t{1} << l1_cache_bits)) {
                return false;
            }
            auto* entries = static_cast<TranslateEntry*>(
                    self->state->indirect_l1_code_cache);
            const auto target = std::atomic_ref<size_t>(entries[index].key)
                                        .load(std::memory_order_acquire);
            self->state->current_loc = ir::Location(target);
            backend::SignalHandler::SetContextPC(
                    uctx,
                    reinterpret_cast<std::uintptr_t>(
                            self->address_space->GetTrampolines()
                                    .GetIndirectL1Miss()));
            return true;
        }
        bool has_entry = host_pc &&
                self->address_space->LookupFault(host_pc, entry);
        if (!has_entry && !host_pc && fault_addr == 0) {
            const auto link_register =
                    backend::SignalHandler::GetContextGPR(uctx, 30);
            if (link_register >= sizeof(u32)) {
                has_entry = self->address_space->LookupFault(
                        reinterpret_cast<u8*>(link_register - sizeof(u32)),
                        entry);
                has_entry &=
                        entry.recovery_kind == backend::FaultRecoveryKind::IndirectCallMiss ||
                        entry.recovery_kind == backend::FaultRecoveryKind::ExternalContinuation;
            }
        }
        if (!has_entry) {
            return false;
        }
        if (entry.recovery_kind == backend::FaultRecoveryKind::IndirectCallMiss ||
            entry.recovery_kind == backend::FaultRecoveryKind::ExternalContinuation) {
            if (fault_addr != 0 || !entry.recovery) {
                return false;
            }
            backend::SignalHandler::SetContextPC(
                    uctx, reinterpret_cast<std::uintptr_t>(entry.recovery));
            return true;
        }
        if (self->return_stack && self->return_stack->Recover(uctx, fault_addr)) {
            return true;
        }
        if (backend::SignalHandler::IsGuestAddressMapped(fault_addr)) {
            // The faulting page IS mapped for the guest: this is a protection
            // violation (SMC write-protect, Phase 4) or a host bug, not a
            // wild guest pointer. Let a higher-priority handler (or the
            // default crash handler) deal with it.
            return false;
        }
        // Recover the guest context: resume the guest at the faulting block's
        // entry and report PageFatal through the normal halt path.
        self->state->current_loc = ir::Location(entry.guest_loc);
        // The recovery entry reloads this value into its configuration-specific
        // halt register before saving statically allocated guest registers.
        // This preserves guest RSI in x0 at pin levels 2/3 while still returning
        // PageFatal in the C-ABI result register after the static spill.
        self->state->halt_reason = HaltReason::PageFatal;
        const auto recovery_pc = entry.recovery
                                         ? reinterpret_cast<std::uintptr_t>(entry.recovery)
                                         : reinterpret_cast<std::uintptr_t>(
                                                   self->address_space->GetTrampolines()
                                                           .GetReturnHost());
        backend::SignalHandler::SetContextPC(uctx, recovery_pc);
        return true;
    }

    void DumpExecutionTrace(const ucontext_t* uctx,
                            int sig,
                            const siginfo_t* info) const {
        const auto next = execution_trace.next.load(std::memory_order_acquire);
        const auto count = std::min<u64>(next, backend::kExecutionTraceEntryCount);
        SignalDiagnostic diagnostic("[svm-exec-trace]");
        diagnostic.Field("sig", sig);
        diagnostic.Field("host_pc", backend::SignalHandler::GetContextPC(uctx));
        diagnostic.Field("fault_addr", reinterpret_cast<std::uintptr_t>(info ? info->si_addr : nullptr));
        diagnostic.Field("guest_target", state->current_loc.Value());
        diagnostic.Field("live_rsp", backend::SignalHandler::GetContextGPR(uctx, 19));
        diagnostic.Field("host_lr", backend::SignalHandler::GetContextGPR(uctx, 30));
        diagnostic.Field("next", next);
        diagnostic.Write();
        for (u64 sequence = next - count; sequence < next; ++sequence) {
            const auto& entry = execution_trace.entries[
                    sequence & (backend::kExecutionTraceEntryCount - 1)];
            SignalDiagnostic row("[svm-exec-trace]");
            row.Field("sequence", sequence);
            row.Field("guest_rip", entry.guest_rip);
            row.Field("guest_rsp", entry.guest_rsp);
            row.Write();
        }
    }

    void SetLocation(LocationDescriptor location) const {
        state->current_loc = ir::Location(location);
    }

    [[nodiscard]] LocationDescriptor GetLocation() const { return state->current_loc.Value(); }

    [[nodiscard]] HaltReason JitRun(void* cache) const { return jit_entry(state, cache); }

    [[nodiscard]] u64 LoadExitRequest() const {
        return std::atomic_ref<u64>(state->exit_request)
                .load(std::memory_order_acquire);
    }

    void AcknowledgeSmcRequest(u64 observed) const {
        if ((observed & kBackedgeSmcRequestMask) == 0) {
            return;
        }
        auto request = std::atomic_ref<u64>(state->exit_request);
        u64 expected = observed;
        const u64 desired = observed & kBackedgeSignalRequest;
        // Clear only the exact SMC generation seen before CloseWriteWindow.
        // A concurrent invalidation changes the low counter, makes this CAS
        // fail, and is therefore observed by the next boundary/backedge.
        if (request.compare_exchange_strong(expected,
                                            desired,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            state_storage.DisarmIfIdle(&state->exit_request);
        }
    }

    [[nodiscard]] HaltReason Interpreter() const {
        auto current_loc = state->current_loc.Value();
        auto current_module{address_space->GetModule(current_loc)};

        if (!current_module) {
            return HaltReason::CodeMiss | HaltReason::ModuleMiss;
        }
        // The IR interpreter is the execution engine for enable_jit=false. It
        // is NOT a fallback for a dispatch-table miss while the JIT is on: the
        // IR held by the module has been through the JIT pipeline and is no
        // longer valid input for it. Two concrete ways that goes wrong, both
        // observed:
        //  - UniformEliminationPass rewrites reads of a statically allocated
        //    uniform into GetHostGPR (Config::buffers_static_alloc; the x86
        //    frontend pins guest rbx/rsp/rbp into x20/x19/x21). The
        //    interpreter has no host-register file and RunGetHostGPR yields 0,
        //    so `ret` pops from guest address 0 and the guest thread dies of
        //    PageFatal.
        //  - Terminals that mean "go back to the dispatcher"
        //    (ReturnToDispatch/Invalid/PopRSBHint, and a Switch with no
        //    matching case) return HaltReason::None without advancing
        //    current_loc. The loop below has no dispatch step, so it re-runs
        //    the same block forever.
        // Neither is reachable without SMC: a dispatch slot is only ever
        // zeroed while its module node is still live by SmcTracker's write
        // fault (ClearDispatchSlots runs in the signal handler, DetachNode
        // only later in CloseWriteWindow). Report CodeMiss instead so the
        // translator recompiles and republishes the unit -- exactly what
        // already happens when the node itself is gone.
        if (address_space->GetConfig().enable_jit) {
            return HaltReason::CodeMiss;
        }

        HaltReason hr{HaltReason::None};
        IntrusivePtr<ir::Function> active_function{};

        while (hr == HaltReason::None) {
            // Re-read the location every iteration: interpreted blocks update
            // state->current_loc through their terminals (LinkBlock /
            // SetLocation), so the dispatcher must follow it like the JIT
            // trampolines code_dispatcher loop does.
            current_loc = state->current_loc.Value();
            if (active_function) {
                auto read_lock = active_function->LockRead();
                if (auto* block = active_function->FindBlock(current_loc)) {
                    backend::interp::Interpreter interpreter{*state, block};
                    hr = interpreter.Run();
                    continue;
                }
                active_function = {};
            }
            if (auto node = current_module->GetNode(current_loc); !backend::IsEmpty(node)) {
                hr = VisitVariant<HaltReason>(
                        node, [this, &active_function, current_loc](auto x) -> auto {
                    using T = std::decay_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, IntrusivePtr<ir::Function>>) {
                        auto read_lock = x->LockRead();
                        auto current_block = x->FindBlock(current_loc);
                        if (!current_block) return HaltReason::CodeMiss;
                        active_function = x;
                        backend::interp::Interpreter interpreter{*state, current_block};
                        return interpreter.Run();
                    } else if constexpr (std::is_same_v<T, IntrusivePtr<ir::Block>>) {
                        auto read_lock = x->LockRead();
                        backend::interp::Interpreter interpreter{*state, x.get()};
                        return interpreter.Run();
                    } else {
                        return HaltReason::CodeMiss;
                    }
                });
            } else {
                hr = HaltReason::CodeMiss;
            }
        }
        // Mirror the JIT trampoline's label_return_host: the halt reason is
        // consumed by the host loop, so clear it so the next entry (and any
        // CheckHalt terminal) starts from a clean state.
        state->halt_reason = HaltReason::None;
        return hr;
    }

    [[nodiscard]] HaltReason Run() const {
        if (!exec_profile_started) {
            exec_profile_start = std::chrono::steady_clock::now();
            exec_profile_started = true;
        }
        // Publish this runtime to the host signal handler chain and make sure
        // this thread has an alternate signal stack while guest code runs.
        backend::SignalHandler::InstallThreadAltStack();
        struct ActiveGuard {
            void* prev;
            explicit ActiveGuard(Impl* self) : prev(tls_active_runtime) {
                tls_active_runtime = self;
            }
            ~ActiveGuard() { tls_active_runtime = prev; }
        } guard{const_cast<Impl*>(this)};
        const auto& config = address_space->GetConfig();
        const backend::GuestMemoryScope guest_memory{
                config.memory_base, config.guest_addr_mask};

        HaltReason hr{HaltReason::None};
        while (running.load(std::memory_order_acquire)) {
            auto current_loc = GetLocation();
            auto& smc = address_space->GetSmcTracker();
            if (address_space->ExitLatchEnabled()) {
                const u64 request = LoadExitRequest();
                if (request & kBackedgeSmcRequestMask) {
                    // Covers an SMC write made by host syscall emulation
                    // between JitRuns: slots are already clear, and closing
                    // before cache lookup prevents entry into/disk revival of
                    // a node from the open write window.
                    smc.CloseWriteWindow(*address_space, l1_code_cache);
                    AcknowledgeSmcRequest(request);
                }
            }
            // Publish before cache lookup, not merely before JitRun: a pointer
            // fetched while an invalidation races must remain epoch-protected
            // until the trampoline returns.
            smc.BeginJit(smc_epoch);
            if (auto cache = hr != HaltReason::CacheMiss ? address_space->GetCodeCache(current_loc)
                                                         : nullptr;
                cache) {
                // JIT Run!
                const bool manage_guest_fpcr =
                        address_space->GetConfig().sse_afp_nan;
                if (manage_guest_fpcr) {
                    jit_host_fpcr.store(backend::arm64::ReadNativeFPCR(),
                                        std::memory_order_relaxed);
                    jit_guest_fpcr_active.store(true, std::memory_order_release);
                }
                hr = JitRun(cache);
                if (manage_guest_fpcr) {
                    jit_guest_fpcr_active.store(false, std::memory_order_release);
                }
                // Read after the generated return. If a newer invalidation
                // arrived after its poll, CloseWriteWindow below consumes it
                // too. If one arrives after this load, the exact-generation
                // CAS fails and the next boundary observes it.
                const u64 observed = address_space->ExitLatchEnabled()
                        ? LoadExitRequest()
                        : 0;
                // The runtime is now quiescent with respect to retired JIT
                // code. This is an atomic-only fast path unless reclamation
                // work is actually pending.
                smc.EndJit(smc_epoch);
                // SMC write-window close (Phase 4): if a guest store hit a
                // write-protected code page during this JitRun, the stale
                // translations are invalidated now — the guest is back on
                // the host side, so freeing JIT code and editing module
                // maps is safe. Runs before the hr checks so it also covers
                // the CodeMiss/CacheMiss exits. Direct links have already
                // been restored by the signal-window transaction before any
                // retired allocation can become reclaimable.
                smc.CloseWriteWindow(*address_space, l1_code_cache);
                if (address_space->ExitLatchEnabled()) {
                    AcknowledgeSmcRequest(observed);
                    // A Signal may race after the generated poll selected an
                    // SMC-only CodeMiss veneer. The sticky atomic bit, not
                    // the non-atomic halt_reason sampled by the trampoline,
                    // is authoritative at this boundary.
                    if (LoadExitRequest() & kBackedgeSignalRequest) {
                        hr = HaltReason::Signal;
                    }
                }
            } else {
                smc.EndJit(smc_epoch);
                // IR Interpreter
                hr = Interpreter();
            }
            if (hr == HaltReason::CacheMiss) {
                continue;
            } else {
                break;
            }
        }
        // SignalInterrupt() is the only writer that clears running. If the
        // interrupt lands between two Run() calls, the loop above is skipped;
        // returning None would make the consumer retry Run() forever without
        // reaching ClearInterrupt(). Preserve the interrupt as the only valid
        // reason for this empty-loop exit. CacheMiss keeps its existing
        // in-loop continue semantics.
        if (hr == HaltReason::None &&
            !running.load(std::memory_order_acquire)) {
            return HaltReason::Signal;
        }
        return hr;
    }

    Instance* instance{};
    backend::InterruptPollState state_storage;
    backend::State* state{};
    std::optional<backend::GuardedReturnStack> return_stack{};
    backend::AddressSpace* address_space{};
    // mutable: JIT dispatch fills this per-Runtime table even from const Run
    // paths; State publishes its stable Data() base to generated code.
    mutable TranslateTable l1_code_cache{
            l1_cache_bits, TranslateTableHash::Direct};
    backend::SmcTracker::RuntimeToken smc_epoch{};
    // Kept alive alongside the creating thread's tls_owner_slot; see OwnerSlot.
    std::shared_ptr<OwnerSlot> owner_slot{};
    backend::interp::InterpStack interp_stack;
    std::atomic_bool running{true};
    backend::Trampolines::RuntimeEntry jit_entry{};
    backend::RuntimeProfileInterface profile_interface{};
    backend::ExecutionTraceBuffer execution_trace{};
    std::vector<u64> hot_coalesce_counters{};
    bool hot_coalesce_enabled{};
    bool indirect_l1_prof_enabled{};
    bool flags_regs_audit_enabled{};
    bool hot_counter_storage_enabled{};
    bool execution_trace_enabled{};
    mutable bool exec_profile_started{};
    mutable std::chrono::steady_clock::time_point exec_profile_start{};
    // Signal handlers run on the interrupted guest thread and cannot recover
    // the runtime-entry FPCR slot from an arbitrary direct-helper stack frame.
    // Keep a lock-free copy alongside an active marker for that boundary.
    mutable std::atomic<u64> jit_host_fpcr{};
    mutable std::atomic_bool jit_guest_fpcr_active{false};

    void PublishIndirectL1Base(void* base) {
        std::atomic_ref<void*>(state->indirect_l1_code_cache)
                .store(base, std::memory_order_release);
    }
};

Runtime::Runtime(Instance* instance)
        : impl(std::make_unique<Impl>(reinterpret_cast<backend::AddressSpace*>(instance))) {
    // Process-wide sigaction handlers (idempotent) + this runtime's entries
    // in the fault handler chain. The chain is process-global; both handlers
    // filter by the thread-local active runtime so only the executing
    // runtime claims a fault. SMC sorts first (priority 0), the JIT
    // guest-fault recovery second (priority 100).
    backend::SignalHandler::Install();
    // The callbacks route through tls_active_runtime, so one stable pair is
    // sufficient for every Runtime in the process. This also avoids modifying
    // the process-global handler array while another guest thread may be
    // handling a fault.
    static std::once_flag register_fault_handlers;
    std::call_once(register_fault_handlers, [] {
        backend::SignalHandler::RegisterHandler(
                &Impl::HandleSmcFault, nullptr, Impl::kSmcPriority);
        backend::SignalHandler::RegisterHandler(
                &Impl::HandleFault, nullptr, Impl::kFaultPriority);
    });
}

Runtime::~Runtime() = default;

HaltReason Runtime::Run() { return impl->Run(); }

HaltReason Runtime::Step() { return HaltReason::None; }

void Runtime::SignalInterrupt() {
    impl->running.store(false, std::memory_order_release);
    // The fault handler acquire-loads the request before it consumes Signal.
    impl->state->halt_reason = HaltReason::Signal;
    std::atomic_ref<u64>(impl->state->exit_request)
            .fetch_or(kBackedgeSignalRequest, std::memory_order_release);
    impl->PublishIndirectL1Base(GetInterruptL1Mapping().Data());
    std::atomic_ref<void*>(impl->state->indirect_call_l1_code_cache)
            .store(GetInterruptL1Mapping().Data(), std::memory_order_release);
    std::atomic_ref<void*>(impl->state->pending_call_l1_code_cache)
            .store(GetInterruptL1Mapping().Data(), std::memory_order_release);
    impl->state_storage.Arm();
}

void Runtime::ClearInterrupt() {
    impl->state->halt_reason = HaltReason::None;
    if (impl->return_stack) {
        impl->state->rsb_pointer = impl->return_stack->Empty();
    }
    impl->PublishIndirectL1Base(impl->l1_code_cache.Data());
    std::atomic_ref<void*>(impl->state->indirect_call_l1_code_cache)
            .store(impl->address_space->GetCallCodeCacheTable().Data(),
                   std::memory_order_release);
    std::atomic_ref<void*>(impl->state->pending_call_l1_code_cache)
            .store(impl->address_space->GetPendingCallCodeCacheTable().Data(),
                   std::memory_order_release);
    std::atomic_ref<u64>(impl->state->exit_request)
            .fetch_and(kBackedgeSmcRequestMask, std::memory_order_acq_rel);
    impl->state_storage.DisarmIfIdle(&impl->state->exit_request);
    impl->running.store(true, std::memory_order_release);
}

void Runtime::SetLocation(LocationDescriptor location) { impl->SetLocation(location); }

LocationDescriptor Runtime::GetLocation() { return impl->GetLocation(); }

backend::State* Runtime::GetState() const {
    return impl->state;
}

std::span<u8> Runtime::GetUniformBuffer() const {
    return {(u8*) &impl->state->uniform_buffer_begin,
            impl->address_space->GetConfig().uniform_buffer_size};
}

void Runtime::DumpExecutionTrace() const {
    if (!impl->execution_trace_enabled) return;
    const auto& trace = impl->execution_trace;
    const u64 next = trace.next.load(std::memory_order_acquire);
    const u64 count = std::min<u64>(next, backend::kExecutionTraceEntryCount);
    SVM_DIAG_PRINT(Runtime, "[sys-trace] dumping %llu recent block entries:\n",
                 static_cast<unsigned long long>(count));
    for (u64 sequence = next - count; sequence < next; ++sequence) {
        const auto& entry = trace.entries[
                sequence & (backend::kExecutionTraceEntryCount - 1)];
        SVM_DIAG_PRINT(Runtime, "[sys-trace] #%llu rip=%#llx rsp=%#llx\n",
                     static_cast<unsigned long long>(sequence),
                     static_cast<unsigned long long>(entry.guest_rip),
                     static_cast<unsigned long long>(entry.guest_rsp));
    }
}

namespace backend {

namespace {

// JIT disk cache hook (backend/jit_cache.h). Off unless SVM_JIT_CACHE is set;
// the unit is described by its guest block ranges plus the offset of each
// block's entry inside the emitted buffer.
void RecordJitCacheUnit(const std::shared_ptr<backend::Module>& module,
                        VAddr guest_start,
                        bool is_function,
                        const std::vector<SerialBlock>& blocks,
                        const CodeBuffer& buffer,
                        const backend::arm64::JitContext& context,
                        std::span<const backend::arm64::JitTranslator* const>
                                translators) {
    auto* cache = module->GetAddressSpace().GetJitDiskCache();
    if (!cache) {
        return;
    }
    std::vector<SerialLinkSite> link_sites;
    link_sites.reserve(context.GetDirectLinkSites().size());
    for (const auto& site : context.GetDirectLinkSites()) {
        link_sites.push_back({site.code_offset,
                              site.guest_target,
                              static_cast<u8>(site.kind),
                              site.flags_bypass.code_offset,
                              site.flags_bypass.resume_offset,
                              site.flags_bypass_instruction,
                              site.flags_bypass.linked_instruction,
                              site.flags_bypass.merge_branch_offset,
                              site.flags_bypass.edge_flags});
    }
    std::vector<SerialFaultSite> fault_sites;
    for (const auto* translator : translators) {
        ASSERT(translator);
        fault_sites.reserve(fault_sites.size() +
                            translator->GetFaultMetadata().size());
        for (const auto& fault : translator->GetFaultMetadata()) {
            fault_sites.push_back({fault.guest_start,
                                   fault.host_begin,
                                   fault.host_end,
                                   fault.recovery_offset == 0
                                           ? UINT32_MAX
                                           : fault.recovery_offset,
                                   static_cast<u8>(fault.recovery_kind)});
        }
    }
    cache->RecordUnit(module,
                      guest_start,
                      is_function,
                      buffer.exec_data,
                      buffer.rw_data,
                      static_cast<u32>(buffer.size),
                      blocks,
                      link_sites,
                      fault_sites);
}

void RecordJitCacheUnit(const std::shared_ptr<backend::Module>& module,
                        VAddr guest_start,
                        bool is_function,
                        const std::vector<SerialBlock>& blocks,
                        const CodeBuffer& buffer,
                        const backend::arm64::JitContext& context,
                        const backend::arm64::JitTranslator& translator) {
    const backend::arm64::JitTranslator* translators[]{&translator};
    RecordJitCacheUnit(module,
                       guest_start,
                       is_function,
                       blocks,
                       buffer,
                       context,
                       translators);
}

void PublishIndirectL1Faults(
        const std::shared_ptr<backend::Module>& module,
        const CodeBuffer& buffer,
        const backend::arm64::JitTranslator& translator) {
    for (const auto& fault : translator.GetFaultMetadata()) {
        module->AddFaultEntry(buffer.exec_data + fault.host_begin,
                              buffer.exec_data + fault.host_end,
                              fault.guest_start,
                              buffer.exec_data,
                              fault.recovery_offset
                                      ? buffer.exec_data + fault.recovery_offset
                                      : nullptr,
                              fault.recovery_kind);
    }
}

// PassPipeline::BuildDefault builds a nine-entry vector of std::function
// objects. It was rebuilt for every compiled unit even though its only input,
// the address space's UniformInfo, is fixed for the life of that address space
// -- so under lazy function compilation it ran once per decoded guest block.
//
// Cached per thread and keyed on the UniformInfo pointer rather than in a
// basic static: an embedder can hold several Instances with different uniform
// layouts, and a mismatched pipeline would silently apply the wrong uniform
// map. A key change rebuilds; in the single-address-space case that never
// happens after the first unit.
const ir::PassPipeline& GetPassPipeline(const ir::UniformInfo* uni_info) {
    static thread_local ir::PassPipeline cached{};
    static thread_local const ir::UniformInfo* cached_key{};
    static thread_local bool cached_valid{false};
    if (!cached_valid || cached_key != uni_info) {
        cached = ir::PassPipeline::BuildDefault(uni_info);
        cached_key = uni_info;
        cached_valid = true;
    }
    return cached;
}

size_t PrepareFunctionGuestRanges(ir::HIRFunction* function) {
    size_t decoded_blocks = 0;
    VAddr max_end = function->GetFunction()->GetStartLocation().Value();
    for (auto* hir_block : function->GetHIRBlocks()) {
        if (!hir_block || hir_block == function->GetEntryBlock()) {
            continue;
        }
        auto* block = hir_block->GetBlock();
        if (block->GetInstList().empty()) {
            continue;
        }
        ++decoded_blocks;
        u64 block_size = 0;
        for (auto& inst : block->GetInstList()) {
            if (inst.GetOp() == ir::OpCode::AdvancePC) {
                block_size += inst.GetArg<ir::Imm>(0).Get();
            }
        }
        const VAddr block_start = block->GetStartLocation().Value();
        // A one-instruction terminal block may not retain an AdvancePC: the
        // assembler closes the block before the decoder's trailing
        // AdvancePC. Never leave the constructor's zero end as an unsigned
        // wraparound range; tracking the start byte still protects the
        // containing host page.
        block->SetEndLocation(ir::Location(block_start + std::max<u64>(block_size, 1)));
        max_end = std::max(max_end, block->GetEndLocation().Value());
    }
    function->GetFunction()->SetEndLocation(ir::Location(max_end));
    return decoded_blocks;
}

// Escape hatch for the release added at the end of TranslateIR(HIRFunction*).
// `SVM_FUNC_IR_FREE=0` restores the old behaviour (function-mode IR retained
// for the lifetime of the compiled unit), which is how the two sides of the
// memory measurement are produced from one binary.
bool FuncIRFreeEnabled() {
    return GetSvmConfig().func_ir_free;
}

}  // namespace

bool PublishIRFunction(const std::shared_ptr<backend::Module>& module,
                       ir::HIRFunction* function) {
    PrepareFunctionGuestRanges(function);
    if (!module->Push(function->GetFunction())) {
        return false;
    }
    function->ReleaseFunctionOwnership();
    return true;
}

static FeatureSet ResolveBackendFeatures(const std::shared_ptr<backend::Module>& module) {
    auto features = backend::ResolveFeatureSet(module->GetModuleConfig());
    // induction tie 已由 Config 承载，本期保持既有所有权；FeatureSet 中保留
    // 同名槽位只是为了与 B 类字段表一一对应，P3 再统一去重。
    features.induct_tie = module->GetAddressSpace().GetConfig().induct_tie;
    return features;
}

struct PreparedFunctionRegion final {
    ir::HIRFunction* function{};
    ir::Function* ir_function{};
    VAddr guest_start{};
    size_t decoded_blocks{};
    backend::RegAlloc* reg_alloc{};
};

struct FunctionRegionAllocation final {
    std::optional<backend::RegAlloc> baseline;
    std::unique_ptr<backend::RegAlloc> hoisted;
};

PreparedFunctionRegion PrepareFunctionRegion(const std::shared_ptr<backend::Module>& module,
                                             ir::HIRFunction* function,
                                             const FeatureSet& features,
                                             PerfFixedCapture2& fixed_capture,
                                             FunctionRegionAllocation& allocation) {
    PreparedFunctionRegion prepared{
            .function = function,
            .ir_function = function->GetFunction(),
            .guest_start = function->GetFunction()->GetStartLocation().Value(),
    };
    PerfScope2 perf_prepare{GetPerfStats2().publish_prepare};
    prepared.decoded_blocks = PrepareFunctionGuestRanges(function);
    perf_prepare.Stop();

    PerfScope perf_rpo{GetPerfStats().rpo_ns};
    PerfScope2 perf_compute_rpo{GetPerfStats2().compute_rpo};
    function->ComputeRPO();
    perf_compute_rpo.Stop();
    PerfScope2 perf_id_pre{GetPerfStats2().id_rpo_pre};
    function->IdByRPO();
    perf_id_pre.Stop();
    perf_rpo.Stop();

    const bool dump_ir = GetSvmConfig().dump_ir;
    if (dump_ir) {
        SVM_DIAG_FORMAT(Runtime, "[func-compile] {:#x} rpo-ready\n", prepared.guest_start);
    }
    const auto& address_space = module->GetAddressSpace();
    const ir::UniformInfo* uni_info =
            address_space.GetUniformInfo().uniform_size ? &address_space.GetUniformInfo() : nullptr;
    PerfScope perf_opt{GetPerfStats().opt_ns};
    GetPassPipeline(uni_info).RunFunction(
            function, module->GetModuleConfig().optimizations, features);
    perf_opt.Stop();

    PerfScope perf_rpo2{GetPerfStats().rpo_ns};
    PerfScope2 perf_id_post{GetPerfStats2().id_rpo_post};
    function->IdByRPO();
    perf_id_post.Stop();
    perf_rpo2.Stop();
    if (dump_ir) {
        SVM_DIAG_FORMAT(Runtime, "[func-compile] {:#x} opts-ready\n", prepared.guest_start);
    }

    auto gprs{address_space.GetTrampolines().GetGPRRegs()};
    auto fprs{address_space.GetTrampolines().GetFPRRegs()};
    PerfScope perf_ra{GetPerfStats().regalloc_ns};
    PerfScope2 perf_ra_detail{GetPerfStats2().regalloc_total};
    allocation.baseline.emplace(static_cast<u32>(function->MaxInstrCount()),
                                gprs,
                                fprs,
                                features,
                                address_space.GetConfig().sse_afp_nan);
    ir::RegisterAllocPass::RunWithScalarInsert(
            function, &*allocation.baseline, address_space.GetConfig().sse_scalar_insert, features);
    prepared.reg_alloc = &*allocation.baseline;
    std::unique_ptr<ir::LoopInvariantHoistRecipe> hoist_recipe;
    if (uni_info && (features.loop_gpr_hoist || features.loop_const_hoist)) {
        hoist_recipe = ir::LoopInvariantHoistRecipe::Analyze(function, *uni_info, features);
    }
    if (hoist_recipe && !hoist_recipe->Empty()) {
        hoist_recipe->Apply();
        function->IdByRPO();
        allocation.hoisted =
                std::make_unique<backend::RegAlloc>(static_cast<u32>(function->MaxInstrCount()),
                                                    gprs,
                                                    fprs,
                                                    features,
                                                    address_space.GetConfig().sse_afp_nan);
        ir::RegisterAllocPass::RunWithScalarInsert(function,
                                                   allocation.hoisted.get(),
                                                   address_space.GetConfig().sse_scalar_insert,
                                                   features);
        if (allocation.hoisted->SpillCount() <= allocation.baseline->SpillCount()) {
            prepared.reg_alloc = allocation.hoisted.get();
        } else {
            hoist_recipe->Revert();
            function->IdByRPO();
        }
    }
    perf_ra_detail.Stop();
    perf_ra.Stop();
    fixed_capture.Record(static_cast<unsigned>(prepared.decoded_blocks));
    if (dump_ir) {
        SVM_DIAG_FORMAT(Runtime, "[func-compile] {:#x} regalloc-ready\n", prepared.guest_start);
    }
    return prepared;
}

static void* TranslatePreparedFunctionRegions(const std::shared_ptr<backend::Module>& module,
                                              std::span<PreparedFunctionRegion> prepared_regions) {
    ASSERT(!prepared_regions.empty());
    auto& prepared = prepared_regions.front();
    auto* function = prepared.function;
    auto* ir_function = prepared.ir_function;
    const auto func_start = prepared.guest_start;
    for (size_t i = 1; i < prepared_regions.size(); ++i) {
        if (!ir_function->TakeBlocksFrom(*prepared_regions[i].ir_function)) {
            return nullptr;
        }
    }
    auto& jit_state = ir_function->GetJitCache();
    const bool dump_ir = GetSvmConfig().dump_ir;
    PerfScope perf_cg{GetPerfStats().codegen_ns};
    PerfScope2 perf_cg_detail{GetPerfStats2().codegen_total};
    std::array<backend::arm64::FunctionRegionEmission, 1> single_region{};
    std::vector<backend::arm64::FunctionRegionEmission> region_storage;
    std::span<const backend::arm64::FunctionRegionEmission> regions;
    if (prepared_regions.size() == 1) {
        single_region.front() = {
                .function = function,
                .reg_alloc = prepared.reg_alloc,
        };
        regions = single_region;
    } else {
        region_storage.reserve(prepared_regions.size());
        for (auto& item : prepared_regions) {
            region_storage.push_back({
                    .function = item.function,
                    .reg_alloc = item.reg_alloc,
            });
        }
        regions = region_storage;
    }
    backend::arm64::FunctionCodeObjectEmitter emitter{module, regions};
    emitter.Emit();
    perf_cg_detail.Stop();
    perf_cg.Stop();
    if (dump_ir) SVM_DIAG_FORMAT(Runtime, "[func-compile] {:#x} emit-ready\n", func_start);
    auto buffer_size = emitter.CurrentBufferSize();
    if (dump_ir) SVM_DIAG_FORMAT(Runtime, "[func-compile] {:#x} size={}\n", func_start, buffer_size);
    PerfScope2 perf_pub_total{GetPerfStats2().publish_total};
    PerfScope2 perf_pub_alloc{GetPerfStats2().publish_alloc};
    auto* emitted_context = &emitter.Context();
    auto allocation = module->AllocCodeCache(
            buffer_size, emitter.RequiresRegionTrampoline());
    if (allocation.first == backend::INVALID_CACHE_ID &&
        emitter.RequiresRegionTrampoline()) {
        // A unit too large for a <=128MiB trampoline region must not become a
        // half-direct translation or fail solely because direct linking was selected.
        // Re-emit the entire unit with the legacy slot leaf and allocate it
        // under the existing unrestricted arena policy.
        emitter.Emit(false);
        emitted_context = &emitter.Context();
        buffer_size = emitter.CurrentBufferSize();
        allocation = module->AllocCodeCache(buffer_size, false);
    }
    perf_pub_alloc.Stop();
    if (auto [idx, buffer] = allocation;
        idx != backend::INVALID_CACHE_ID) {
        PerfScope perf_pub{GetPerfStats().publish_ns};
        PerfAdd(GetPerfStats().host_bytes, buffer_size);
        size_t ir_insts{};
        for (const auto& item : prepared_regions) {
            ir_insts += item.function->MaxInstrCount();
        }
        PerfAdd(GetPerfStats().ir_insts, ir_insts);
        if (PerfPerUnit()) {
            SVM_DIAG_FORMAT(Runtime,
                       "[svm-unit] pc={:#x} ir={} host={}\n",
                       func_start,
                       ir_insts,
                       buffer_size);
        }
        PerfScope2 perf_pub_flush{GetPerfStats2().publish_flush};
        (void)emitter.Flush(buffer);
        perf_pub_flush.Stop();
        if (dump_ir) SVM_DIAG_FORMAT(Runtime, "[func-compile] {:#x} flush-ready\n", func_start);
        jit_state.jit_state = backend::JitState::Cached;
        jit_state.cache_id = idx;
        jit_state.offset_in = buffer.offset;
        jit_state.cache_size = buffer.size;
        PerfScope2 perf_pub_module{GetPerfStats2().publish_module};
        const bool pushed = module->Push(ir_function);
        perf_pub_module.Stop();
        if (!pushed) {
            jit_state = {};
            module->DiscardLinkSource(buffer.exec_data);
            if (auto* cache = module->GetCodeCache(buffer.exec_data)) {
                cache->FreeCode(buffer.exec_data);
            }
            return nullptr;
        }
        function->ReleaseFunctionOwnership();
        if (dump_ir) SVM_DIAG_FORMAT(Runtime, "[func-compile] {:#x} publish-ready\n", func_start);
        // Publish every decoded block label, not only the function entry.
        // External links, RSB return targets, and code misses are allowed to
        // land at a basic-block boundary inside this compiled unit.
        auto& mutable_address_space = module->GetAddressSpace();
        const auto allocation_size = static_cast<u32>(buffer.size);
        backend::FunctionEntryPublisher entry_publisher{*module, buffer.exec_data, allocation_size};
        std::vector<backend::SerialBlock> cache_blocks;
        for (auto& item : prepared_regions) {
            auto* region_function = item.function;
            const auto canonical_terminal_entries =
                    backend::FunctionEntryContract::AnalyzeCanonicalTerminalEntries(
                            *region_function);
            for (auto& hir_block : region_function->GetHIRBlocksRPO()) {
                auto* block = hir_block.GetBlock();
                const auto guest = block->GetStartLocation().Value();
                const bool canonical_terminal = canonical_terminal_entries.contains(guest);
                if (block->GetInstList().empty() && !canonical_terminal) {
                    continue;
                }
                const auto offset = emitted_context->GetCodeOffset(guest);
                const auto direct_offset = emitted_context->GetDirectLinkCodeOffset(guest);
                const auto pending_flags_offset = emitted_context->GetPendingFlagsCodeOffset(guest);
                const auto pending_flags_contract =
                        emitted_context->GetPendingFlagsTargetContract(guest);
                const auto call_offset = emitted_context->GetCallCodeOffset(guest);
                const auto call_pending_flags_offset =
                        emitted_context->GetCallPendingFlagsCodeOffset(guest);
                const auto to_offset = [](ptrdiff_t value) {
                    return value < 0 ? backend::kInvalidFunctionEntryOffset
                                     : static_cast<u32>(value);
                };
                const auto entry_contract = backend::FunctionEntryContract::Build(
                        *region_function,
                        *block,
                        {
                                .canonical = to_offset(offset),
                                .direct_link = to_offset(direct_offset),
                                .pending_flags = to_offset(pending_flags_offset),
                                .continuation = to_offset(call_offset),
                                .pending_flags_continuation = to_offset(call_pending_flags_offset),
                        },
                        pending_flags_contract,
                        canonical_terminal);
                ASSERT(entry_contract.IsWellFormed(allocation_size));
                {
                    PerfScope2 perf_pub_l2{GetPerfStats2().publish_l2};
                    (void)entry_publisher.Publish(entry_contract);
                }
                cache_blocks.push_back({
                        .guest_start = entry_contract.Guest().Value(),
                        .guest_end = entry_contract.GuestEnd().Value(),
                        .code_offset = entry_contract.Canonical().code_offset,
                        .guest_bytes_hash = 0,
                        .direct_code_offset = entry_contract.DirectLink().code_offset,
                        .pending_flags_code_offset = entry_contract.PendingFlags().code_offset,
                        .pending_flags_contract = pending_flags_contract,
                        .entry_flags = static_cast<u8>(
                                entry_contract.Linkable() ? backend::SerialBlock::Linkable : 0),
                });
                if (!module->GetModuleConfig().read_only) {
                    PerfScope2 perf_pub_smc{GetPerfStats2().publish_smc};
                    mutable_address_space.GetSmcTracker().RegisterNode(
                            module,
                            ir_function,
                            entry_contract.Guest().Value(),
                            entry_contract.GuestEnd().Value());
                    for (const auto& dependency : entry_contract.Dependencies()) {
                        mutable_address_space.GetSmcTracker().RegisterNode(module,
                                                                           ir_function,
                                                                           dependency.start.Value(),
                                                                           dependency.end.Value());
                    }
                }
            }
        }
        std::vector<const backend::arm64::JitTranslator*> emitted_translators;
        std::vector<backend::arm64::JitTranslator::BackedgeBlockMetadata> backedges;
        emitted_translators.reserve(prepared_regions.size());
        for (size_t i = 0; i < prepared_regions.size(); ++i) {
            const auto& translator = emitter.Translator(i);
            emitted_translators.push_back(&translator);
            const auto& region_backedges = translator.GetBackedgeBlockMetadata();
            backedges.insert(backedges.end(), region_backedges.begin(), region_backedges.end());
        }
        {
            PerfScope2 perf_pub_fault{GetPerfStats2().publish_fault};
            if (mutable_address_space.ExitLatchEnabled()) {
                // Keep the legacy whole-unit entry as a committed-state
                // fallback for metadata gaps/cold stubs. Overlapping precise
                // entries below win in Module::LookupFault.
                module->AddFaultEntry(buffer.exec_data,
                                      buffer.exec_data + buffer.size,
                                      func_start,
                                      buffer.exec_data);
                for (size_t i = 0; i < cache_blocks.size(); ++i) {
                    const auto recovery = std::find_if(
                            backedges.begin(), backedges.end(), [&](const auto& item) {
                                return item.guest_start == cache_blocks[i].guest_start;
                            });
                    const u32 begin = recovery != backedges.end()
                            ? recovery->host_begin
                            : cache_blocks[i].code_offset;
                    const u32 end = recovery != backedges.end()
                            ? recovery->host_end
                            : (i + 1 < cache_blocks.size()
                                       ? cache_blocks[i + 1].code_offset
                                       : static_cast<u32>(buffer.size));
                    ASSERT(begin < end && end <= buffer.size);
                    module->AddFaultEntry(buffer.exec_data + begin,
                                          buffer.exec_data + end,
                                          cache_blocks[i].guest_start,
                                          buffer.exec_data,
                                          recovery != backedges.end() &&
                                                          recovery->recovery_offset
                                                  ? buffer.exec_data +
                                                            recovery->recovery_offset
                                                  : nullptr);
                }
            } else {
                module->AddFaultEntry(buffer.exec_data,
                                      buffer.exec_data + buffer.size,
                                      func_start);
            }
            for (const auto* translator : emitted_translators) {
                PublishIndirectL1Faults(module, buffer, *translator);
            }
        }
        if (dump_ir) SVM_DIAG_FORMAT(Runtime, "[func-compile] {:#x} entries-ready\n", func_start);
        {
            PerfScope2 perf_pub_disk{GetPerfStats2().publish_disk};
            RecordJitCacheUnit(module,
                               func_start,
                               true,
                               cache_blocks,
                               buffer,
                               *emitted_context,
                               emitted_translators);
        }

        // Release the function's IR. Block mode has always done this (the
        // Block::DestroyInstrs at the end of TranslateIR(IntrusivePtr<Block>));
        // function mode never did, so every instruction of every compiled
        // function stayed allocated until the unit was invalidated or the
        // module torn down -- i.e. for the whole process in the normal case.
        //
        // Nothing reads it again:
        //  - the host code is emitted and flushed, and the fault table, the L2
        //    dispatch slots, the SMC ranges and the disk-cache record are all
        //    written above from data that lives in the AddressNode (guest
        //    start/end) or in the JitCache, not in the instruction list;
        //  - Runtime::Impl::Interpreter refuses to run module IR whenever the
        //    JIT is on and returns CodeMiss instead (see the comment there);
        //    the enable_jit=false path never reaches this function, it
        //    publishes through PublishIRFunction and keeps its IR;
        //  - JitContext::Forward's `cur_function->FindBlock` only ever inspects
        //    the unit being emitted right now;
        //  - the AOT collector re-reads the published node's blocks, but only
        //    their guest start/end;
        //  - the disk-cache *load* path already publishes function nodes whose
        //    blocks hold no instructions at all (jit_cache.cpp), so this is a
        //    shape the rest of the runtime is required to handle anyway.
        //
        // The write lock mirrors the block path, which destroys under
        // ir_block->LockWrite(). It is uncontended here: Translate() holds the
        // frontend's coarse translate lock and the module read lock, so an
        // invalidation cannot be detaching this node concurrently.
        if (FuncIRFreeEnabled()) {
            // Outside publish_ns so that counter keeps meaning what it did.
            perf_pub.Stop();
            perf_pub_total.Stop();
            PerfScope perf_free{GetPerfStats().ir_free_ns};
            PerfScope2 perf_free_detail{GetPerfStats2().ir_free};
            auto ir_guard = ir_function->LockWrite();
            ir_function->DestroyInstrs();
        }
        return buffer.exec_data;
    }
    return nullptr;
}

void* TranslateIR(const std::shared_ptr<backend::Module>& module,
                  std::span<ir::HIRFunction* const> functions) {
    ASSERT(!functions.empty());
    const auto features = ResolveBackendFeatures(module);
    PerfFixedCapture2 fixed_capture;
    std::vector<std::unique_ptr<FunctionRegionAllocation>> allocations;
    std::vector<PreparedFunctionRegion> prepared_regions;
    allocations.reserve(functions.size());
    prepared_regions.reserve(functions.size());
    for (auto* function : functions) {
        auto allocation = std::make_unique<FunctionRegionAllocation>();
        prepared_regions.push_back(
                PrepareFunctionRegion(module, function, features, fixed_capture, *allocation));
        allocations.push_back(std::move(allocation));
    }
    return TranslatePreparedFunctionRegions(module, prepared_regions);
}

void* TranslateIR(const std::shared_ptr<backend::Module>& module, ir::HIRFunction* function) {
    const auto features = ResolveBackendFeatures(module);
    PerfFixedCapture2 fixed_capture;
    FunctionRegionAllocation allocation;
    auto prepared = PrepareFunctionRegion(module, function, features, fixed_capture, allocation);
    return TranslatePreparedFunctionRegions(module, std::span{&prepared, 1});
}

void* TranslateIR(const std::shared_ptr<backend::Module>& module, ir::HIRBlock* block) {
    auto ir_block = block->GetBlock();
    auto block_start = ir_block->GetStartLocation().Value();
    if (!module->Push(ir_block)) {
        return nullptr;
    }

    auto guard = ir_block->LockWrite();
    auto& jit_state = ir_block->GetJitCache();
    if (jit_state.jit_state == backend::JitState::Cached) {
        return module->GetJitCache(jit_state);
    }
    const auto& address_space = module->GetAddressSpace();
    const auto features = ResolveBackendFeatures(module);
    auto gprs{address_space.GetTrampolines().GetGPRRegs()};
    auto fprs{address_space.GetTrampolines().GetFPRRegs()};
    backend::RegAlloc reg_alloc{
            static_cast<u32>(block->MaxInstrCount()), gprs, fprs, features,
            address_space.GetConfig().sse_afp_nan};
    backend::arm64::JitContext context{module, reg_alloc};
    backend::arm64::JitTranslator translator{context};
    translator.Translate(block->GetBlock());
    auto buffer_size = context.CurrentBufferSize();
    backend::arm64::JitContext* emitted_context = &context;
    backend::arm64::JitTranslator* emitted_translator = &translator;
    std::optional<backend::arm64::JitContext> fallback_context;
    std::optional<backend::arm64::JitTranslator> fallback_translator;
    auto allocation = module->AllocCodeCache(buffer_size, context.RequiresRegionTrampoline());
    if (allocation.first == backend::INVALID_CACHE_ID && context.RequiresRegionTrampoline()) {
        fallback_context.emplace(module, reg_alloc, false);
        fallback_translator.emplace(*fallback_context);
        fallback_translator->Translate(block->GetBlock());
        emitted_context = &*fallback_context;
        emitted_translator = &*fallback_translator;
        buffer_size = emitted_context->CurrentBufferSize();
        allocation = module->AllocCodeCache(buffer_size, false);
    }
    if (auto [idx, buffer] = allocation;
        idx != backend::INVALID_CACHE_ID) {
        emitted_context->Flush(buffer);
        module->AddFaultEntry(buffer.exec_data, buffer.exec_data + buffer.size, block_start);
        if (module->GetAddressSpace().ExitLatchEnabled()) {
            for (const auto& item : emitted_translator->GetBackedgeBlockMetadata()) {
                module->AddFaultEntry(buffer.exec_data + item.host_begin,
                                      buffer.exec_data + item.host_end,
                                      item.guest_start,
                                      buffer.exec_data,
                                      item.recovery_offset
                                              ? buffer.exec_data + item.recovery_offset
                                              : nullptr);
            }
        }
        jit_state.jit_state = backend::JitState::Cached;
        jit_state.cache_id = idx;
        jit_state.offset_in = buffer.offset;
        (void)module->PublishLinkTarget(
                ir::Location{block_start}, buffer.exec_data, buffer.exec_data);
        return buffer.exec_data;
    }
    return nullptr;
}

void* TranslateIR(const std::shared_ptr<backend::Module>& module,
                  const IntrusivePtr<ir::Block>& block) {
    auto& jit_state = block->GetJitCache();

    if (jit_state.jit_state == backend::JitState::Cached) {
        return module->GetJitCache(jit_state);
    }

    const auto& module_config = module->GetModuleConfig();
    const auto features = ResolveBackendFeatures(module);
    const auto& address_space = module->GetAddressSpace();

    // Optimize passes
    const ir::UniformInfo* uni_info = address_space.GetUniformInfo().uniform_size
                                      ? &address_space.GetUniformInfo() : nullptr;
    GetPassPipeline(uni_info).RunBlock(
            block.get(), module_config.optimizations, features);

    auto gprs{address_space.GetTrampolines().GetGPRRegs()};
    auto fprs{address_space.GetTrampolines().GetFPRRegs()};
    backend::RegAlloc reg_alloc{
            static_cast<u32>(block->MaxInstrId()), gprs, fprs, features,
            address_space.GetConfig().sse_afp_nan};

    PerfScope2 perf_ra_detail{GetPerfStats2().regalloc_total};
    ir::RegisterAllocPass::Run(
            block.get(), &reg_alloc,
            module->GetAddressSpace().GetConfig().sse_scalar_insert,
            features);
    perf_ra_detail.Stop();

    PerfScope2 perf_cg_detail{GetPerfStats2().codegen_total};
    backend::arm64::JitContext context{module, reg_alloc};
    backend::arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    perf_cg_detail.Stop();
    auto buffer_size = context.CurrentBufferSize();
    PerfScope2 perf_pub_total{GetPerfStats2().publish_total};
    PerfScope2 perf_pub_alloc{GetPerfStats2().publish_alloc};
    backend::arm64::JitContext* emitted_context = &context;
    backend::arm64::JitTranslator* emitted_translator = &translator;
    std::optional<backend::arm64::JitContext> fallback_context;
    std::optional<backend::arm64::JitTranslator> fallback_translator;
    auto allocation = module->AllocCodeCache(buffer_size, context.RequiresRegionTrampoline());
    if (allocation.first == backend::INVALID_CACHE_ID && context.RequiresRegionTrampoline()) {
        fallback_context.emplace(module, reg_alloc, false);
        fallback_translator.emplace(*fallback_context);
        fallback_translator->Translate(block.get());
        emitted_context = &*fallback_context;
        emitted_translator = &*fallback_translator;
        buffer_size = emitted_context->CurrentBufferSize();
        allocation = module->AllocCodeCache(buffer_size, false);
    }
    perf_pub_alloc.Stop();
    if (auto [idx, buffer] = allocation;
        idx != backend::INVALID_CACHE_ID) {
        PerfScope2 perf_pub_flush{GetPerfStats2().publish_flush};
        emitted_context->Flush(buffer);
        perf_pub_flush.Stop();
        {
            PerfScope2 perf_pub_fault{GetPerfStats2().publish_fault};
            module->AddFaultEntry(buffer.exec_data,
                                  buffer.exec_data + buffer.size,
                                  block->GetStartLocation().Value());
            if (module->GetAddressSpace().ExitLatchEnabled()) {
                for (const auto& item : emitted_translator->GetBackedgeBlockMetadata()) {
                    module->AddFaultEntry(buffer.exec_data + item.host_begin,
                                          buffer.exec_data + item.host_end,
                                          item.guest_start,
                                          buffer.exec_data,
                                          item.recovery_offset
                                                  ? buffer.exec_data + item.recovery_offset
                                                  : nullptr);
                }
            }
            PublishIndirectL1Faults(module, buffer, *emitted_translator);
        }
        jit_state.jit_state = backend::JitState::Cached;
        jit_state.cache_id = idx;
        jit_state.offset_in = buffer.offset;
        // The target generation is visible before SMC registration. Once a
        // node can be collected by TakeDirtyNodes, BeginTargetInvalidation is
        // therefore guaranteed to see and deactivate this publication.
        (void)module->PublishLinkTarget(
                block->GetStartLocation(), buffer.exec_data, buffer.exec_data);
        // SMC tracking (Phase 4): fix the block's guest end location (the
        // frontends never set node_size; AdvancePC immediates are per-
        // instruction sizes and survive the opt pipeline, so their sum is
        // the block's guest length) and write-protect the covered pages.
        // Read-only (static) modules skip protection: their guests cannot
        // legally self-modify.
        if (!module_config.read_only) {
            const VAddr block_start = block->GetStartLocation().Value();
            u64 block_size = 0;
            for (auto& inst : block->GetInstList()) {
                if (inst.GetOp() == ir::OpCode::AdvancePC) {
                    block_size += inst.GetArg<ir::Imm>(0).Get();
                }
            }
            if (block_size) {
                block->SetEndLocation(ir::Location(block_start + block_size));
            }
            PerfScope2 perf_pub_smc{GetPerfStats2().publish_smc};
            address_space.GetSmcTracker().RegisterNode(
                    module, block.get(), block_start, block->GetEndLocation().Value());
            for (const auto& dependency : block->GetGuestCodeDependencies()) {
                address_space.GetSmcTracker().RegisterNode(
                        module, block.get(), dependency.start.Value(),
                        dependency.end.Value());
            }
        }
        {
            PerfScope2 perf_pub_disk{GetPerfStats2().publish_disk};
            const VAddr start = block->GetStartLocation().Value();
            const std::vector<backend::SerialBlock> cache_blocks{
                    {.guest_start = start,
                     .guest_end = block->GetEndLocation().Value(),
                     .code_offset = 0,
                     .guest_bytes_hash = 0}};
            RecordJitCacheUnit(
                    module,
                    start,
                    false,
                    cache_blocks,
                    buffer,
                    *emitted_context,
                    *emitted_translator);
        }
        perf_pub_total.Stop();
        PerfScope2 perf_free_detail{GetPerfStats2().ir_free};
        block->DestroyInstrs();
        return buffer.exec_data;
    }
    return nullptr;
}

}  // namespace backend

}  // namespace swift::runtime
