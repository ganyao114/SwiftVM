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
#include "runtime/backend/translate_table.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/function.h"
#include "runtime/ir/opts/const_folding_pass.h"
#include "runtime/ir/opts/deadcode_elimination_pass.h"
#include "runtime/ir/opts/flags_elimination_pass.h"
#include "runtime/ir/opts/local_elimination_pass.h"
#include "runtime/ir/opts/register_alloc_pass.h"
#include "runtime/ir/opts/uniform_elimination_pass.h"

namespace swift::runtime {

constexpr static auto l1_cache_bits = 18;

std::unique_ptr<Instance> Instance::Make(const Config& config) {
    return std::make_unique<backend::AddressSpace>(config);
}

struct Runtime::Impl final {
    explicit Impl(backend::AddressSpace* address_space) : address_space(address_space) {
        state_buffer.resize(sizeof(backend::State) +
                            address_space->GetConfig().uniform_buffer_size);
        state = reinterpret_cast<backend::State*>(state_buffer.data());
        jit_entry = address_space->GetTrampolines().GetRuntimeEntry();
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
            if (auto node = current_module->GetNode(current_loc); !backend::IsEmpty(node)) {
                hr = VisitVariant<HaltReason>(node, [this](auto x) -> auto {
                    using T = std::decay_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, IntrusivePtr<ir::Function>>) {
                        auto read_lock = x->LockRead();
                        auto current_block = x->EntryBlock();
                        return HaltReason::None;
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
        return hr;
    }

    HaltReason Run() {
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
    TranslateTable l1_code_cache{l1_cache_bits};
    backend::interp::InterpStack interp_stack;
    std::atomic_bool running{true};
    backend::Trampolines::RuntimeEntry jit_entry{};
};

Runtime::Runtime(Instance* instance)
        : impl(std::make_unique<Impl>(reinterpret_cast<backend::AddressSpace*>(instance))) {}

Runtime::~Runtime() = default;

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
    if (module_config.HasOpt(Optimizations::LocalElimination)) {
        ir::LocalEliminationPass::Run(block.get());
    }
    if (module_config.HasOpt(Optimizations::UniformElimination)) {
        ir::UniformEliminationPass::Run(block.get(), address_space.GetUniformInfo());
    }
    if (module_config.HasOpt(Optimizations::FlagElimination)) {
        ir::FlagsEliminationPass::Run(block.get());
    }
    if (module_config.HasOpt(Optimizations::ConstantFolding)) {
        ir::ConstFoldingPass::Run(block.get());
    }
    if (module_config.HasOpt(Optimizations::DeadCodeRemove)) {
        ir::DeadCodeEliminationPass::Run(block.get());
    }

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
        jit_state.jit_state = backend::JitState::Cached;
        jit_state.cache_id = idx;
        jit_state.offset_in = buffer.offset;
        block->DestroyInstrs();
        return buffer.exec_data;
    }
    return nullptr;
}

}  // namespace backend

}  // namespace swift::runtime