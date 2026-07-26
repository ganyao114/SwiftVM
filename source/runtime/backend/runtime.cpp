//
// Created by 甘尧 on 2023/9/7.
//
#pragma once

#include <atomic>
#include <mutex>
#include <utility>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/constant.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/context.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/signal_handler.h"
#include "runtime/backend/translate_table.h"
#include "runtime/common/perf_stats.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/function.h"
#include "runtime/ir/opts/pass_pipeline.h"
#include "runtime/ir/opts/register_alloc_pass.h"

namespace swift::runtime {

constexpr static auto l1_cache_bits = 18;

std::unique_ptr<Instance> Instance::Make(const Config& config) {
    return std::make_unique<backend::AddressSpace>(config);
}

// Thread-local pointer to the Runtime::Impl currently executing guest code
// on this thread; the host signal handler uses it to route faults to the
// right State. void* because Runtime::Impl is a private nested type.
static thread_local void* tls_active_runtime{};

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
    explicit Impl(backend::AddressSpace* address_space) : address_space(address_space) {
        state_buffer.resize(sizeof(backend::State) +
                            address_space->GetConfig().uniform_buffer_size);
        state = reinterpret_cast<backend::State*>(state_buffer.data());
        // Wire the dispatcher's code-cache tables: L1 is per-runtime, L2 is the
        // address-space wide translate table that PushCodeCache writes to.
        state->l1_code_cache = l1_code_cache.Data();
        state->l2_code_cache = address_space->GetCodeCacheTable().Data();
        smc_epoch = address_space->GetSmcTracker().RegisterRuntime(l1_code_cache);
        // Guest address virtualization: Config::memory_base carries the
        // guest->host bias (host = guest + bias); the JIT keeps it in the
        // reserved pt register and the interpreter reads it from here.
        // nullptr (identity) keeps the zero-overhead fast path.
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
        // Return Stack Buffer: allocate the RSB backing store and point
        // state->rsb_pointer at the top of the stack (the stack grows
        // downward: push pre-decrements, pop post-increments).  The buffer
        // has rsb_stack_size + 2 entries; the initial pointer sits at entry
        // [rsb_stack_size] so 64 pushes reach entry [0] before the two
        // guard slots absorb a modest overflow.
        if (True(address_space->GetConfig().global_opts & Optimizations::ReturnStackBuffer)) {
            state->rsb_pointer = &rsb_buffer.rsb_frames[backend::rsb_stack_size];
            // JIT overflow/underflow guards: push skips at the bottom (full),
            // pop falls back to the dispatcher at the top (empty). See
            // JitContext::EmitRSBPush / EmitRSBPop.
            state->rsb_bottom = &rsb_buffer.rsb_frames[0];
            state->rsb_top = &rsb_buffer.rsb_frames[backend::rsb_stack_size];
        }
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
    // SmcTracker::RegisterNode; the tracker opens a write window (page back
    // to RW, stale blocks' dispatch slots zeroed) and the faulting store is
    // re-executed on sigreturn. Actual invalidation is deferred to
    // CloseWriteWindow after the current JitRun returns.
    static constexpr int kSmcPriority = 0;

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
        if (sig != SIGSEGV && sig != SIGBUS) {
            return false;
        }
        const auto fault_addr = reinterpret_cast<std::uintptr_t>(info->si_addr);
        return self->address_space->GetSmcTracker().HandleWriteFault(
                *self->address_space, self->l1_code_cache, fault_addr);
    }

    static bool HandleFault(void* ctx, ucontext_t* uctx, int sig, siginfo_t* info) {
        (void) ctx;
        auto* self = static_cast<Impl*>(tls_active_runtime);
        if (!self) return false;
        if (sig != SIGSEGV && sig != SIGBUS) {
            return false;  // SIGILL in JIT code is a host codegen bug: crash
        }
        const auto host_pc = reinterpret_cast<u8*>(backend::SignalHandler::GetContextPC(uctx));
        backend::FaultEntry entry{};
        if (!self->address_space->LookupFault(host_pc, entry)) {
            return false;  // fault PC not in any JIT code buffer
        }
        const auto fault_addr = reinterpret_cast<std::uintptr_t>(info->si_addr);
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
        // label_return_host clears state->halt_reason and returns w0 to the
        // host caller, so the halt reason travels in x0. It also saves the
        // statically allocated guest registers and restores the host callee
        // saves off the stack — both valid here because JIT blocks never
        // unbalance sp and the reserved state/flags registers are intact at
        // any guest memory access.
        backend::SignalHandler::SetContextReturnValue(uctx,
                                                      static_cast<u64>(HaltReason::PageFatal));
        backend::SignalHandler::SetContextPC(
                uctx,
                reinterpret_cast<std::uintptr_t>(
                        self->address_space->GetTrampolines().GetReturnHost()));
        return true;
    }

