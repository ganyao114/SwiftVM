//
// Created by 甘尧 on 2024/3/8.
//

#pragma once

#include <map>
#include <shared_mutex>
#include "runtime/backend/code_cache.h"
#include "runtime/common/address_hash_map.h"
#include "runtime/common/range_mutex.h"
#include "runtime/common/types.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/function.h"

namespace swift::runtime::backend {

constexpr static auto INVALID_CACHE_ID = UINT16_MAX;
class AddressSpace;

struct ModuleConfig {
    bool read_only{};
    Optimizations optimizations{Optimizations::None};

    [[nodiscard]] bool HasOpt(Optimizations cmp) const { return True(optimizations & cmp); }
};

struct NoneAddressNode {};

// JIT fault table entry. Function units may contribute one subrange per
// emitted guest block; owner_start keeps all entries tied to the allocation
// that must be retired together. recovery is optional and defaults to the
// global committed-state fault return.
struct FaultEntry {
    u8* host_start{};
    u8* host_end{};
    u8* owner_start{};
    u8* recovery{};
    VAddr guest_loc{};

    [[nodiscard]] bool Contains(const u8* host_pc) const {
        return host_pc >= host_start && host_pc < host_end;
    }
};

using AddressNodeRef = boost::variant<NoneAddressNode, IntrusivePtr<ir::Block>, IntrusivePtr<ir::Function>>;

using AddressNodeRefs = StackVector<AddressNodeRef, 32>;

constexpr bool IsEmpty(const AddressNodeRef& node) { return node.empty() || node.which() == 0; }

constexpr bool IsBlock(const AddressNodeRef& node) { return !IsEmpty(node) && node.which() == 1; }

constexpr bool IsFunction(const AddressNodeRef& node) { return !IsEmpty(node) && node.which() == 2; }

inline IntrusivePtr<ir::Function> GetFunction(const AddressNodeRef& node) {
    return boost::get<IntrusivePtr<ir::Function>>(node);
}

inline IntrusivePtr<ir::Block> GetBlock(const AddressNodeRef& node) {
    return boost::get<IntrusivePtr<ir::Block>>(node);
}

class DataAllocator {
public:
    explicit DataAllocator(u32 size);

    ~DataAllocator();

    [[nodiscard]] void* Alloc(u32 size);

    void Free(void* ptr);

    [[nodiscard]] u32 GetSize() const { return mem_map->GetSize(); }

    [[nodiscard]] u8* GetBackend() const { return mem_map->GetMemory(); }

    [[nodiscard]] bool IsOverlap(const u8 *ptr);
private:
    std::unique_ptr<MemMap> mem_map;
    mspace space;
};

class Module : DeleteCopyAndMove {
public:
    explicit Module(AddressSpace& space,
                    const ir::Location& start,
                    const ir::Location& end,
                    const ModuleConfig& m_config);

    ~Module();

    bool Push(ir::AddressNode* block);

    void Remove(ir::AddressNode* block);

    [[nodiscard]] AddressNodeRef GetNode(ir::Location location);

    [[nodiscard]] AddressNodeRefs GetNodes(ir::Location start, ir::Location end);

    [[nodiscard]] AddressNodeRef GetNodeOrCreate(ir::Location location, bool function = false);

    [[nodiscard]] AddressNodeRefs GetRangeNodes(ir::Location start, ir::Location end);

    [[nodiscard]] CodeCache* GetCodeCache(u8* exe_ptr);

    [[nodiscard]] void* GetJitCache(ir::Location location);

    [[nodiscard]] u32 GetDispatchIndex(ir::Location location);

    [[nodiscard]] void* GetJitCache(const JitCache& jit_cache);

    [[nodiscard]] ScopedRangeLock LockAddress(ir::Location start, ir::Location end) {
        return ScopedRangeLock{address_lock, start.Value(), end.Value()};
    }

    [[nodiscard]] std::shared_lock<std::shared_mutex> ModuleLockRead() {
        return std::shared_lock{module_lock};
    }

    [[nodiscard]] std::unique_lock<std::shared_mutex> ModuleLockWrite() {
        return std::unique_lock{module_lock};
    }

    [[nodiscard]] const ModuleConfig& GetModuleConfig() const { return module_config; }

    // Ensures that at least one <=128MiB arena has its region trampoline
    // initialized before codegen chooses the 4-byte direct-link leaf.
    [[nodiscard]] bool PrepareDirectLinkV2Region();
    [[nodiscard]] std::pair<u16, CodeBuffer> AllocCodeCache(
            u32 size, bool require_direct_link_region = false);
    [[nodiscard]] std::optional<CodeRegion> GetCodeRegion(const u8* exec_ptr);
    [[nodiscard]] std::optional<CodeRegion> GetCodeRegion(CodeRegionId region_id);
    [[nodiscard]] u64 PublishLinkTarget(ir::Location guest,
                                        void* host_pc,
                                        const void* allocation);
    // A flushed allocation that never became module/L2-visible may be
    // discarded without an SMC/QSBR transaction, but its center-table owner
    // must still be removed before the bytes are freed.
    void DiscardLinkSource(const void* allocation);

    // Records the host PC range of a freshly compiled unit (called right
    // after JitContext::Flush in TranslateIR).
    void AddFaultEntry(u8* host_start,
                       u8* host_end,
                       VAddr guest_loc,
                       u8* owner_start = nullptr,
                       u8* recovery = nullptr);

    // Finds the fault entry whose host range contains host_pc. Called from
    // the host signal handler; takes the cache lock shared.
    [[nodiscard]] bool LookupFault(const u8* host_pc, FaultEntry& out);

    // SMC invalidation is split in two for MT safety. DetachNode resets the
    // JitCache and removes the address-map node, but deliberately keeps the
    // executable allocation and its fault-table entry alive. SmcTracker
    // reclaims that pair only after every runtime has passed a quiescent
    // state, so another host thread can finish stale code without UAF and a
    // fault in that code can still be recovered.
    [[nodiscard]] u8* DetachNode(ir::AddressNode* node);
    void ReclaimCode(u8* exec_ptr);

    // Drops every fault-table subrange owned by the allocation at host_start.
    void RemoveFaultEntries(const u8* host_start);

    [[nodiscard]] AddressSpace& GetAddressSpace() { return address_space; }

    [[nodiscard]] AddressSpace& GetAddressSpace() const { return address_space; }

private:
    const ModuleConfig module_config;
    AddressSpace& address_space;
    ir::Location module_start;
    ir::Location module_end;
    std::shared_mutex inner_lock;
    std::shared_mutex module_lock;
    RangeMutex address_lock{};
    AddressHashMap<&ir::AddressNode::map_node> address_node_map;
    std::shared_mutex cache_lock;
    std::map<u16, CodeCache> code_caches{};
    // Sorted by host_start (mspace allocations are not guaranteed monotonic).
    // Entries are retained while detached JIT code is epoch-protected and
    // removed together with the allocation by ReclaimCode.
    std::vector<FaultEntry> fault_table{};
    std::list<DataAllocator> data_allocators{};
    u16 current_code_cache{};
};

}  // namespace swift::runtime::backend
