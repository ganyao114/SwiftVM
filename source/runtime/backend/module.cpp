//
// Created by 甘尧 on 2024/3/8.
//

#include "module.h"
#include "address_space.h"
#include "runtime/common/alignment.h"

namespace swift::runtime::backend {

static u32 ModuleCodeCacheSize(size_t address_space_size) {
    if (address_space_size <= 256_MB) {
        return AlignUp(2 * address_space_size, getpagesize());
    } else {
        return 512_MB;
    }
}

Module::Module(const Config& config,
               AddressSpace& space,
               const ir::Location& start,
               const ir::Location& end,
               bool ro)
        : config(config), address_space(space), module_start(start), module_end(end), read_only(ro) {}

bool Module::Push(ir::Block* block) {
    ASSERT(block);
    std::unique_lock guard(lock);
    if (auto itr = ir_blocks.find(*block); itr != ir_blocks.end()) {
        return false;
    }
    IntrusivePtrAddRef(block);
    ir_blocks.insert(*block);
    return true;
}

bool Module::Push(ir::Function* func) {
    ASSERT(func);
    std::unique_lock guard(lock);
    if (auto itr = ir_functions.find(*func); itr != ir_functions.end()) {
        return false;
    }
    IntrusivePtrAddRef(func);
    ir_functions.insert(*func);
    return true;
}

IntrusivePtr<ir::Block> Module::GetBlock(ir::Location location) {
    std::shared_lock guard(lock);
    if (auto itr = ir_blocks.find(ir::Block{location}); itr != ir_blocks.end()) {
        return IntrusivePtr<ir::Block>{itr.operator->()};
    } else {
        return {};
    }
}

IntrusivePtr<ir::Function> Module::GetFunction(ir::Location location) {
    std::shared_lock guard(lock);
    if (auto itr = ir_functions.find(ir::Function{location}); itr != ir_functions.end()) {
        return IntrusivePtr<ir::Function>{itr.operator->()};
    } else {
        return {};
    }
}

CodeCache* Module::GetCodeCache(u8* exe_ptr) {
    std::shared_lock guard(cache_lock);
    for (auto &[index, cache] : code_caches) {
        if (cache.Contain(exe_ptr)) {
            return &cache;
        }
    }
    return nullptr;
}

void* Module::GetJitCache(ir::Location location) {
    if (auto block = GetBlock(location); block) {
        auto guard = block->LockRead();
        return GetJitCache(block->GetJitCache());
    } else if (auto func = GetFunction(location); func) {
        auto guard = func->LockRead();
        return GetJitCache(func->GetJitCache());
    } else {
        return nullptr;
    }
}

void* Module::GetJitCache(const JitCache &jit_cache) {
    if (jit_cache.jit_state.get<JitState>() != JitState::Cached) {
        return nullptr;
    }
    std::shared_lock guard(cache_lock);
    auto mem_map_id = *jit_cache.cache_id;
    auto mem_offset = *jit_cache.offset_in;
    if (auto mem_map = code_caches.find(mem_map_id); mem_map != code_caches.end()) {
        return mem_map->second.GetExePtr(mem_offset);
    } else {
        return nullptr;
    }
}

void Module::RemoveBlock(ir::Block* block) {
    {
        std::unique_lock guard(lock);
        ir_blocks.erase(*block);
    }
    IntrusivePtrRelease(block);
}

void Module::RemoveFunction(ir::Function* function) {
    {
        std::unique_lock guard(lock);
        ir_functions.erase(*function);
    }
    IntrusivePtrRelease(function);
}

std::pair<u16, CodeBuffer> Module::AllocCodeCache(u32 size) {
    std::unique_lock guard(cache_lock);
    for (auto &[index, cache] : code_caches) {
        if (auto buf = cache.AllocCode(size);buf) {
            return {index, *buf};
        }
    }
    auto ref = code_caches.try_emplace(current_code_cache, config, ModuleCodeCacheSize(size));
    current_code_cache++;
    return {ref.first->first, *ref.first->second.AllocCode(size)};
}

}