    void SetLocation(LocationDescriptor location) const {
        state->current_loc = ir::Location(location);
    }

    [[nodiscard]] LocationDescriptor GetLocation() const { return state->current_loc.Value(); }

    [[nodiscard]] HaltReason JitRun(void* cache) const { return jit_entry(state, cache); }

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

        HaltReason hr{HaltReason::None};
        while (running.load(std::memory_order_acquire)) {
            auto current_loc = GetLocation();
            auto& smc = address_space->GetSmcTracker();
            // Publish before cache lookup, not merely before JitRun: a pointer
            // fetched while an invalidation races must remain epoch-protected
            // until the trampoline returns.
            smc.BeginJit(smc_epoch);
            if (auto cache = hr != HaltReason::CacheMiss ? address_space->GetCodeCache(current_loc)
                                                         : nullptr;
                cache) {
                if (hr == HaltReason::BlockLinkage) {
                    // Do linkage
                    auto linkage_cache_place = state->blocking_linkage_address;
                    auto pre_block_vaddr = state->prev_loc.Value();
                    LinkBlock(pre_block_vaddr, linkage_cache_place, current_loc, cache);
                }
                // JIT Run!
                hr = JitRun(cache);
                // The runtime is now quiescent with respect to retired JIT
                // code. This is an atomic-only fast path unless reclamation
                // work is actually pending.
                smc.EndJit(smc_epoch);
                // SMC write-window close (Phase 4): if a guest store hit a
                // write-protected code page during this JitRun, the stale
                // translations are invalidated now — the guest is back on
                // the host side, so freeing JIT code and editing module
                // maps is safe. Runs before the hr checks so it also covers
                // the CodeMiss/CacheMiss exits. NOTE: with DirectBlockLink
                // disabled HaltReason::BlockLinkage is never produced; if it
                // is ever enabled, the linkage patch below must be ordered
                // against invalidation of the *previous* block.
                smc.CloseWriteWindow(*address_space, l1_code_cache);
            } else {
                smc.EndJit(smc_epoch);
                // IR Interpreter
                hr = Interpreter();
            }
            if (hr == HaltReason::CacheMiss || hr == HaltReason::BlockLinkage) {
                continue;
            } else {
                break;
            }
        }
        return hr;
    }

    bool LinkBlock(LocationDescriptor stub_vaddr,
                   void* link_stub,
                   LocationDescriptor target_vaddr,
                   void* target_cache) const {
        auto src_module = address_space->GetModule(stub_vaddr);
        auto dest_module = address_space->GetModule(target_vaddr);
        if (!src_module && src_module != dest_module) {
            return false;
        }
        auto code_cache = dest_module->GetCodeCache(static_cast<u8*>(link_stub));
        if (auto rw_ptr = code_cache->GetRWPtr(stub_vaddr); rw_ptr) {
            return address_space->GetTrampolines().LinkBlock(
                    static_cast<u8*>(link_stub), static_cast<u8*>(target_cache), rw_ptr, true);
        }
        return false;
    }

    Instance* instance{};
    std::vector<u8> state_buffer{};
    backend::State* state{};
    // RSB backing store: rsb_stack_size + 2 entries of 16 bytes each.
    // state->rsb_pointer points into this buffer; the trampoline saves/
    // restores it across host exits.
    backend::RSBBuffer rsb_buffer{};
    backend::AddressSpace* address_space{};
    // mutable: the JIT dispatcher writes L1 entries through the raw
    // state->l1_code_cache pointer even from const Run paths.
    mutable TranslateTable l1_code_cache{l1_cache_bits};
    backend::SmcTracker::RuntimeToken smc_epoch{};
    // Kept alive alongside the creating thread's tls_owner_slot; see OwnerSlot.
    std::shared_ptr<OwnerSlot> owner_slot{};
    backend::interp::InterpStack interp_stack;
    std::atomic_bool running{true};
    backend::Trampolines::RuntimeEntry jit_entry{};
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
    // The JIT trampoline samples halt_reason after every returned block. This
    // makes an interrupt observable while Run() is inside generated code
    // instead of waiting for an unrelated guest syscall.
    impl->state->halt_reason = HaltReason::Signal;
}

