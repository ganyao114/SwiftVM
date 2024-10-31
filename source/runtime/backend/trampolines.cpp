//
// Created by 甘尧 on 2024/4/10.
//

#include "trampolines.h"

namespace swift::runtime::backend {

Trampolines::Trampolines(const Config& config)
        : config(config), code_cache(config, 4_MB) {}

bool Trampolines::LinkBlock(u8* source, u8* target, u8* source_rw, bool pic) { return false; }

std::optional<Trampolines::CallHost> Trampolines::GetCallHost(HostFunction* func, ISA frontend) {
    {
        std::unique_lock guard(lock);
        host_functions[func->addr] = func;
    }
    return call_host;
}

}  // namespace swift::runtime::backend
