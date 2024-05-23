//
// Created by 甘尧 on 2024/4/10.
//

#pragma once

#include "runtime/backend/code_cache.h"
#include "runtime/common/types.h"
#include "runtime/include/sruntime.h"

namespace swift::runtime::backend {

class Trampolines : DeleteCopyAndMove {
public:
    using RuntimeEntry = HaltReason (*)(void* state_, void* cache_);
    using ReturnHost = void (*)();

    explicit Trampolines(const Config& config, const CodeBuffer& buffer);

    virtual void Build(){};

    virtual bool LinkBlock(u8* source, u8* target, u8* source_rw, bool pic);

    [[nodiscard]] RuntimeEntry GetRuntimeEntry() const { return runtime_entry; }

    [[nodiscard]] ReturnHost GetReturnHost() const { return return_host; }

protected:
    bool built = false;
    const Config& config;
    const CodeBuffer code_buffer;
    RuntimeEntry runtime_entry{};
    ReturnHost return_host{};
};

}  // namespace swift::runtime::backend
