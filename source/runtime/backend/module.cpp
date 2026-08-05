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

static void AddNodeRef(ir::AddressNode* node) { ir::AddressNodeAddRef(node); }

static void ReleaseNodeRef(ir::AddressNode* node) { ir::AddressNodeRelease(node); }

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

// Drops the module's reference to every node it still maps -- it does NOT
// delete them.
//
// A map entry is one strong reference, taken by Push/GetNodeOrCreate and
// dropped by Remove. Never calling this leaked every node still resident at
// teardown; deleting instead of releasing would be worse than the leak,
// because SmcTracker holds its own ir::NodeRef on exactly these nodes (that is
// what 8d8ddba changed it to, after a borrowed pointer there was a
// use-after-free), and callers can be holding IntrusivePtr handles from
// GetNode/GetNodes. Releasing is correct under every one of those: the node
// dies here only if this map held the last reference.
//
// Ordering note for the common teardown path: AddressSpace declares
// smc_tracker before default_module/modules, so ~AddressSpace destroys the
// tracker -- and with it the page records' node references -- before any
// ~Module runs. Modules unmapped earlier are held alive by the shared_ptr
// inside SmcTracker::TrackedNode until the tracker lets go. Neither ordering
// is relied on for safety here; the refcount is.
Module::~Module() {
    std::unique_lock guard(inner_lock);
    address_node_map.ReleaseAll(
            [](const ir::AddressNode* node) { return node->location.Value(); },
            &ReleaseNodeRef);
}

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
    // Identity-checked and idempotent. SMC invalidation can detach the same
    // node twice (a publisher re-registers it with the tracker between
    // TakeDirtyNodes and DetachNode, so the retry loop collects it again), and
    // the map may already hold a *different*, freshly created node at the same
    // location. Removing by location alone would then evict the new node and
    // release the old node's map reference a second time -- a refcount
    // underflow that frees a live node.
    bool erased = false;
    {
        std::unique_lock guard(inner_lock);
        if (address_node_map.Get(node->location.Value()) == node) {
            address_node_map.Remove(node->location.Value());
            erased = true;
        }
    }
    if (erased) {
        ReleaseNodeRef(node);
    }
}

// RemoveRange used to live here: remove-by-location plus an unconditional
// release, over a whole range. It had no callers and carried the exact
// identity bug Module::Remove documents above -- if the map had since been
// repopulated at one of those locations with a different node, it evicted the
// new node and released the old node's reference a second time. Deleted rather
// than fixed: SMC invalidation reaches the same effect through
// SmcTracker::TakeRangeNodes + DetachNode, which is identity-checked.

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

bool Module::IsDirectLinkConfigured() const {
    return address_space.GetConfig().backend_isa == kArm64 &&
           module_config.HasOpt(Optimizations::BlockLink);
}

bool Module::PrepareDirectLinkRegion() {
    if (!IsDirectLinkConfigured()) {
        return false;
    }
    std::unique_lock guard(cache_lock);
    for (auto& [index, cache] : code_caches) {
        (void)index;
        if (cache.GetRegionTrampoline()) {
            return true;
        }
    }

    constexpr u32 kMinCodeCacheSize = 32_MB;
    auto [it, inserted] = code_caches.try_emplace(
            current_code_cache, address_space.GetConfig(), kMinCodeCacheSize,
            ResolveFeatureSet(module_config));
    if (inserted) {
        ++current_code_cache;
    }
    const auto return_host = reinterpret_cast<void*>(
            address_space.GetTrampolines().GetReturnHost());
    if (!it->second.InitializeRegionTrampoline(
                address_space.GetLinkManager(), return_host, return_host)) {
        LOG_WARNING("direct link: region {} ({} bytes) cannot host an imm26-wide "
                    "trampoline; exits in this module use slot leaves",
                    it->second.GetRegion().id,
                    it->second.GetRegion().capacity);
        return false;
    }
    return true;
}

