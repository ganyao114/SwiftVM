#include "base/logging.h"
#include "base/scope_exit.h"
//
// Created by 甘尧 on 2023/9/7.
//
#pragma once

#include "runtime/compile/analysis/guest_state.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <span>
#include <stdexcept>
#include <utility>
#include <unistd.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/actions.h"
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/backend/arm64/compile/target.h"
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
#include "runtime/backend/riscv64/jit/compile.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/signal_handler.h"
#include "runtime/backend/signal_owner_slot.h"
#include "runtime/backend/translate_table.h"
#include "runtime/common/backedge_control.h"
#include "runtime/common/hot_coalesce_prof.h"
#include "runtime/common/perf_stats.h"
#include "runtime/common/signal_diagnostic.h"
#include "runtime/compile/unit.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/function.h"

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
// TLS owns the slot separately from Impl. A signal reads only the lock-free
// pointer; shared_ptr assignment/destruction is never part of that read path.
static thread_local std::atomic<backend::SignalOwnerSlot*> tls_owner_slot{};
struct ThreadOwnerStorage {
    std::shared_ptr<backend::SignalOwnerSlot> owner;
    ~ThreadOwnerStorage() { tls_owner_slot.store(nullptr, std::memory_order_seq_cst); }
};
static thread_local ThreadOwnerStorage tls_owner_storage{};

struct Runtime::Impl final {
    explicit Impl(backend::AddressSpace* address_space)
            : state_storage(sizeof(RuntimeNamedVectorConstants) +
                            sizeof(backend::State) +
                            address_space->GetConfig().uniform_buffer_size),
              address_space(address_space) {
        retirement_control = address_space->GetRetirementControl();
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
        retirement_exit.emplace(retirement_control, state->exit_request,
                address_space->GetConfig().backend_isa == kArm64 ? &state_storage : nullptr);
        state->interface = &profile_interface;
        profile_interface.instruction_capture = &instruction_capture;
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
        if (address_space->GetConfig().backend_isa == kRiscv64) {
            state->spill_area[backend::kRiscvAddressSpaceSlot] = reinterpret_cast<u64>(address_space);
            memory_participant.emplace();
            state->spill_area[backend::kRiscvMemoryParticipantSlot] = reinterpret_cast<u64>(&*memory_participant);
        }
        state->unaligned_atomic_lock_address =
                &backend::unaligned_atomic_lock;
        if ((address_space->GetConfig().backend_isa == kArm64 ||
             address_space->GetConfig().backend_isa == kRiscv64) &&
            True(address_space->GetConfig().global_opts & Optimizations::ReturnStackBuffer)) {
            return_stack.emplace();
            state->rsb_pointer = return_stack->Empty();
            state->rsb_empty = return_stack->Empty();
        }
        // Finish allocations before registering raw Runtime fields with SMC.
        auto& thread_storage = tls_owner_storage;
        owner_slot = std::make_shared<backend::SignalOwnerSlot>();
        jit_entry = address_space->GetTrampolines().GetRuntimeEntry();
        smc_epoch = address_space->GetSmcTracker().RegisterRuntime(
                l1_code_cache,
                &state->exit_request,
                address_space->GetConfig().backend_isa == kArm64 ? &state_storage : nullptr,
                &state->rsb_pointer,
                state->rsb_empty);
        // Claim this thread for host-side SMC fault recovery (see OwnerSlot).
        // A thread that constructs a second Runtime keeps the newest; either
        // resolves to the same AddressSpace and SmcTracker.
        if (!owner_slot->Publish(this)) std::terminate();
        // The new Impl pins its slot until TLS takes ownership. An interrupted
        // old-slot reader finishes on this same thread before old TLS storage
        // can be dropped by the following assignment.
        tls_owner_slot.store(owner_slot.get(), std::memory_order_seq_cst);
        thread_storage.owner = owner_slot;
    }

    ~Impl() {
        if (run_in_progress.test(std::memory_order_acquire)) {
            std::terminate();
        }
        if (owner_slot) owner_slot->DetachAndWait(this);
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
        // Terminal readiness must remain blocked through the final AS access.
        retirement_exit.reset();
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
        backend::SignalOwnerSlot::Reader owner_reader{
                self ? nullptr : tls_owner_slot.load(std::memory_order_seq_cst)};
        if (!self) self = static_cast<Impl*>(owner_reader.Get());
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
        if (handled && self == tls_active_runtime && self->return_stack &&
            self->address_space->GetConfig().backend_isa == kArm64) {
            (void)self->return_stack->Reset(uctx);
        }
        return handled;
    }

    // Decode the scalar RCpc forms the ARM64 emitter can produce without
    // alignment dispatch (Config::rcpc_fault_emulate). Encodings were
    // cross-checked against emitted instruction words: register forms keep
    // Rs=11111 in the compare mask so exclusive LDAXR/STLXR never match.
    struct RcpcAccess {
        bool load;
        u32 size;
        u32 rt;
        bool sign;
        bool sign_to_x;
    };

