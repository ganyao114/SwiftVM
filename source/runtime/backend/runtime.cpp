//
// Created by 甘尧 on 2023/9/7.
//
#pragma once

#include <atomic>
#include <utility>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/constant.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/context.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/signal_handler.h"
#include "runtime/backend/translate_table.h"
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

struct Runtime::Impl final {
    explicit Impl(backend::AddressSpace* address_space) : address_space(address_space) {
        state_buffer.resize(sizeof(backend::State) +
                            address_space->GetConfig().uniform_buffer_size);
        state = reinterpret_cast<backend::State*>(state_buffer.data());
        // Wire the dispatcher's code-cache tables: L1 is per-runtime, L2 is the
        // address-space wide translate table that PushCodeCache writes to.
        state->l1_code_cache = l1_code_cache.Data();
        state->l2_code_cache = address_space->GetCodeCacheTable().Data();
        // Guest address virtualization: Config::memory_base carries the
        // guest->host bias (host = guest + bias); the JIT keeps it in the
        // reserved pt register and the interpreter reads it from here.
        // nullptr (identity) keeps the zero-overhead fast path.
        state->pt = address_space->GetConfig().memory_base;
        jit_entry = address_space->GetTrampolines().GetRuntimeEntry();
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
    // SmcTracker::RegisterBlock; the tracker opens a write window (page back
    // to RW, stale blocks' dispatch slots zeroed) and the faulting store is
    // re-executed on sigreturn. Actual invalidation is deferred to
    // CloseWriteWindow after the current JitRun returns.
    static constexpr int kSmcPriority = 0;

    static bool HandleSmcFault(void* ctx, ucontext_t* uctx, int sig, siginfo_t* info) {
        auto* self = static_cast<Impl*>(ctx);
        if (tls_active_runtime != self) {
            return false;  // not executing on this thread
        }
        if (sig != SIGSEGV && sig != SIGBUS) {
            return false;
        }
        const auto fault_addr = reinterpret_cast<std::uintptr_t>(info->si_addr);
        return self->address_space->GetSmcTracker().HandleWriteFault(
                *self->address_space, self->l1_code_cache, fault_addr);
    }

    static bool HandleFault(void* ctx, ucontext_t* uctx, int sig, siginfo_t* info) {
        auto* self = static_cast<Impl*>(ctx);
        if (tls_active_runtime != self) {
            return false;  // not executing on this thread
        }
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

        HaltReason hr{HaltReason::None};

        while (hr == HaltReason::None) {
            // Re-read the location every iteration: interpreted blocks update
            // state->current_loc through their terminals (LinkBlock /
            // SetLocation), so the dispatcher must follow it like the JIT
            // trampolines code_dispatcher loop does.
            current_loc = state->current_loc.Value();
            if (auto node = current_module->GetNode(current_loc); !backend::IsEmpty(node)) {
                hr = VisitVariant<HaltReason>(node, [this](auto x) -> auto {
                    using T = std::decay_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, IntrusivePtr<ir::Function>>) {
                        auto read_lock = x->LockRead();
                        auto current_block = x->EntryBlock();
                        if (!current_block) return HaltReason::CodeMiss;
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
                // SMC write-window close (Phase 4): if a guest store hit a
                // write-protected code page during this JitRun, the stale
                // translations are invalidated now — the guest is back on
                // the host side, so freeing JIT code and editing module
                // maps is safe. Runs before the hr checks so it also covers
                // the CodeMiss/CacheMiss exits. NOTE: with DirectBlockLink
                // disabled HaltReason::BlockLinkage is never produced; if it
                // is ever enabled, the linkage patch below must be ordered
                // against invalidation of the *previous* block.
                address_space->GetSmcTracker().CloseWriteWindow(*address_space, l1_code_cache);
            } else {
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
    backend::AddressSpace* address_space{};
    // mutable: the JIT dispatcher writes L1 entries through the raw
    // state->l1_code_cache pointer even from const Run paths.
    mutable TranslateTable l1_code_cache{l1_cache_bits};
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
    backend::SignalHandler::RegisterHandler(&Impl::HandleSmcFault, impl.get(), Impl::kSmcPriority);
    backend::SignalHandler::RegisterHandler(&Impl::HandleFault, impl.get(), Impl::kFaultPriority);
}

Runtime::~Runtime() {
    backend::SignalHandler::UnregisterHandler(impl.get());
}

HaltReason Runtime::Run() { return impl->Run(); }

HaltReason Runtime::Step() { return HaltReason::None; }

void Runtime::SignalInterrupt() {}

void Runtime::ClearInterrupt() {}

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

void* TranslateIR(const std::shared_ptr<backend::Module>& module, ir::HIRFunction* function) {
    auto ir_function = function->GetFunction();
    auto func_start = ir_function->GetStartLocation().Value();
    if (!module->Push(ir_function)) {
        return nullptr;
    }

    auto guard = ir_function->LockWrite();
    auto& jit_state = ir_function->GetJitCache();
    if (jit_state.jit_state == backend::JitState::Cached) {
        return module->GetJitCache(jit_state);
    }
    const auto& address_space = module->GetAddressSpace();
    auto gprs{address_space.GetTrampolines().GetGPRRegs()};
    auto fprs{address_space.GetTrampolines().GetFPRRegs()};
    backend::RegAlloc reg_alloc{static_cast<u32>(function->MaxInstrCount()), gprs, fprs};
    ir::RegisterAllocPass::Run(function, &reg_alloc);
    backend::arm64::JitContext context{module, reg_alloc};
    backend::arm64::JitTranslator translator{context};
    translator.Translate(function);
    auto buffer_size = context.CurrentBufferSize();
    if (auto [idx, buffer] = module->AllocCodeCache(buffer_size);
        idx != backend::INVALID_CACHE_ID) {
        context.Flush(buffer);
        module->AddFaultEntry(buffer.exec_data, buffer.exec_data + buffer.size, func_start);
        jit_state.jit_state = backend::JitState::Cached;
        jit_state.cache_id = idx;
        jit_state.offset_in = buffer.offset;
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
    auto fprs{address_space.GetTrampolines().GetFPRRegs()};
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
    auto pipeline = ir::PassPipeline::BuildDefault(uni_info);
    pipeline.RunBlock(block.get(), module_config.optimizations);

    auto gprs{address_space.GetTrampolines().GetGPRRegs()};
    auto fprs{address_space.GetTrampolines().GetFPRRegs()};
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
            address_space.GetSmcTracker().RegisterBlock(
                    block.get(), block_start, block->GetEndLocation().Value());
        }
        block->DestroyInstrs();
        return buffer.exec_data;
    }
    return nullptr;
}

}  // namespace backend

}  // namespace swift::runtime