//
// Created by 甘尧 on 2023/9/27.
//

#include "runtime/backend/code_cache.h"
#include "runtime/backend/host_isa.h"
#include "runtime/common/alignment.h"

namespace swift::runtime::backend {

CodeCache::CodeCache(const Config& config, u32 size, bool read_only)
        : config(config)
        , max_size(size)
        , inst_alignment(GetInstructionSetInstructionAlignment(config.backend_isa))
        , read_only(read_only) {
    Init();
}

CodeCache::~CodeCache() {
    if (space_code) {
        destroy_mspace(space_code);
    }
}

void CodeCache::Init() {
    code_mem = std::make_unique<MemMap>(max_size, true);
#if __APPLE__
    code_mem_mapped = reinterpret_cast<u8*>(code_mem->Map(code_mem->GetSize(), 0, MemMap::ReadExe));
#endif

    if (!read_only) {
        space_code = create_mspace_with_base(code_mem->GetMemory(), code_mem->GetSize(), 0);
    } else {
        code_cursor = code_mem->GetMemory();
    }
}

std::optional<CodeBuffer> CodeCache::AllocCode(size_t size) {
    u8* result{};
    if (read_only) {
        code_cursor = reinterpret_cast<u8*>(
                AlignUp(reinterpret_cast<uintptr_t>(code_cursor), inst_alignment));
        if (code_cursor + size <= (code_mem->GetMemory() + max_size)) {
            result = code_cursor;
        }
    } else {
        result = reinterpret_cast<u8*>(mspace_memalign(space_code, inst_alignment, size));
    }
    if (!result) {
        return std::nullopt;
    }

    CodeBuffer result_buffer{
            result, result, static_cast<u32>(result - code_mem->GetMemory()), size};
#if __APPLE__
    if (code_mem_mapped) {
        result_buffer.exec_data = result - code_mem->GetMemory() + code_mem_mapped;
    }
#endif
    return result_buffer;
}

bool CodeCache::FreeCode(u8* exec_ptr) {
    ASSERT(!read_only);
#if __APPLE__
    if (code_mem_mapped) {
        exec_ptr = code_mem->GetMemory() + (exec_ptr - code_mem_mapped);
    }
#endif
    mspace_free(space_code, exec_ptr);
    return true;
}

bool CodeCache::Contain(const u8* exec_ptr) {
#if __APPLE__
    if (code_mem_mapped) {
        return exec_ptr >= code_mem_mapped && exec_ptr <= (code_mem_mapped + max_size);
    } else
#endif
    {
        return exec_ptr >= code_mem->GetMemory() && exec_ptr <= (code_mem->GetMemory() + max_size);
    }
}

u8* CodeCache::GetExePtr(u32 offset) {
    if (offset > max_size) {
        return nullptr;
    }
#if __APPLE__
    if (code_mem_mapped) {
        return code_mem_mapped + offset;
    } else
#endif
    {
        return code_mem->GetMemory() + offset;
    }
}

u8* CodeCache::GetRWPtr(u32 offset) {
    if (offset > max_size) {
        return nullptr;
    }
    return code_mem->GetMemory() + offset;
}

u8* CodeCache::GetRWPtr(const u8* exec_ptr) {
#if __APPLE__
    if (code_mem_mapped) {
        if (exec_ptr < code_mem_mapped || exec_ptr > (code_mem_mapped + max_size)) {
            return nullptr;
        }
        return code_mem->GetMemory() + (exec_ptr - code_mem_mapped);
    } else
#endif
    {
        auto map_start = code_mem->GetMemory();
        if (exec_ptr < map_start || exec_ptr > (map_start + max_size)) {
            return nullptr;
        }
        return const_cast<u8*>(exec_ptr);
    }
}

}  // namespace swift::runtime::backend