//
// Created by 甘尧 on 2024/4/10.
//

#include "trampolines.h"

namespace swift::runtime::backend {

Trampolines::Trampolines(const Config& config, const FeatureSet& features)
        : config(config), code_cache(config, 4_MB, features) {}

Trampolines::~Trampolines() {
    // Derived construction and the remaining AddressSpace metadata may still
    // fail after the arena is acquired. No executable entry has escaped yet.
    // MemMap retains any failed checked release without allocating in unwind.
    if (!construction_committed) (void)code_cache.ReleaseAfterQuiescence();
}

std::optional<Trampolines::CallHost> Trampolines::GetCallHost(HostFunction* func, ISA frontend) {
    {
        std::unique_lock guard(lock);
        if (release_started) return std::nullopt;
        host_functions[func->addr] = func;
    }
    return call_host;
}

MemMap::ReleaseResult Trampolines::ReleaseAfterQuiescence() {
    std::unique_lock guard(lock);
    release_started = true;
    RevokeHostCallAliases();
    runtime_entry = nullptr;
    return_host = nullptr;
    call_host = nullptr;
    indirect_l1_miss = nullptr;
    pair_call = nullptr;
    host_functions.clear();
    return code_cache.ReleaseAfterQuiescence();
}

}  // namespace swift::runtime::backend
