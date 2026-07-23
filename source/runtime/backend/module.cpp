//
// Created by 甘尧 on 2024/3/8.
//

#include "address_space.h"
#include "module.h"
#include "runtime/common/alignment.h"
#include <algorithm>

namespace swift::runtime::backend {

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>

int getpagesize() {
    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    return systemInfo.dwPageSize;
}
#endif

static u32 ModuleCodeCacheSize(size_t address_space_size) {
    if (address_space_size <= 256_MB) {
        return AlignUp(2 * address_space_size, getpagesize());
    } else {
        return 512_MB;
    }
}

static AddressNodeRef ToNodeRef(ir::AddressNode* node) {
    switch (node->node_type) {
        case ir::AddressNode::Function:
            return IntrusivePtr<ir::Function>((ir::Function*)node);
        case ir::AddressNode::Block:
            return IntrusivePtr<ir::Block>((ir::Block*)node);
        default:
            return {};
    }
}

static void AddNodeRef(ir::AddressNode* node) {
    switch (node->node_type) {
        case ir::AddressNode::Function:
            IntrusivePtrAddRef((ir::Function*)node);
            break;
        case ir::AddressNode::Block:
            IntrusivePtrAddRef((ir::Block*)node);
            break;
        default:
            break;
    }
}

static void ReleaseNodeRef(ir::AddressNode* node) {
    switch (node->node_type) {
        case ir::AddressNode::Function:
            IntrusivePtrRelease((ir::Function*)node);
            break;
        case ir::AddressNode::Block:
            IntrusivePtrRelease((ir::Block*)node);
            break;
        default:
            break;
    }
}

DataAllocator::DataAllocator(swift::u32 size) {
    mem_map = std::make_unique<MemMap>(size, true);
    space = create_mspace_with_base(mem_map->GetMemory(), mem_map->GetSize(), 0);
}

DataAllocator::~DataAllocator() {
    if (space) {
        destroy_mspace(space);
    }
}

void* DataAllocator::Alloc(u32 size) {
    return mspace_malloc(space, size);
}

void DataAllocator::Free(void* ptr) {
    mspace_free(space, ptr);
}

bool DataAllocator::IsOverlap(const u8* ptr) {
    auto mem = mem_map->GetMemory();
    auto size = mem_map->GetSize();
    return ptr >= mem && ptr < mem + size;
}

Module::Module(AddressSpace& space,
               const ir::Location& start,
               const ir::Location& end,
               const ModuleConfig& m_config)
        : address_space(space)
        , module_start(start)
        , module_end(end)
        , module_config(m_config)
        , address_node_map{start.Value(), end.Value()} {}

bool Module::Push(ir::AddressNode* node) {
    ASSERT(node);
    auto loc = node->GetStartLocation().Value();
    std::unique_lock guard(inner_lock);
    if (!address_node_map.Put<true>(loc, node)) {
        return false;
    }

    AddNodeRef(node);
    return true;
}

void Module::Remove(ir::AddressNode* node) {
    {
        std::unique_lock guard(inner_lock);
        address_node_map.Remove(node->location.Value());
    }
    ReleaseNodeRef(node);
}

AddressNodeRefs Module::RemoveRange(ir::Location start, ir::Location end) {
    std::unique_lock guard(inner_lock);
    auto nodes_ptr = address_node_map.GetRange(start.Value(), end.Value());
    AddressNodeRefs nodes{};
    for (auto node_ptr : nodes_ptr) {
        nodes.push_back(ToNodeRef(node_ptr));
        address_node_map.Remove(node_ptr->location.Value());
        ReleaseNodeRef(node_ptr);
    }
    return nodes;
}

AddressNodeRef Module::GetNode(ir::Location location) {
    std::shared_lock guard(inner_lock);
    if (auto node = address_node_map.Get(location.Value()); node) {
        return ToNodeRef(node);
    }
    return {};
}

AddressNodeRefs Module::GetNodes(ir::Location start, ir::Location end) {
    std::shared_lock guard(inner_lock);
    auto nodes_ptr = address_node_map.GetRange(start.Value(), end.Value());
    AddressNodeRefs nodes{};
    for (auto node_ptr : nodes_ptr) {
        nodes.push_back(ToNodeRef(node_ptr));
    }
    return nodes;
}

AddressNodeRef Module::GetNodeOrCreate(ir::Location location, bool is_func) {
    if (auto node = GetNode(location); !IsEmpty(node)) {
        return node;
    }
    std::unique_lock guard(inner_lock);
    if (auto node = address_node_map.Get(location.Value()); node) {
        return ToNodeRef(node);
    }
    if (is_func) {
        auto func = new ir::Function(location);
        IntrusivePtrAddRef(func);
        address_node_map.Put(location.Value(), func);
        return func;
    } else {
        auto block = new ir::Block(location);
        IntrusivePtrAddRef(block);
        address_node_map.Put(location.Value(), block);
        return block;
    }
}

AddressNodeRefs Module::GetRangeNodes(ir::Location start, ir::Location end) {
    std::shared_lock guard(inner_lock);
    auto nodes_ptr = address_node_map.GetRange(start.Value(), end.Value());
    AddressNodeRefs nodes{};
    for (auto node_ptr : nodes_ptr) {
        nodes.push_back(ToNodeRef(node_ptr));
    }
    return nodes;
}

CodeCache* Module::GetCodeCache(u8* exe_ptr) {
    std::shared_lock guard(cache_lock);
    for (auto& [index, cache] : code_caches) {
        if (cache.Contain(exe_ptr)) {
            return &cache;
        }
    }
    return nullptr;
}

void* Module::GetJitCache(ir::Location location) {
    auto node = GetNode(location);
    if (IsEmpty(node)) {
        return nullptr;
    }
    return VisitVariant<void*>(node, [this](auto x) -> auto {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, IntrusivePtr<ir::Function>>) {
            auto guard = x->LockRead();
            return GetJitCache(x->GetJitCache());
        } else if constexpr (std::is_same_v<T, IntrusivePtr<ir::Block>>) {
            auto guard = x->LockRead();
            return GetJitCache(x->GetJitCache());
        } else {
            return nullptr;
        }
    });
}

