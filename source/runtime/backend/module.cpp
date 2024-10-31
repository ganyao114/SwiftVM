//
// Created by 甘尧 on 2024/3/8.
//

#include "address_space.h"
#include "module.h"
#include "runtime/common/alignment.h"

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
    std::unique_lock guard(lock);
    if (!address_node_map.Put<true>(loc, node)) {
        return false;
    }

    AddNodeRef(node);
    return true;
}

void Module::Remove(ir::AddressNode* node) {
    {
        std::unique_lock guard(lock);
        address_node_map.Remove(node->location.Value());
    }
    ReleaseNodeRef(node);
}

AddressNodeRefs Module::RemoveRange(ir::Location start, ir::Location end) {
    std::unique_lock guard(lock);
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
    std::shared_lock guard(lock);
    if (auto node = address_node_map.Get(location.Value()); node) {
        return ToNodeRef(node);
    }
    return {};
}

AddressNodeRefs Module::GetNodes(ir::Location start, ir::Location end) {
    std::shared_lock guard(lock);
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
    std::unique_lock guard(lock);
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
    std::shared_lock guard(lock);
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
    auto ref = code_caches.try_emplace(
            current_code_cache, address_space.GetConfig(), ModuleCodeCacheSize(size));
    current_code_cache++;
    return {ref.first->first, *ref.first->second.AllocCode(size)};
}

void Module::DestroyNodes() {
    address_node_map.Destroy();
}

}  // namespace swift::runtime::backend
