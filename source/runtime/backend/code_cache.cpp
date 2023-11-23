//
// Created by 甘尧 on 2023/9/27.
//

#include "runtime/backend/code_cache.h"
#include "runtime/backend/host_isa.h"

namespace swift::runtime::backend {

CodeCache::CodeCache(const Config& config, u32 size)
        : config(config)
        , max_size(size)
        , inst_alignment(GetInstructionSetInstructionAlignment(config.backend_isa)) {
    Init();
}

CodeCache::~CodeCache() {
    if (space_code) {
        destroy_mspace(space_code);
    }
    if (space_data) {
        destroy_mspace(space_data);
    }
}

void CodeCache::Init() {
    code_mem = std::make_unique<MemMap>(max_size, true);

#if __APPLE__
    code_mem_mapped = reinterpret_cast<u8*>(code_mem->Map(code_mem->GetSize(), 0, MemMap::ReadExe));
#endif
    space_code = create_mspace_with_base(code_mem->GetMemory(), code_mem->GetSize(), 0);

    data_mem = std::make_unique<MemMap>(max_size, false);
    space_data = create_mspace_with_base(data_mem->GetMemory(), data_mem->GetSize(), 0);
}

std::optional<CodeBuffer> CodeCache::AllocCode(size_t size) {
    auto result = reinterpret_cast<u8*>(mspace_memalign(space_code, inst_alignment, size));
    if (!result) {
        return std::nullopt;
    }

    CodeBuffer result_buffer{result, result, size};
    if (code_mem_mapped) {
        result_buffer.exec_data = result - code_mem->GetMemory() + code_mem_mapped;
    }
    return std::move(result_buffer);
}

bool CodeCache::FreeCode(u8* exec_ptr) {
    if (code_mem_mapped) {
        exec_ptr = code_mem->GetMemory() + (exec_ptr - code_mem_mapped);
    }
    mspace_free(space_code, exec_ptr);
    return true;
}

bool CodeCache::Contain(u8* exec_ptr) { return false; }

}  // namespace swift::runtime::backend