u32 Module::GetDispatchIndex(ir::Location location) {
    return address_space.GetCodeCacheIndex(location);
}

void* Module::GetJitCache(const JitCache& jit_cache) {
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

std::pair<u16, CodeBuffer> Module::AllocCodeCache(u32 size) {
    std::unique_lock guard(cache_lock);
    for (auto& [index, cache] : code_caches) {
        if (auto buf = cache.AllocCode(size); buf) {
            return {index, *buf};
        }
    }
    // NOTE: the cache must be big enough for dlmalloc's bookkeeping plus a
    // useful number of blocks. Sizing it from the *block* size (a few hundred
    // bytes) produced sub-minimum mspace arenas that handed out out-of-bounds
    // pointers and corrupted the JIT heap. Use a sane floor instead.
    constexpr u32 kMinCodeCacheSize = 32_MB;
    auto ref = code_caches.try_emplace(
            current_code_cache, address_space.GetConfig(),
            std::max(ModuleCodeCacheSize(size), kMinCodeCacheSize));
    current_code_cache++;
    return {ref.first->first, *ref.first->second.AllocCode(size)};
}

void Module::AddFaultEntry(u8* host_start, u8* host_end, VAddr guest_loc) {
    std::unique_lock guard(cache_lock);
    FaultEntry entry{host_start, host_end, guest_loc};
    auto it = std::lower_bound(fault_table.begin(),
                               fault_table.end(),
                               host_start,
                               [](const FaultEntry& e, const u8* pc) { return e.host_start < pc; });
    fault_table.insert(it, entry);
}

bool Module::LookupFault(const u8* host_pc, FaultEntry& out) {
    std::shared_lock guard(cache_lock);
    // Last entry with host_start <= host_pc, then check the range.
    auto it = std::upper_bound(fault_table.begin(),
                               fault_table.end(),
                               host_pc,
                               [](const u8* pc, const FaultEntry& e) { return pc < e.host_start; });
    if (it == fault_table.begin()) {
        return false;
    }
    --it;
    if (!it->Contains(host_pc)) {
        return false;
    }
    out = *it;
    return true;
}

void Module::RemoveFaultEntries(const u8* host_start) {
    std::unique_lock guard(cache_lock);
    std::erase_if(fault_table,
                  [&](const FaultEntry& e) { return e.host_start == host_start; });
}

void Module::InvalidateBlock(ir::Block* block) {
    u8* exec_ptr = nullptr;
    {
        auto block_guard = block->LockWrite();
        auto& jit_cache = block->GetJitCache();
        if (jit_cache.jit_state.get<JitState>() == JitState::Cached) {
            exec_ptr = static_cast<u8*>(GetJitCache(jit_cache));
            // Reset before Remove() below: a fresh block re-created at this
            // location must not see a stale Cached state (and a zeroed
            // JitCache is exactly what the frontend's "fresh node" workaround
            // expects).
            jit_cache.jit_state = JitState::None;
            jit_cache.cache_id = 0;
            jit_cache.offset_in = 0;
            jit_cache.cache_size = 0;
        }
    }
    if (exec_ptr) {
        RemoveFaultEntries(exec_ptr);
        if (auto* cache = GetCodeCache(exec_ptr); cache) {
            cache->FreeCode(exec_ptr);
        }
    }
    // Last: removes the node from the address map and releases the module's
    // reference — `block` may be destroyed here.
    Remove(block);
}

void Module::DestroyNodes() {
    address_node_map.Destroy();
}

}  // namespace swift::runtime::backend
