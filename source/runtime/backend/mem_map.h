//
// Created by 甘尧 on 2023/9/27.
//
#pragma once

#include "runtime/common/common_funcs.h"
#include "runtime/common/types.h"

namespace swift::runtime::backend {

void ClearICache(void *start, size_t size);
void ClearDCache(void *start, size_t size);

class MemMap : DeleteCopyAndMove {
public:
    enum Mode {
        None = 0,
        Read = 1 << 0,
        Write = 1 << 1,
        Executable = 1 << 2,
        ReadWrite = Read | Write,
        ReadExe = Read | Executable,
        RWX = Read | Write | Executable
    };

    explicit MemMap(u32 size, bool executable = false);

    virtual ~MemMap();

    void* Map(u32 size, u32 offset, Mode mode, bool pri = false);
    void Protect(u32 size, u32 offset, Mode mode);
    void Unmap(void* mem, u32 size);
    void Free(u32 offset, u32 size);
    u8* GetMemory();
    u32 GetSize() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
    u32 arena_size;
};

}  // namespace swift::runtime::backend