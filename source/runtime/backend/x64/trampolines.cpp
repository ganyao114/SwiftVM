#include "trampolines.h"

#include <atomic>
#include <stdexcept>
#include "runtime/backend/context.h"
#include "runtime/backend/translate_table.h"

namespace swift::runtime::backend::x64 {
namespace {

HaltReason Enter(void* opaque, void* initial_code) {
    auto* state = static_cast<State*>(opaque);
    auto* code = initial_code;
    using Entry = HaltReason (*)(State*);
    const auto finish = [&](HaltReason reason) { state->halt_reason = HaltReason::None; return reason; };
    for (;;) {
        const auto request = std::atomic_ref<u64>{state->exit_request}.load(std::memory_order_acquire);
        if (request) return finish(request >> 63 ? HaltReason::Signal : HaltReason::CodeMiss);
        if (!code) return finish(HaltReason::CacheMiss);
        const auto reason = reinterpret_cast<Entry>(code)(state);
        if (reason != HaltReason::None) return finish(reason);
        const auto location = state->current_loc.Value();
        auto hash = location >> 2;
        hash = (hash ^ (hash >> HASH_TABLE_PAGE_BITS)) & ((u64{1} << HASH_TABLE_PAGE_BITS) - 1);
        auto* table = static_cast<u64*>(state->l2_code_cache);
        code = nullptr;
        for (u64 index = hash; index < (u64{1} << HASH_TABLE_PAGE_BITS); ++index) {
            const auto key = std::atomic_ref<u64>{table[index * 2]}.load(std::memory_order_acquire);
            if (key == location) {
                code = reinterpret_cast<void*>(std::atomic_ref<u64>{table[index * 2 + 1]}.load(std::memory_order_acquire));
                if (!code) return finish(HaltReason::CacheMiss);
                break;
            }
            if (!key) return finish(HaltReason::CodeMiss);
        }
        if (!code) return finish(HaltReason::CodeMiss);
    }
}

void CommittedReturnHost() {}
}

TrampolinesX64::TrampolinesX64(const Config& config) : Trampolines(config) {
    if (config.enable_asm_interp)
        throw std::invalid_argument("x64 does not support the ARM64 assembly interpreter");
    runtime_entry = &Enter;
    return_host = &CommittedReturnHost;
    built = true;
}

}  // namespace swift::runtime::backend::x64
