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
#include "runtime/ir/opts/register_alloc_pass.h"

namespace swift::runtime {

constexpr static auto l1_cache_bits = 18;

std::unique_ptr<Instance> Instance::Make(const Config& config) {
    return std::make_unique<backend::AddressSpace>(config);
}

struct Runtime::Impl final {
    Impl(Runtime* rt, backend::AddressSpace* address_space) : rt(rt), address_space(address_space) {
        state_buffer.resize(sizeof(backend::State) +
                            address_space->GetConfig().uniform_buffer_size);
        state = reinterpret_cast<backend::State*>(state_buffer.data());
    }

    [[nodiscard]] HaltReason Interpreter() const {
        auto current_loc = state->current_loc.Value();
        auto current_module{address_space->GetModule(current_loc)};

        if (!current_module) {
            return HaltReason::CodeMiss | HaltReason::ModuleMiss;
        }

        HaltReason hr{HaltReason::None};

        while (hr == HaltReason::None) {
            if (auto func = current_module->GetFunction(current_loc); func) {
                auto read_lock = func->LockRead();
                auto current_block = func->EntryBlock();
            } else if (auto block = current_module->GetBlock(current_loc); block) {
                auto read_lock = block->LockRead();
                backend::interp::Interpreter interpreter{*state, block.get()};
                hr = interpreter.Run();
            } else {
                hr = HaltReason::CodeMiss;
                break;
            }
        }
        return hr;
    }

    void RuntimeTranslateIR(std::shared_ptr<backend::Module> module, ir::Block* block) {
#ifdef __aarch64__
        RuntimeTranslateIRArm64(std::move(module), block);
#endif
    }

    void RuntimeTranslateIR(ir::Function* function) {}

    void RuntimeTranslateIRArm64(const std::shared_ptr<backend::Module>& module, ir::Block* block) {
        backend::GPRSMask gprs{ARM64_X_REGS_MASK};
        backend::FPRSMask fprs{ARM64_V_REGS_MASK};
        backend::RegAlloc reg_alloc{static_cast<u32>(block->GetInstList().size()), gprs, fprs};
        backend::arm64::JitContext context{module, reg_alloc};
        backend::arm64::JitTranslator translator{context};
        translator.Translate();
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
    Runtime* rt;
    backend::AddressSpace* address_space{};
    TranslateTable l1_code_cache{l1_cache_bits};
    backend::interp::InterpStack interp_stack;
    std::atomic_bool running{true};
};

Runtime::Runtime(Instance* instance)
        : impl(std::make_unique<Impl>(this, reinterpret_cast<backend::AddressSpace*>(instance))) {}

HaltReason Runtime::Run() {
    HaltReason hr{HaltReason::None};
    auto address_space = impl->address_space;
    while (impl->running.load(std::memory_order_acquire)) {
        auto current_loc = GetLocation();
        if (auto cache = hr != HaltReason::CacheMiss ? address_space->GetCodeCache(current_loc)
                                                     : nullptr;
            cache) {
            if (hr == HaltReason::BlockLinkage) {
                // Do linkage
                auto linkage_cache_place = impl->state->blocking_linkage_address;
                auto pre_block_vaddr = impl->state->prev_loc.Value();
                impl->LinkBlock(pre_block_vaddr, linkage_cache_place, current_loc, cache);
            }
            // JIT Run!
            auto jit_entry = address_space->GetTrampolines().GetRuntimeEntry();
            hr = jit_entry(impl->state, cache);
        } else {
            // IR Interpreter
            hr = impl->Interpreter();
        }
        if (hr == HaltReason::CacheMiss || hr == HaltReason::BlockLinkage) {
            continue;
        } else {
            break;
        }
    }
    return hr;
}

HaltReason Runtime::Step() { return HaltReason::None; }

void Runtime::SignalInterrupt() {
    // #ifdef _MSC_VER
    //     _InterlockedOr(reinterpret_cast<volatile long*>(ptr), value);
    // #else
    //     __atomic_or_fetch((u32*) &impl->state->halt_reason, (u32) backend::HaltReason::Signal,
    //     __ATOMIC_SEQ_CST);
    // #endif
}

void Runtime::ClearInterrupt() {}

void Runtime::SetLocation(LocationDescriptor location) {
    impl->state->current_loc = ir::Location(location);
}

LocationDescriptor Runtime::GetLocation() { return impl->state->current_loc.Value(); }

std::span<u8> Runtime::GetUniformBuffer() const {
    return {(u8*)&impl->state->uniform_buffer_begin,
            impl->address_space->GetConfig().uniform_buffer_size};
}

void* PushAndTranslateHIR(std::shared_ptr<backend::Module> module, ir::HIRFunction* function) {
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
    backend::GPRSMask gprs{ARM64_X_REGS_MASK};
    backend::FPRSMask fprs{ARM64_V_REGS_MASK};
    backend::RegAlloc reg_alloc{static_cast<u32>(function->MaxInstrCount()), gprs, fprs};
    ir::RegisterAllocPass::Run(function, &reg_alloc);
    backend::arm64::JitContext context{module, reg_alloc};
    backend::arm64::JitTranslator translator{context};
    translator.Translate();
    context.Finish();
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

bool PushAndTranslateHIR(const std::shared_ptr<backend::Module>& module, ir::HIRBlock* block) {
    auto ir_block = block->GetBlock();
    auto block_start = ir_block->GetStartLocation().Value();
    if (!module->Push(ir_block)) {
        return false;
    }

    auto guard = ir_block->LockWrite();
    auto& jit_state = ir_block->GetJitCache();
    if (jit_state.jit_state == backend::JitState::Cached) {
        return true;
    }
    backend::GPRSMask gprs{ARM64_X_REGS_MASK};
    backend::FPRSMask fprs{ARM64_V_REGS_MASK};
    backend::RegAlloc reg_alloc{static_cast<u32>(block->MaxInstrCount()), gprs, fprs};
    backend::arm64::JitContext context{module, reg_alloc};
    backend::arm64::JitTranslator translator{context};
    translator.Translate();
    context.Finish();
    auto buffer_size = context.CurrentBufferSize();
    if (auto [idx, buffer] = module->AllocCodeCache(buffer_size);
        idx != backend::INVALID_CACHE_ID) {
        context.Flush(buffer);
        jit_state.jit_state = backend::JitState::Cached;
        jit_state.cache_id = idx;
        jit_state.offset_in = buffer.offset;
        return true;
    }
}

}  // namespace swift::runtime