    static std::optional<RcpcAccess> DecodeRcpcAccess(u32 w) {
        const u32 size = u32{1} << (w >> 30);
        const u32 rt = w & 31;
        if ((w & 0x3FFFFC00u) == 0x38BFC000u) {
            return RcpcAccess{true, size, rt, false, false};  // LDAPR
        }
        if ((w & 0x3FFFFC00u) == 0x089FFC00u) {
            return RcpcAccess{false, size, rt, false, false};  // STLR
        }
        if ((w & 0x3F000000u) == 0x19000000u) {  // RCpc unscaled immediate
            switch ((w >> 22) & 3) {
                case 0: return RcpcAccess{false, size, rt, false, false};  // STLUR
                case 1: return RcpcAccess{true, size, rt, false, false};   // LDAPUR
                case 2: return RcpcAccess{true, size, rt, true, true};     // LDAPURSB/SH/SW
                default: return RcpcAccess{true, size, rt, true, false};   // LDAPURSB/SH (W)
            }
        }
        return std::nullopt;
    }

    // Emulate one faulting scalar RCpc access and resume after it. The
    // fault address must be proven guest-mapped for the whole access by
    // the embedder oracle first; anything else falls through to the
    // generic PageFatal path. Ordering mirrors the emitter's own slow
    // paths: load = copy + acquire fence, store = release fence + copy.
    // The copy itself can still fault if the oracle raced an unmap; the
    // nested fault then reaches the default handler like other host
    // dereferences in this chain.
    static bool TryEmulateRcpcFault(Impl* self, ucontext_t* uctx,
                                    const u8* host_pc, std::uintptr_t fault_addr) {
        if (self->address_space->GetConfig().backend_isa != kArm64 ||
            !self->address_space->GetConfig().rcpc_fault_emulate ||
            !backend::SignalHandler::HasGuestMapCheck() || !host_pc) {
            return false;
        }
        u32 insn = 0;
        std::memcpy(&insn, host_pc, sizeof(insn));
        const auto access = DecodeRcpcAccess(insn);
        if (!access) return false;
        if (backend::SignalHandler::GuestMappedBytes(fault_addr, access->size) !=
            access->size) {
            return false;
        }
        auto* ea = reinterpret_cast<u8*>(fault_addr);
        if (access->load) {
            u64 value = 0;
            std::memcpy(&value, ea, access->size);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (access->sign) {
                const u32 bits = access->size * 8;
                const auto extended = static_cast<u64>(
                        static_cast<std::int64_t>(value << (64 - bits)) >> (64 - bits));
                value = access->sign_to_x ? extended : (extended & 0xFFFFFFFFu);
            }
            if (access->rt != 31 &&
                !backend::SignalHandler::SetContextGPR(uctx, access->rt, value)) {
                return false;
            }
        } else {
            const auto value = backend::SignalHandler::GetContextGPR(uctx, access->rt);
            std::atomic_thread_fence(std::memory_order_release);
            std::memcpy(ea, &value, access->size);
        }
        backend::SignalHandler::SetContextPC(
                uctx, reinterpret_cast<std::uintptr_t>(host_pc + sizeof(u32)));
        return true;
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
        if (self->address_space->GetConfig().backend_isa == kRiscv64 &&
            self->state->spill_area[backend::kRiscvRecoveryPcSlot] &&
            self->state->spill_area[backend::kRiscvRecoveryFrameSlot]) {
            const auto recovery = self->state->spill_area[backend::kRiscvRecoveryPcSlot];
            if (!self->address_space->LookupFault(reinterpret_cast<u8*>(recovery), entry) ||
                backend::SignalHandler::IsGuestAddressMapped(fault_addr))
                return false;
            self->state->halt_reason = HaltReason::PageFatal;
            backend::ReleaseAbandonedUnalignedAtomicGuard();
            backend::riscv64::ReleaseAbandonedMemoryKernels();
            if (self->state->spill_area[backend::kRiscvMemoryOwnedSlot]) {
                auto* participant = reinterpret_cast<backend::riscv64::MemoryParticipant*>(
                        self->state->spill_area[backend::kRiscvMemoryParticipantSlot]);
                if (participant) participant->active.store(0, std::memory_order_release);
                else backend::riscv64::anonymous_memory_readers.fetch_sub(1, std::memory_order_seq_cst);
                self->state->spill_area[backend::kRiscvMemoryOwnedSlot] = 0;
            }
            // A semantic helper may have its own C++ stack/register frame.
            // Restore the generated block anchors before its epilogue runs.
            backend::SignalHandler::SetContextGPR(uctx, 8, self->state->spill_area[backend::kRiscvRecoveryFrameSlot]);
            backend::SignalHandler::SetContextGPR(uctx, 9, reinterpret_cast<uintptr_t>(self->state));
            backend::SignalHandler::SetContextGPR(uctx, 2, self->state->spill_area[backend::kRiscvRecoveryFrameSlot]);
            // Generated recovery reloads guest flags only when that block
            // reserves s11. Writing it here would corrupt the caller's s11
            // in a block with no flags cache and no s11 entry save.
            backend::SignalHandler::SetContextPC(uctx, recovery);
            return true;
        }
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
            if ((request & (kBackedgeSignalRequest | kRetirementExitRequest)) != 0) {
                if (request & kRetirementExitRequest)
                    self->state->halt_reason = HaltReason::Retirement;
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
        if (sig == SIGBUS && TryEmulateRcpcFault(self, uctx, host_pc, fault_addr)) {
            // Adaptive accounting: charge the fault to its host PC. At
            // kRcpcFaultThreshold the dispatcher marks the owning guest
            // block checks-forced and invalidates it for a checked
            // recompile (ServiceRcpcAdaptive).
            if (self->address_space->CountRcpcEmulatedFault(
                        reinterpret_cast<std::uintptr_t>(host_pc)) &&
                self->address_space->ExitLatchEnabled()) {
                // Steady-state loops may not reach a dispatcher boundary
                // for the whole workload, so request one: bump this
                // runtime's SMC generation (the PublishExitRequest CAS
                // shape, restricted to the executing runtime). The next
                // backedge poll exits through the SMC veneer; the Run
                // loop's post-JitRun service then adapts the block, and
                // AcknowledgeSmcRequest retires the generation as usual.
                // No page is dirty, so CloseWriteWindow fast-paths.
                auto request = std::atomic_ref<u64>(self->state->exit_request);
                auto observed = request.load(std::memory_order_relaxed);
                bool bumped = false;
                for (;;) {
                    const auto count = observed & kBackedgeSmcRequestMask;
                    if (count == kBackedgeSmcRequestMask) break;
                    const auto next =
                            (observed & ~kBackedgeSmcRequestMask) | (count + 1);
                    if (request.compare_exchange_weak(
                                observed, next, std::memory_order_release,
                                std::memory_order_relaxed)) {
                        bumped = true;
                        break;
                    }
                }
                // Arm the latch like every other exit_request
                // publisher: with the latch enabled, backedge polls
                // are fault-only (PollCycleExit), so this generation
                // bump must fault the dummy load to be observed.
                if (bumped &&
                    self->address_space->GetConfig().backend_isa == kArm64) {
                    self->state_storage.Arm();
                }
            }
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
        if (self->return_stack && self->address_space->GetConfig().backend_isa == kArm64 &&
            self->return_stack->Recover(uctx, fault_addr)) {
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
        const u64 desired = observed & ~kBackedgeSmcRequestMask;
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
        // A Function body that requires predecessor SSA/width state is also
        // intentionally absent from dispatch. Report CodeMiss so the frontend
        // decodes that PC from canonical guest state. SMC can likewise clear a
        // slot before detaching its still-live node at the quiescent boundary.
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

    [[nodiscard]] Runtime::InstructionKey InstructionKey() const {
        if (!instruction_pin || (instruction_capture.status != backend::InstructionCapture::Suspended &&
                                 instruction_capture.status != backend::InstructionCapture::Abandoned)) return {};
        const auto& pin = *instruction_pin;
        return {this, address_space, retirement_control.get(), pin.descriptor.get(), pin.descriptor->emitted,
                pin.allocation, instruction_generation, pin.allocation_id, pin.descriptor->ir_identity,
                instruction_capture.head_id, instruction_capture.guest_pc};
    }

    [[nodiscard]] HaltReason Run(const Runtime::InstructionKey* resume_key = nullptr) const {
        if (run_in_progress.test_and_set(std::memory_order_acquire)) {
            return HaltReason::RuntimeBusy;
        }
        struct RunGuard {
            std::atomic_flag& flag;
            ~RunGuard() { flag.clear(std::memory_order_release); }
        } run_guard{run_in_progress};
        backend::RetirementControl::Lease admission{
                retirement_control, backend::RetirementControl::Kind::Execution};
        if (!admission) return HaltReason::Retirement;
        if (!resume_key && instruction_capture.status != backend::InstructionCapture::Empty)
            return HaltReason::InstructionPending;
        if (resume_key) {
            if (instruction_capture.status != backend::InstructionCapture::Suspended || !instruction_pin ||
                *resume_key != InstructionKey() ||
                &instruction_pin->module->GetAddressSpace() != address_space ||
                instruction_pin->module->GetRetirementControl() != retirement_control ||
                !retirement_control->AuthorizeInstructionResume(admission, instruction_generation) ||
                !instruction_pin->module->ValidateInstructionPin(*instruction_pin) ||
                instruction_capture.head_id != instruction_pin->descriptor->head_id ||
                instruction_capture.guest_pc != instruction_pin->descriptor->guest_pc ||
                !backend::ValidInstructionStickyFlags(instruction_capture.sticky_fflags) ||
                GetLocation() != instruction_capture.guest_pc || state->host_cpu_flags != instruction_capture.guest_flags)
                return HaltReason::InstructionRejected;
        }
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
            if (retirement_control->IsClosed()) return HaltReason::Retirement;
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
            backend::SmcTracker::ExecutionScope execution{smc, smc_epoch};
            // Closing a write window can detach an allocation after the
            // initial owner/key check. Revalidate under actual execution
            // protection before selecting any process-local resume entry.
            if (resume_key &&
                (!retirement_control->AuthorizeInstructionResume(admission, instruction_generation) ||
                 !instruction_pin->module->ValidateInstructionPin(*instruction_pin)))
                return HaltReason::InstructionRejected;
            if (auto cache = resume_key ? instruction_pin->allocation + instruction_pin->descriptor->resume_offset :
                            hr != HaltReason::CacheMiss ? address_space->GetCodeCache(current_loc)
                                                         : nullptr;
                cache) {
                if (resume_key) {
                    instruction_capture.status = backend::InstructionCapture::Resuming;
                } else if (address_space->GetConfig().backend_isa == kRiscv64) {
                    auto module = address_space->GetModule(current_loc);
                    instruction_pin = module->PinInstructionCode(module, static_cast<u8*>(cache));
                    if (instruction_pin) {
                        if (!module->ValidateInstructionPin(*instruction_pin)) {
                            instruction_pin.reset(); return HaltReason::InstructionRejected;
                        }
                        instruction_capture = {};
                        instruction_capture.status = backend::InstructionCapture::Armed;
                        instruction_generation = admission.Generation();
                    }
                }
                // JIT Run!
                const bool manage_guest_fpcr =
                        address_space->GetConfig().sse_afp_nan;
                if (manage_guest_fpcr) {
                    jit_host_fpcr.store(backend::arm64::ReadNativeFPCR(),
                                        std::memory_order_relaxed);
                    jit_guest_fpcr_active.store(true, std::memory_order_release);
                }
                hr = JitRun(cache);
                if (instruction_pin) {
                    if (instruction_capture.status == backend::InstructionCapture::Suspended) {
                        instruction_generation = admission.Generation();
                    } else if (instruction_capture.status == backend::InstructionCapture::Resuming) {
                        // The trampoline refused entry before the resume veneer.
                        instruction_capture.status = backend::InstructionCapture::Suspended;
                    } else if (hr == HaltReason::CallHost && GetLocation() == instruction_pin->descriptor->completion_pc) {
                        instruction_pin.reset(); instruction_capture = {};
                    } else {
                        instruction_capture.status = backend::InstructionCapture::Abandoned;
                    }
                }
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
                execution.End();
                // SMC write-window close (Phase 4): if a guest store hit a
                // write-protected code page during this JitRun, the stale
                // translations are invalidated now — the guest is back on
                // the host side, so freeing JIT code and editing module
                // maps is safe. Runs before the hr checks so it also covers
                // the CodeMiss/CacheMiss exits. Direct links have already
                // been restored by the signal-window transaction before any
                // retired allocation can become reclaimable.
                smc.CloseWriteWindow(*address_space, l1_code_cache);
                // Adaptive RCpc: blocks that exhausted their emulated-
                // fault budget during this JitRun are marked checks-
                // forced and invalidated here, at the same quiescent
                // boundary as the SMC window close. Atomic-only unless
                // a threshold crossing is actually pending.
                address_space->ServiceRcpcAdaptive(&l1_code_cache);
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
                execution.End();
                // IR Interpreter
                hr = Interpreter();
            }
            if (retirement_control->IsClosed()) return HaltReason::Retirement;
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
    std::optional<backend::riscv64::MemoryParticipant> memory_participant{};
    std::optional<backend::GuardedReturnStack> return_stack{};
    backend::AddressSpace* address_space{};
    std::shared_ptr<backend::RetirementControl> retirement_control;
    std::optional<backend::RetirementControl::ExitRegistration> retirement_exit;
    // mutable: JIT dispatch fills this per-Runtime table even from const Run
    // paths; State publishes its stable Data() base to generated code.
    mutable TranslateTable l1_code_cache{
            l1_cache_bits, TranslateTableHash::Direct};
    backend::SmcTracker::RuntimeToken smc_epoch{};
    // Kept alive alongside the creating thread's tls_owner_slot; see OwnerSlot.
    std::shared_ptr<backend::SignalOwnerSlot> owner_slot{};
    backend::interp::InterpStack interp_stack;
    std::atomic_bool running{true};
    mutable std::atomic_flag run_in_progress = ATOMIC_FLAG_INIT;
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
    mutable backend::InstructionCapture instruction_capture{};
    mutable std::unique_ptr<backend::InstructionCodePin> instruction_pin{};
    mutable u64 instruction_generation{};

    void PublishIndirectL1Base(void* base) {
        std::atomic_ref<void*>(state->indirect_l1_code_cache)
                .store(base, std::memory_order_release);
    }
};

Runtime::Runtime(Instance* instance) {
    auto* space = reinterpret_cast<backend::AddressSpace*>(instance);
    backend::RetirementControl::Lease creation{
            space->GetRetirementControl(), backend::RetirementControl::Kind::Producer};
    if (!creation) throw std::runtime_error("runtime admission is closed");
    impl = std::make_unique<Impl>(space);
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

Runtime::InstructionKey Runtime::SuspendedInstruction() const { return impl->InstructionKey(); }

HaltReason Runtime::ResumeInstruction(const InstructionKey& key) { return impl->Run(&key); }

bool Runtime::AbandonInstruction(const InstructionKey& key) {
    if (impl->run_in_progress.test_and_set(std::memory_order_acquire)) return false;
    struct Guard { std::atomic_flag& flag; ~Guard() { flag.clear(std::memory_order_release); } } guard{impl->run_in_progress};
    if (!key.runtime || key != impl->InstructionKey()) return false;
    impl->instruction_pin.reset();
    impl->instruction_capture.status = backend::InstructionCapture::Abandoned;
    return true;
}

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
    if (impl->address_space->GetConfig().backend_isa == kArm64)
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
            .fetch_and(~kBackedgeSignalRequest, std::memory_order_acq_rel);
    impl->state_storage.DisarmIfIdle(&impl->state->exit_request);
    impl->running.store(true, std::memory_order_release);
}

void Runtime::SetLocation(LocationDescriptor location) {
    if (impl->instruction_pin) throw std::runtime_error("instruction must complete or be abandoned before location reset");
    impl->instruction_capture = {};
    impl->SetLocation(location);
}

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
                        std::span<const backend::arm64::JitTranslator* const> translators,
                        std::span<const SerialFaultSite> block_faults = {}) {
    if (module->GetAddressSpace().SerializableCodeEmission()) return;
    // A checks-forced (adaptive RCpc) unit must not enter the disk
    // cache: its key carries no per-unit mark, so recording it would
    // serve checked bytes as the canonical unchecked form (and vice
    // versa the revived unchecked unit simply adapts again at runtime).
    if (context.RcpcForceChecks()) return;
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
    std::vector<SerialFaultSite> fault_sites{block_faults.begin(), block_faults.end()};
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

// A failed region allocation has not published this buffer. Rebuild the
// encoder in the same storage, releasing the translator before its context.
struct BlockEmission {
    BlockEmission(const std::shared_ptr<Module>& module,
                  compile::AllocationView allocation,
                  const arm64::UnitActions& actions,
                  bool enable_direct_link)
            : context(module, allocation, actions, enable_direct_link), translator(context) {}

    arm64::JitContext context;
    arm64::JitTranslator translator;
};

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
    RetirementControl::Lease producer{module->GetRetirementControl(), RetirementControl::Kind::Producer};
    if (!producer) return false;
    if (!function || function->HasStandaloneBlocks()) return false;
    (void)compile::PrepareFunctionGuestRanges(function);
    if (!module->Push(function->GetFunction())) {
        return false;
    }
    function->MarkFunctionPublished();
    return true;
}

static void* TranslatePreparedFunctionRegions(
        const std::shared_ptr<backend::Module>& module,
        std::span<const arm64::Arm64PreparedUnit> prepared_regions,
        ir::HIRFunction* function) {
    ASSERT(!prepared_regions.empty() && prepared_regions.front().Function() == function);
    auto* ir_function = function->GetFunction();
    const auto func_start = ir_function->GetStartLocation().Value();
    auto& jit_state = ir_function->GetJitCache();
    const bool dump_ir = GetSvmConfig().dump_ir;
    PerfScope perf_cg{GetPerfStats().codegen_ns};
    PerfScope2 perf_cg_detail{GetPerfStats2().codegen_total};
    backend::arm64::FunctionCodeObjectEmitter emitter{module, prepared_regions};
    if (!emitter.Emit()) return nullptr;
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
        if (!emitter.Emit(false)) return nullptr;
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
            ir_insts += item.Function()->MaxInstrCount();
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
        function->MarkFunctionPublished();
        if (dump_ir) SVM_DIAG_FORMAT(Runtime, "[func-compile] {:#x} publish-ready\n", func_start);
        // Publish every decoded block label, not only the function entry.
        // External links, RSB return targets, and code misses are allowed to
        // land at a basic-block boundary inside this compiled unit.
        auto& mutable_address_space = module->GetAddressSpace();
        const auto allocation_size = static_cast<u32>(buffer.size);
        backend::FunctionEntryPublisher entry_publisher{*module, buffer.exec_data, allocation_size};
        std::vector<backend::SerialBlock> cache_blocks;
        std::vector<std::pair<u64, u32>> code_entries;
        for (auto& item : prepared_regions) {
            const auto* region_function = item.Function();
            const auto canonical_terminal_entries =
                    backend::FunctionEntryContract::AnalyzeCanonicalTerminalEntries(
                            *region_function);
            for (auto& hir_block : region_function->GetHIRBlocksRPO()) {
                auto* block = hir_block.GetBlock();
                const auto guest = block->GetStartLocation().Value();
                const bool canonical_terminal = canonical_terminal_entries.contains(guest);
                if (block->GetInstList().empty() && !block->HasTerminal()) continue;
                const auto* entry_inputs = item.Actions().Edges().ForBlock(block);
                ASSERT(entry_inputs);
                const auto offset = emitted_context->GetCodeOffset(guest);
                ASSERT(offset >= 0);
                code_entries.emplace_back(guest, static_cast<u32>(offset));
                const auto guest_end =
                        canonical_terminal ? std::max(guest + 1, block->GetEndLocation().Value())
                                           : block->GetEndLocation().Value();
                if (entry_inputs->requires_unit_state ||
                    (block->GetInstList().empty() && !canonical_terminal)) {
                    // This body is reachable through certified local edges.
                    // An external dispatch must decode from canonical guest
                    // state instead of assuming its predecessor's SSA values.
                    if (guest_end > guest)
                        cache_blocks.push_back({
                                .guest_start = guest,
                                .guest_end = guest_end,
                                .code_offset = UINT32_MAX,
                                .entry_flags = backend::SerialBlock::MetadataOnly,
                        });
                } else {
                    const auto to_offset = [](ptrdiff_t value) {
                        return value < 0 ? backend::kInvalidFunctionEntryOffset
                                         : static_cast<u32>(value);
                    };
                    const auto pending_contract =
                            emitted_context->GetPendingFlagsTargetContract(guest);
                    const auto contract = backend::FunctionEntryContract::Build(
                            *region_function,
                            *block,
                            {.canonical = to_offset(offset),
                             .direct_link =
                                     to_offset(emitted_context->GetDirectLinkCodeOffset(guest)),
                             .pending_flags =
                                     to_offset(emitted_context->GetPendingFlagsCodeOffset(guest)),
                             .continuation = to_offset(emitted_context->GetCallCodeOffset(guest)),
                             .pending_flags_continuation = to_offset(
                                     emitted_context->GetCallPendingFlagsCodeOffset(guest))},
                            pending_contract,
                            canonical_terminal);
                    ASSERT(contract.IsWellFormed(allocation_size));
                    {
                        PerfScope2 perf_pub_l2{GetPerfStats2().publish_l2};
                        (void)entry_publisher.Publish(contract);
                    }
                    cache_blocks.push_back({
                            .guest_start = contract.Guest().Value(),
                            .guest_end = contract.GuestEnd().Value(),
                            .code_offset = contract.Canonical().code_offset,
                            .guest_bytes_hash = 0,
                            .direct_code_offset = contract.DirectLink().code_offset,
                            .pending_flags_code_offset = contract.PendingFlags().code_offset,
                            .pending_flags_contract = pending_contract,
                            .entry_flags = static_cast<u8>(
                                    contract.Linkable() ? backend::SerialBlock::Linkable : 0),
                            .continuation_code_offset = contract.Continuation().code_offset,
                            .pending_flags_continuation_code_offset =
                                    contract.PendingFlagsContinuation().code_offset,
                    });
                }
                for (const auto& dependency : block->GetGuestCodeDependencies()) {
                    cache_blocks.push_back({
                            .guest_start = dependency.start.Value(),
                            .guest_end = dependency.end.Value(),
                            .code_offset = UINT32_MAX,
                            .entry_flags = backend::SerialBlock::DependencyOnly,
                    });
                }
                if (!module->GetModuleConfig().read_only) {
                    PerfScope2 perf_pub_smc{GetPerfStats2().publish_smc};
                    mutable_address_space.GetSmcTracker().RegisterNode(
                            module, ir_function, guest, guest_end);
                    for (const auto& dependency : block->GetGuestCodeDependencies()) {
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
        std::vector<backend::SerialFaultSite> block_faults;
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
                for (size_t i = 0; i < code_entries.size(); ++i) {
                    const auto recovery =
                            std::find_if(backedges.begin(), backedges.end(), [&](const auto& item) {
                                return item.guest_start == code_entries[i].first;
                            });
                    const u32 begin = recovery != backedges.end() ? recovery->host_begin
                                                                  : code_entries[i].second;
                    const u32 end =
                            recovery != backedges.end()
                                    ? recovery->host_end
                                    : (i + 1 < code_entries.size() ? code_entries[i + 1].second
                                                                   : static_cast<u32>(buffer.size));
                    ASSERT(begin < end && end <= buffer.size);
                    module->AddFaultEntry(buffer.exec_data + begin,
                                          buffer.exec_data + end,
                                          code_entries[i].first,
                                          buffer.exec_data,
                                          recovery != backedges.end() && recovery->recovery_offset
                                                  ? buffer.exec_data + recovery->recovery_offset
                                                  : nullptr);
                    block_faults.push_back(
                            {code_entries[i].first,
                             begin,
                             end,
                             recovery != backedges.end() && recovery->recovery_offset
                                     ? recovery->recovery_offset
                                     : UINT32_MAX,
                             static_cast<u8>(backend::FaultRecoveryKind::GuestRange)});
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
                               emitted_translators,
                               block_faults);
        }

        return buffer.exec_data;
    }
    return nullptr;
}

static void ReleaseFunctionIR(ir::Function* function) {
    // Call only after prepared results and encoders have released their IR borrows.
    PerfScope perf_free{GetPerfStats().ir_free_ns};
    PerfScope2 perf_free_detail{GetPerfStats2().ir_free};
    auto guard = function->LockWrite();
    function->DestroyInstrs();
}

void* TranslateIR(const std::shared_ptr<backend::Module>& module,
                  std::span<ir::HIRFunction* const> functions) {
    RetirementControl::Lease producer{module->GetRetirementControl(), RetirementControl::Kind::Producer};
    if (!producer) return nullptr;
    ASSERT(!functions.empty());
    for (auto* function : functions)
        if (!function || function->HasStandaloneBlocks()) return nullptr;
    if (module->GetAddressSpace().GetConfig().backend_isa == kRiscv64)
        return riscv64::CompileFunctions(module, functions);
    void* code{};
    {
        auto prepared = arm64::PrepareFunctions(module, functions);
        if (!prepared) return nullptr;
        code = TranslatePreparedFunctionRegions(module, *prepared, functions.front());
    }
    if (code && FuncIRFreeEnabled()) ReleaseFunctionIR(functions.front()->GetFunction());
    return code;
}

void* TranslateIR(const std::shared_ptr<backend::Module>& module, ir::HIRFunction* function) {
    RetirementControl::Lease producer{module->GetRetirementControl(), RetirementControl::Kind::Producer};
    if (!producer) return nullptr;
    if (!function || function->HasStandaloneBlocks()) return nullptr;
    if (module->GetAddressSpace().GetConfig().backend_isa == kRiscv64)
        return riscv64::CompileFunctions(module, std::span{&function, 1});
    void* code{};
    {
        auto prepared = arm64::PrepareFunction(module, function);
        if (!prepared) return nullptr;
        code = TranslatePreparedFunctionRegions(module, std::span{&*prepared, 1}, function);
    }
    if (code && FuncIRFreeEnabled()) ReleaseFunctionIR(function->GetFunction());
    return code;
}

void* TranslateIR(const std::shared_ptr<backend::Module>& module, ir::HIRBlock* block) {
    RetirementControl::Lease producer{module->GetRetirementControl(), RetirementControl::Kind::Producer};
    if (!producer) return nullptr;
    auto ir_block = block->GetBlock();
    auto block_start = ir_block->GetStartLocation().Value();
    if (module->GetAddressSpace().GetConfig().backend_isa == kArm64 && !arm64::SupportsIR(ir_block))
        return nullptr;
    // Function and Module use the same intrusive map hook. Detach before Push,
    // and retain the Function owner until the module has taken its reference.
    if (!block->BeginStandalonePublication()) return nullptr;
    auto publication = ScopeExit([&] { block->AbortStandalonePublication(); });
    if (!module->Push(ir_block)) {
        return nullptr;
    }
    block->CommitStandalonePublication();
    publication.Cancel();

    auto guard = ir_block->LockWrite();
    if (module->GetAddressSpace().GetConfig().backend_isa == kRiscv64)
        return riscv64::CompileBlock(module, ir_block);
    auto& jit_state = ir_block->GetJitCache();
    if (jit_state.jit_state == backend::JitState::Cached) {
        return module->GetJitCache(jit_state);
    }
    auto prepared = arm64::PrepareFinalBlock(module, block);
    if (!prepared) return nullptr;
    const auto& reg_alloc = prepared->Allocation();
    PerfScope perf_cg{GetPerfStats().codegen_ns};
    PerfScope2 perf_cg_detail{GetPerfStats2().codegen_total};
    std::optional<BlockEmission> emission{
            std::in_place, module, reg_alloc, prepared->Actions(), true};
    if (!emission->translator.Translate(block->GetBlock())) return nullptr;
    perf_cg_detail.Stop();
    perf_cg.Stop();
    auto buffer_size = emission->context.CurrentBufferSize();
    PerfScope2 perf_pub_total{GetPerfStats2().publish_total};
    PerfScope2 perf_pub_alloc{GetPerfStats2().publish_alloc};
    auto allocation =
            module->AllocCodeCache(buffer_size, emission->context.RequiresRegionTrampoline());
    if (allocation.first == backend::INVALID_CACHE_ID &&
        emission->context.RequiresRegionTrampoline()) {
        emission.reset();
        emission.emplace(module, reg_alloc, prepared->Actions(), false);
        if (!emission->translator.Translate(block->GetBlock())) return nullptr;
        buffer_size = emission->context.CurrentBufferSize();
        allocation = module->AllocCodeCache(buffer_size, false);
    }
    perf_pub_alloc.Stop();
    auto* emitted_context = &emission->context;
    auto* emitted_translator = &emission->translator;
    if (auto [idx, buffer] = allocation;
        idx != backend::INVALID_CACHE_ID) {
        PerfScope perf_pub{GetPerfStats().publish_ns};
        if (PerfEnabled()) {
            PerfAdd(GetPerfStats().host_bytes, buffer_size);
            PerfAdd(GetPerfStats().ir_insts, ir_block->MaxInstrId());
        }
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
    RetirementControl::Lease producer{module->GetRetirementControl(), RetirementControl::Kind::Producer};
    if (!producer) return nullptr;
    if (module->GetAddressSpace().GetConfig().backend_isa == kRiscv64)
        return riscv64::CompileBlock(module, block.get());
    if (!arm64::SupportsIR(block.get())) return nullptr;
    auto& jit_state = block->GetJitCache();

    if (jit_state.jit_state == backend::JitState::Cached) {
        return module->GetJitCache(jit_state);
    }

    const auto& module_config = module->GetModuleConfig();
    const auto& address_space = module->GetAddressSpace();
    bool release_ir{};
    auto destroy_ir = ScopeExit([&] {
        if (!release_ir) return;
        PerfScope perf_free{GetPerfStats().ir_free_ns};
        PerfScope2 perf_free_detail{GetPerfStats2().ir_free};
        block->DestroyInstrs();
    });
    auto prepared = arm64::PrepareBlock(module, block);
    if (!prepared) return nullptr;
    const auto& reg_alloc = prepared->Allocation();
    PerfScope perf_cg{GetPerfStats().codegen_ns};
    PerfScope2 perf_cg_detail{GetPerfStats2().codegen_total};
    std::optional<BlockEmission> emission{
            std::in_place, module, reg_alloc, prepared->Actions(), true};
    if (!emission->translator.Translate(block.get())) return nullptr;
    perf_cg_detail.Stop();
    perf_cg.Stop();
    auto buffer_size = emission->context.CurrentBufferSize();
    PerfScope2 perf_pub_total{GetPerfStats2().publish_total};
    PerfScope2 perf_pub_alloc{GetPerfStats2().publish_alloc};
    auto allocation =
            module->AllocCodeCache(buffer_size, emission->context.RequiresRegionTrampoline());
    if (allocation.first == backend::INVALID_CACHE_ID &&
        emission->context.RequiresRegionTrampoline()) {
        emission.reset();
        emission.emplace(module, reg_alloc, prepared->Actions(), false);
        if (!emission->translator.Translate(block.get())) return nullptr;
        buffer_size = emission->context.CurrentBufferSize();
        allocation = module->AllocCodeCache(buffer_size, false);
    }
    perf_pub_alloc.Stop();
    auto* emitted_context = &emission->context;
    auto* emitted_translator = &emission->translator;
    if (auto [idx, buffer] = allocation;
        idx != backend::INVALID_CACHE_ID) {
        PerfScope perf_pub{GetPerfStats().publish_ns};
        if (PerfEnabled()) {
            PerfAdd(GetPerfStats().host_bytes, buffer_size);
            PerfAdd(GetPerfStats().ir_insts, block->MaxInstrId());
        }
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
        // Protect the prepared guest range after publishing the target generation.
        // Read-only modules skip protection: their guests cannot self-modify.
        if (!module_config.read_only) {
            PerfScope2 perf_pub_smc{GetPerfStats2().publish_smc};
            address_space.GetSmcTracker().RegisterNode(
                    module, block.get(), block->GetStartLocation().Value(),
                    block->GetEndLocation().Value());
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
        release_ir = true;
        return buffer.exec_data;
    }
    return nullptr;
}

}  // namespace backend

}  // namespace swift::runtime