void Runtime::ClearInterrupt() {
    impl->state->halt_reason = HaltReason::None;
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

namespace backend {

namespace {

bool X87TopVirtRequested() {
    // Cached: runs once per compiled unit, and Darwin's getenv walks `environ`.
    static const bool requested = [] {
        const char* topvirt = std::getenv("SVM_X87_TOPVIRT");
        const char* x87_jit = std::getenv("SVM_X87_JIT");
        return topvirt && std::strcmp(topvirt, "0") != 0 &&
               x87_jit && std::strcmp(x87_jit, "0") != 0;
    }();
    return requested;
}

bool HasX87Op(ir::Block* block) {
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() == ir::OpCode::X87Op) {
            return true;
        }
    }
    return false;
}

bool HasX87Op(ir::HIRFunction* function) {
    for (auto& block : function->GetHIRBlocksRPO()) {
        if (HasX87Op(block.GetBlock())) {
            return true;
        }
    }
    return false;
}

// JIT disk cache hook (backend/jit_cache.h). Off unless SVM_JIT_CACHE is set;
// the unit is described by its guest block ranges plus the offset of each
// block's entry inside the emitted buffer.
void RecordJitCacheUnit(const std::shared_ptr<backend::Module>& module,
                        VAddr guest_start,
                        bool is_function,
                        const std::vector<SerialBlock>& blocks,
                        const CodeBuffer& buffer) {
    auto* cache = module->GetAddressSpace().GetJitDiskCache();
    if (!cache) {
        return;
    }
    cache->RecordUnit(module,
                      guest_start,
                      is_function,
                      buffer.rw_data,
                      static_cast<u32>(buffer.size),
                      blocks);
}

// PassPipeline::BuildDefault builds a nine-entry vector of std::function
// objects. It was rebuilt for every compiled unit even though its only input,
// the address space's UniformInfo, is fixed for the life of that address space
// -- so under lazy function compilation it ran once per decoded guest block.
//
// Cached per thread and keyed on the UniformInfo pointer rather than in a
// plain static: an embedder can hold several Instances with different uniform
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

void* TranslateIR(const std::shared_ptr<backend::Module>& module, ir::HIRFunction* function) {
    auto ir_function = function->GetFunction();
    auto func_start = ir_function->GetStartLocation().Value();
    PrepareFunctionGuestRanges(function);
    auto& jit_state = ir_function->GetJitCache();
    // Establish a consistent RPO layout before allocation/emission: the
    // function-level linear scan and the emitter (Translate(HIRFunction*) walks
    // GetHIRBlocksRPO) both assume instruction ids are dense in emission order.
    // ComputeRPO fills blocks_rpo; IdByRPO renumbers every instruction 0..N-1 in
    // that order (and re-keys the HIRValue map), so a value's OrderId lines up
    // with where it is actually emitted. Skipped for a single-block function
    // beyond the (harmless) renumber. Must run after EndFunction (the driver
    // calls it) since predecessors/successors are built there.
    PerfScope perf_rpo{GetPerfStats().rpo_ns};
    function->ComputeRPO();
    function->IdByRPO();
    perf_rpo.Stop();
    static const bool dump_ir = std::getenv("SVM_DUMP_IR") != nullptr;
    if (dump_ir) fmt::print(stderr, "[func-compile] {:#x} rpo-ready\n", func_start);
    const auto& address_space = module->GetAddressSpace();
    const ir::UniformInfo* uni_info = address_space.GetUniformInfo().uniform_size
                                      ? &address_space.GetUniformInfo() : nullptr;
    PerfScope perf_opt{GetPerfStats().opt_ns};
    GetPassPipeline(uni_info).RunFunction(function, module->GetModuleConfig().optimizations);
    perf_opt.Stop();
    // The function passes delete instructions (flag elimination, then dead-code
    // elimination), which punches holes in the numbering established above.
    // RegisterAllocPass indexes its interval table by instruction id and sizes
    // it from MaxInstrCount(), so the ids must be dense in emission order
    // again before allocation. This is the function-mode counterpart of
    // PassPipeline::RunBlock's trailing Block::ReIdInstr().
    PerfScope perf_rpo2{GetPerfStats().rpo_ns};
    function->IdByRPO();
    perf_rpo2.Stop();
    if (dump_ir) fmt::print(stderr, "[func-compile] {:#x} opts-ready\n", func_start);
    auto gprs{address_space.GetTrampolines().GetGPRRegs()};
    const bool reserve_x87_topvirt =
            X87TopVirtRequested() && HasX87Op(function);
    if (reserve_x87_topvirt) {
        // Dedicated callee-saved TOP/cache-validity register. Reserve it
        // before allocation so neither block nor function mode can assign a
        // live IR value to it.
        gprs.Mark(20);
    }
    auto fprs{address_space.GetTrampolines().GetFPRRegs()};
    if (reserve_x87_topvirt) {
        // Pin the hot half of the x87 stack only. Reserving all eight FPRs
        // exhausted the finite spill arena in large function-mode units.
        for (u32 code = 28; code < 32; ++code) {
            fprs.Mark(code);
        }
    }
    PerfScope perf_ra{GetPerfStats().regalloc_ns};
    backend::RegAlloc reg_alloc{static_cast<u32>(function->MaxInstrCount()), gprs, fprs};
    ir::RegisterAllocPass::Run(function, &reg_alloc);
    perf_ra.Stop();
    if (dump_ir) fmt::print(stderr, "[func-compile] {:#x} regalloc-ready\n", func_start);
    PerfScope perf_cg{GetPerfStats().codegen_ns};
    backend::arm64::JitContext context{module, reg_alloc};
    backend::arm64::JitTranslator translator{context};
    translator.Translate(function);
    perf_cg.Stop();
    if (dump_ir) fmt::print(stderr, "[func-compile] {:#x} emit-ready\n", func_start);
    auto buffer_size = context.CurrentBufferSize();
    if (dump_ir) fmt::print(stderr, "[func-compile] {:#x} size={}\n", func_start, buffer_size);
    if (auto [idx, buffer] = module->AllocCodeCache(buffer_size);
        idx != backend::INVALID_CACHE_ID) {
        PerfScope perf_pub{GetPerfStats().publish_ns};
        PerfAdd(GetPerfStats().host_bytes, buffer_size);
        PerfAdd(GetPerfStats().ir_insts, function->MaxInstrCount());
        if (PerfPerUnit()) {
            fmt::print(stderr, "[svm-unit] pc={:#x} ir={} host={}\n", func_start,
                       function->MaxInstrCount(), buffer_size);
        }
        context.Flush(buffer);
        if (dump_ir) fmt::print(stderr, "[func-compile] {:#x} flush-ready\n", func_start);
        jit_state.jit_state = backend::JitState::Cached;
        jit_state.cache_id = idx;
        jit_state.offset_in = buffer.offset;
        jit_state.cache_size = buffer.size;
        if (!module->Push(ir_function)) {
            jit_state = {};
            if (auto* cache = module->GetCodeCache(buffer.exec_data)) {
                cache->FreeCode(buffer.exec_data);
            }
            return nullptr;
        }
        function->ReleaseFunctionOwnership();
        if (dump_ir) fmt::print(stderr, "[func-compile] {:#x} publish-ready\n", func_start);
        module->AddFaultEntry(buffer.exec_data, buffer.exec_data + buffer.size, func_start);

        // Publish every decoded block label, not only the function entry.
        // External links, RSB return targets, and code misses are allowed to
        // land at a basic-block boundary inside this compiled unit.
        auto& mutable_address_space = module->GetAddressSpace();
        std::vector<backend::SerialBlock> cache_blocks;
        for (auto& hir_block : function->GetHIRBlocksRPO()) {
            auto* block = hir_block.GetBlock();
            if (block->GetInstList().empty()) {
                continue;
            }
            const auto guest = block->GetStartLocation().Value();
            const auto offset = context.GetCodeOffset(guest);
            ASSERT(offset >= 0 && static_cast<size_t>(offset) < buffer.size);
            mutable_address_space.PushCodeCache(guest, buffer.exec_data + offset);
            cache_blocks.push_back({guest,
                                    block->GetEndLocation().Value(),
                                    static_cast<u32>(offset),
                                    0});
            if (!module->GetModuleConfig().read_only) {
                mutable_address_space.GetSmcTracker().RegisterNode(
                        module,
                        ir_function,
                        block->GetStartLocation().Value(),
                        block->GetEndLocation().Value());
            }
        }
        if (dump_ir) fmt::print(stderr, "[func-compile] {:#x} entries-ready\n", func_start);
        RecordJitCacheUnit(module, func_start, true, cache_blocks, buffer);
        return buffer.exec_data;
    }
    return nullptr;
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
    auto gprs{address_space.GetTrampolines().GetGPRRegs()};
    const bool reserve_x87_topvirt =
            X87TopVirtRequested() && HasX87Op(ir_block);
    if (reserve_x87_topvirt) {
        gprs.Mark(20);
    }
    auto fprs{address_space.GetTrampolines().GetFPRRegs()};
    if (reserve_x87_topvirt) {
        for (u32 code = 28; code < 32; ++code) {
            fprs.Mark(code);
        }
    }
    backend::RegAlloc reg_alloc{static_cast<u32>(block->MaxInstrCount()), gprs, fprs};
    backend::arm64::JitContext context{module, reg_alloc};
    backend::arm64::JitTranslator translator{context};
    translator.Translate(block->GetBlock());
    auto buffer_size = context.CurrentBufferSize();
    if (auto [idx, buffer] = module->AllocCodeCache(buffer_size);
        idx != backend::INVALID_CACHE_ID) {
        context.Flush(buffer);
        module->AddFaultEntry(buffer.exec_data, buffer.exec_data + buffer.size, block_start);
        jit_state.jit_state = backend::JitState::Cached;
        jit_state.cache_id = idx;
        jit_state.offset_in = buffer.offset;
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
    const auto& address_space = module->GetAddressSpace();

    // Optimize passes
    const ir::UniformInfo* uni_info = address_space.GetUniformInfo().uniform_size
                                      ? &address_space.GetUniformInfo() : nullptr;
    GetPassPipeline(uni_info).RunBlock(block.get(), module_config.optimizations);

    auto gprs{address_space.GetTrampolines().GetGPRRegs()};
    const bool reserve_x87_topvirt =
            X87TopVirtRequested() && HasX87Op(block.get());
    if (reserve_x87_topvirt) {
        gprs.Mark(20);
    }
    auto fprs{address_space.GetTrampolines().GetFPRRegs()};
    if (reserve_x87_topvirt) {
        for (u32 code = 28; code < 32; ++code) {
            fprs.Mark(code);
        }
    }
    backend::RegAlloc reg_alloc{static_cast<u32>(block->MaxInstrId()), gprs, fprs};

    ir::RegisterAllocPass::Run(block.get(), &reg_alloc);

    backend::arm64::JitContext context{module, reg_alloc};
    backend::arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    auto buffer_size = context.CurrentBufferSize();
    if (auto [idx, buffer] = module->AllocCodeCache(buffer_size);
        idx != backend::INVALID_CACHE_ID) {
        context.Flush(buffer);
        module->AddFaultEntry(buffer.exec_data,
                              buffer.exec_data + buffer.size,
                              block->GetStartLocation().Value());
        jit_state.jit_state = backend::JitState::Cached;
        jit_state.cache_id = idx;
        jit_state.offset_in = buffer.offset;
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
            address_space.GetSmcTracker().RegisterNode(
                    module, block.get(), block_start, block->GetEndLocation().Value());
        }
        {
            const VAddr start = block->GetStartLocation().Value();
            const std::vector<backend::SerialBlock> cache_blocks{
                    {start, block->GetEndLocation().Value(), 0, 0}};
            RecordJitCacheUnit(module, start, false, cache_blocks, buffer);
        }
        block->DestroyInstrs();
        return buffer.exec_data;
    }
    return nullptr;
}

}  // namespace backend

}  // namespace swift::runtime
