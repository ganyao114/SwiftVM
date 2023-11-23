//
// Created by 甘尧 on 2023/9/27.
//
#pragma once

#include <optional>
#include "runtime/common/types.h"
#include "runtime/include/config.h"
#include "runtime/backend/mem_map.h"
#include "dlmalloc/malloc.h"

namespace swift::runtime::backend {

struct CodeBuffer {
    explicit CodeBuffer(u8 *exec, u8 *rw, size_t size) : exec_data(exec), rw_data(rw), size(size) {}

    inline void Flush() const {
        ClearDCache(rw_data, size);
        ClearDCache(exec_data, size);
        ClearICache(exec_data, size);
    }

    u8 *exec_data;
    u8 *rw_data;
    size_t size;
};

class CodeCache {
public:
    explicit CodeCache(const Config& config, u32 size);

    virtual ~CodeCache();

    std::optional<CodeBuffer> AllocCode(size_t size);
    bool FreeCode(u8 *exec_ptr);
    bool Contain(u8 *exec_ptr);

private:
    void Init();

    const Config &config;
    const size_t inst_alignment;
    u32 max_size;
    mspace space_code{};
    mspace space_data{};
    std::unique_ptr<MemMap> code_mem;
    std::unique_ptr<MemMap> data_mem;

    u8 *code_mem_mapped{};
};

}  // namespace swift::runtime::backend