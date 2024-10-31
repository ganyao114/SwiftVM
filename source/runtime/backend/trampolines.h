//
// Created by 甘尧 on 2024/4/10.
//

#pragma once

#include <shared_mutex>
#include <vector>
#include "runtime/backend/host_function.h"
#include "runtime/backend/code_cache.h"
#include "runtime/backend/reg_alloc.h"
#include "runtime/common/cityhash.h"
#include "runtime/common/types.h"
#include "runtime/include/sruntime.h"

namespace swift::runtime::backend {

class Trampolines : DeleteCopyAndMove {
public:
    using RuntimeEntry = HaltReason (*)(void* state_, void* cache_);
    using ReturnHost = void (*)();
    using CallHost = void (*)();

    explicit Trampolines(const Config& config);

    virtual bool LinkBlock(u8* source, u8* target, u8* source_rw, bool pic);

    virtual std::optional<CallHost> GetCallHost(HostFunction* func, ISA frontend);

    [[nodiscard]] RuntimeEntry GetRuntimeEntry() const { return runtime_entry; }

    [[nodiscard]] ReturnHost GetReturnHost() const { return return_host; }

    [[nodiscard]] GPRSMask GetGPRRegs() const { return gpr_regs; }

    [[nodiscard]] FPRSMask GetFPRRegs() const { return fpr_regs; }

protected:
    bool built = false;
    const Config& config;
    CodeCache code_cache;
    RuntimeEntry runtime_entry{};
    ReturnHost return_host{};
    CallHost call_host{};
    GPRSMask gpr_regs{};
    GPRSMask fpr_regs{};
    std::shared_mutex lock{};
    std::unordered_map<LocationDescriptor, HostFunction *> host_functions{};
};

}  // namespace swift::runtime::backend