std::pair<u16, CodeBuffer> Module::AllocCodeCache(u32 size,
                                                  bool require_direct_link_region) {
    std::unique_lock guard(cache_lock);
    for (auto& [index, cache] : code_caches) {
        if (require_direct_link_region && !cache.GetRegionTrampoline()) {
            continue;
        }
        if (size > cache.GetRegion().capacity) {
            continue;
        }
        if (auto buf = cache.AllocCode(size); buf) {
            return {index, *buf};
        }
    }
    // NOTE: the cache must be big enough for dlmalloc's bookkeeping plus a
    // useful number of blocks. Sizing it from the *block* size (a few hundred
    // bytes) produced sub-minimum mspace arenas that handed out out-of-bounds
    // pointers and corrupted the JIT heap. Use a sane floor instead.
    constexpr u32 kMinCodeCacheSize = 32_MB;
    const u32 arena_size = std::max(ModuleCodeCacheSize(size), kMinCodeCacheSize);
    constexpr u32 kImm26Window = 1u << 27;
    if (require_direct_link_region && arena_size > kImm26Window) {
        LOG_WARNING("direct link: {}-byte unit requires a {}-byte arena; "
                    "no imm26-wide region can contain it",
                    size,
                    arena_size);
        return {INVALID_CACHE_ID, CodeBuffer{nullptr, nullptr, 0, 0}};
    }
    auto ref = code_caches.try_emplace(
            current_code_cache, address_space.GetConfig(), arena_size,
            ResolveFeatureSet(module_config));
    if (require_direct_link_region || IsDirectLinkConfigured()) {
        const auto return_host = reinterpret_cast<void*>(
                address_space.GetTrampolines().GetReturnHost());
        if (!ref.first->second.InitializeRegionTrampoline(
                    address_space.GetLinkManager(), return_host, return_host)) {
            LOG_WARNING("direct link: region {} ({} bytes) cannot host an imm26-wide "
                        "trampoline; this region remains unlinked",
                        ref.first->second.GetRegion().id,
                        ref.first->second.GetRegion().capacity);
            if (require_direct_link_region) {
                ++current_code_cache;
                return {INVALID_CACHE_ID, CodeBuffer{nullptr, nullptr, 0, 0}};
            }
        }
    }
    current_code_cache++;
    auto buffer = ref.first->second.AllocCode(size);
    if (!buffer) {
        return {INVALID_CACHE_ID, CodeBuffer{nullptr, nullptr, 0, 0}};
    }
    return {ref.first->first, *buffer};
}

std::optional<CodeRegion> Module::GetCodeRegion(const u8* exec_ptr) {
    std::shared_lock guard(cache_lock);
    for (auto& [index, cache] : code_caches) {
        (void)index;
        if (cache.Contain(exec_ptr)) {
            return cache.GetRegion();
        }
    }
    return std::nullopt;
}

std::optional<CodeRegion> Module::GetCodeRegion(CodeRegionId region_id) {
    std::shared_lock guard(cache_lock);
    for (auto& [index, cache] : code_caches) {
        (void)index;
        if (cache.GetRegion().id == region_id) {
            return cache.GetRegion();
        }
    }
    return std::nullopt;
}

u64 Module::PublishLinkTarget(ir::Location guest,
                              void* host_pc,
                              const void* allocation) {
    if (!IsDirectLinkConfigured()) {
        return 0;
    }
    const auto region = GetCodeRegion(static_cast<u8*>(host_pc));
    if (!region || !region->ContainsRx(host_pc) ||
        region->trampoline_offset == CodeRegion::kInvalidTrampolineOffset) {
        return 0;
    }
    return address_space.GetLinkManager().PublishTarget(
            guest.Value(), host_pc, region->id, {this, allocation});
}

void Module::DiscardLinkSource(const void* allocation) {
    auto& manager = address_space.GetLinkManager();
    const LinkSourceOwner owner{this, allocation};
    (void)manager.DetachSource(owner);
    (void)manager.PurgeSource(owner);
}

