//
// Created by 甘尧 on 2023/9/7.
//
#pragma once

#include <atomic>
#include "runtime/backend/address_space.h"
#include "runtime/backend/context.h"
#include "runtime/backend/entrypoint.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/translate_table.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/arm64/constant.h"
#include "runtime/common/types.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/function.h"

namespace swift::runtime {

constexpr static auto l1_cache_bits = 18;

struct Interface::Impl final {
    Impl(Interface* interface, Config conf) : interface(interface), conf(std::move(conf)) {
        state_buffer.resize(sizeof(backend::State) + conf.uniform_buffer_size);
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
            if (auto [func, func_lock] = current_module->LockReadFunction(current_loc); func) {
                auto current_block = func->EntryBlock();

            } else if (auto [block, block_lock] = current_module->LockReadBlock(current_loc); block) {
                backend::interp::Interpreter interpreter{*state, block};
                hr = interpreter.Run();
            } else {
                hr = HaltReason::CodeMiss;
                break;
            }
        }
        return hr;
    }

    void RuntimeTranslateIR(ir::Block *block) {
#ifdef __aarch64__
        RuntimeTranslateIRArm64(block);
#endif
    }

    void RuntimeTranslateIR(ir::Function *function) {

    }

    void RuntimeTranslateIRArm64(ir::Block *block) {
        backend::GPRSMask gprs{ARM64_X_REGS_MASK};
        backend::FPRSMask fprs{ARM64_V_REGS_MASK};
        backend::RegAlloc reg_alloc{static_cast<u32>(block->GetInstList().size()), gprs, fprs};
        backend::arm64::JitContext context{conf, reg_alloc};
        backend::arm64::JitTranslator translator{context};
        translator.Translate();
    }

    std::vector<u8> state_buffer{};
    backend::State* state{};
    Interface* interface;
    Instance* instance{};
    const Config conf;
    backend::AddressSpace* address_space{};
    TranslateTable l1_code_cache{l1_cache_bits};
    backend::interp::InterpStack interp_stack;
};

Interface::Interface(Config config) : impl(std::make_unique<Impl>(this, std::move(config))) {}

HaltReason Interface::Run() {
    HaltReason hr{HaltReason::None};
    while (hr == HaltReason::None) {
        auto current_loc = GetLocation();
        if (auto cache = impl->address_space->GetCodeCache(current_loc); cache) {
            // JIT Run!
            hr = static_cast<HaltReason>(swift_runtime_entry(impl->state, cache));
        } else {
            // IR Interpreter
            hr = impl->Interpreter();
        }
    }
    return hr;
}

HaltReason Interface::Step() { return HaltReason::None; }

void Interface::SignalInterrupt() {
    // #ifdef _MSC_VER
    //     _InterlockedOr(reinterpret_cast<volatile long*>(ptr), value);
    // #else
    //     __atomic_or_fetch((u32*) &impl->state->halt_reason, (u32) backend::HaltReason::Signal,
    //     __ATOMIC_SEQ_CST);
    // #endif
}

void Interface::ClearInterrupt() {}

void Interface::SetLocation(LocationDescriptor location) {
    impl->state->current_loc = ir::Location(location);
}

LocationDescriptor Interface::GetLocation() { return impl->state->current_loc.Value(); }

std::span<u8> Interface::GetUniformBuffer() const {
    return {(u8*)&impl->state->uniform_buffer_begin, impl->conf.uniform_buffer_size};
}

}  // namespace swift::runtime