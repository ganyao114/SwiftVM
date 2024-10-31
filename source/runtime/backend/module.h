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

class Module : DeleteCopyAndMove {
public:
    explicit Module(AddressSpace& space,
                    const ir::Location& start,
                    const ir::Location& end,
                    const ModuleConfig& m_config);

    bool Push(ir::AddressNode* block);

    void Remove(ir::AddressNode* block);

    [[maybe_unused]] AddressNodeRefs RemoveRange(ir::Location start, ir::Location end);

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

    [[nodiscard]] const ModuleConfig& GetModuleConfig() const { return module_config; }

    [[nodiscard]] std::pair<u16, CodeBuffer> AllocCodeCache(u32 size);

    [[nodiscard]] AddressSpace& GetAddressSpace() { return address_space; }

    [[nodiscard]] AddressSpace& GetAddressSpace() const { return address_space; }

private:
    void DestroyNodes();

    const ModuleConfig module_config;
    AddressSpace& address_space;
    ir::Location module_start;
    ir::Location module_end;
    std::shared_mutex lock;
    RangeMutex address_lock{};
    AddressHashMap<&ir::AddressNode::map_node> address_node_map;
    std::shared_mutex cache_lock;
    std::map<u16, CodeCache> code_caches{};
    u16 current_code_cache{};
};

}  // namespace swift::runtime::backend