void Module::AddFaultEntry(u8* host_start,
                           u8* host_end,
                           VAddr guest_loc,
                           u8* owner_start,
                           u8* recovery) {
    ASSERT(host_start < host_end);
    std::unique_lock guard(cache_lock);
    FaultEntry entry{host_start,
                     host_end,
                     owner_start ? owner_start : host_start,
                     recovery,
                     guest_loc};
    auto it = std::lower_bound(fault_table.begin(),
                               fault_table.end(),
                               host_start,
                               [](const FaultEntry& e, const u8* pc) { return e.host_start < pc; });
    fault_table.insert(it, entry);
}

bool Module::LookupFault(const u8* host_pc, FaultEntry& out) {
    std::shared_lock guard(cache_lock);
    // Entries may overlap: a whole-unit committed-state fallback covers gaps
    // between precise per-block ranges. Prefer the most specific containing
    // range, and prefer a recovery veneer over the fallback for equal starts.
    auto it = std::upper_bound(fault_table.begin(),
                               fault_table.end(),
                               host_pc,
                               [](const u8* pc, const FaultEntry& e) { return pc < e.host_start; });
    const FaultEntry* best = nullptr;
    while (it != fault_table.begin()) {
        --it;
        if (!it->Contains(host_pc)) {
            continue;
        }
        if (!best || it->host_start > best->host_start ||
            (it->host_start == best->host_start && it->recovery && !best->recovery)) {
            best = &*it;
        }
        if (best && it->host_start < best->host_start) {
            break;
        }
    }
    if (!best) {
        return false;
    }
    out = *best;
    return true;
}

void Module::RemoveFaultEntries(const u8* host_start) {
    std::unique_lock guard(cache_lock);
    std::erase_if(fault_table,
                  [&](const FaultEntry& e) { return e.owner_start == host_start; });
}

static u8* DetachBlock(Module& module, ir::Block* block) {
    u8* exec_ptr = nullptr;
    {
        auto block_guard = block->LockWrite();
        auto& jit_cache = block->GetJitCache();
        if (jit_cache.jit_state.get<JitState>() == JitState::Cached) {
            exec_ptr = static_cast<u8*>(module.GetJitCache(jit_cache));
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
    // Last: removes the node from the address map and releases the module's
    // reference — `block` may be destroyed here.
    module.Remove(block);
    return exec_ptr;
}

static u8* DetachFunction(Module& module, ir::Function* function) {
    u8* exec_ptr = nullptr;
    {
        auto function_guard = function->LockWrite();
        auto& jit_cache = function->GetJitCache();
        if (jit_cache.jit_state.get<JitState>() == JitState::Cached) {
            exec_ptr = static_cast<u8*>(module.GetJitCache(jit_cache));
            jit_cache.jit_state = JitState::None;
            jit_cache.cache_id = 0;
            jit_cache.offset_in = 0;
            jit_cache.cache_size = 0;
        }
    }
    module.Remove(function);
    return exec_ptr;
}

u8* Module::DetachNode(ir::AddressNode* node) {
    ASSERT(node);
    auto module_guard = ModuleLockWrite();
    switch (node->node_type) {
        case ir::AddressNode::Block:
            return DetachBlock(*this, static_cast<ir::Block*>(node));
        case ir::AddressNode::Function:
            return DetachFunction(*this, static_cast<ir::Function*>(node));
        default:
            PANIC("invalid address node type");
    }
}

void Module::ReclaimCode(u8* exec_ptr) {
    if (!exec_ptr) {
        return;
    }
    std::unique_lock guard(cache_lock);
    std::erase_if(fault_table,
                  [&](const FaultEntry& entry) { return entry.owner_start == exec_ptr; });
    for (auto& [index, cache] : code_caches) {
        if (cache.Contain(exec_ptr)) {
            cache.FreeCode(exec_ptr);
            return;
        }
    }
}

}  // namespace swift::runtime::backend
