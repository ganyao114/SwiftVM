//
// Created by 甘尧 on 2023/9/27.
//
#pragma once

#include <optional>
#include "dlmalloc/malloc.h"
#include "runtime/backend/cache_clear.h"
#include "runtime/backend/mem_map.h"
#include "runtime/common/types.h"
#include "runtime/include/config.h"

namespace swift::runtime::backend {

struct CodeBuffer {
    explicit CodeBuffer(u8* exec, u8* rw, u32 offset, size_t size)
            : exec_data(exec), rw_data(rw), offset(offset), size(size) {}

    inline void Flush() const {
        ClearDCache(rw_data, size);
        ClearDCache(exec_data, size);
        ClearICache(exec_data, size);
    }

    u8* exec_data;
    u8* rw_data;
    u32 offset;
    size_t size;
};

class CodeCache {
public:
    explicit CodeCache(const Config& config, u32 size, bool read_only = false);

    ~CodeCache();

    [[nodiscard]] std::optional<CodeBuffer> AllocCode(size_t size);
    bool FreeCode(u8* exec_ptr);
    [[nodiscard]] bool Contain(const u8* exec_ptr);
    [[nodiscard]] u8* GetExePtr(u32 offset);
    [[nodiscard]] u8* GetRWPtr(u32 offset);
    [[nodiscard]] u8* GetRWPtr(const u8* exec_ptr);

private:
    void Init();

    const Config& config;
    const size_t inst_alignment;
    const bool read_only;
    u32 max_size;
    mspace space_code{};
    std::unique_ptr<MemMap> code_mem;

    u8* code_mem_mapped{};
    u8* code_cursor{};
};

}  // namespace swift::runtime::backend