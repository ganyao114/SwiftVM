//
// Created by 甘尧 on 2024/3/8.
//

#pragma once

#include <map>
#include <shared_mutex>
#include "runtime/backend/code_cache.h"
#include "runtime/backend/code_serial.h"
#include "runtime/backend/edge_flags_state.h"
#include "runtime/backend/fault_catalog.h"
#include "runtime/backend/retirement_control.h"
#include "runtime/backend/restricted_instruction_resume.h"
#include "runtime/common/address_hash_map.h"
#include "runtime/common/range_mutex.h"
#include "runtime/common/svm_config.h"
#include "runtime/common/types.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/function.h"

namespace swift::runtime::backend {

constexpr static auto INVALID_CACHE_ID = UINT16_MAX;
class AddressSpace;

struct ModuleConfig {
    bool read_only{};
    Optimizations optimizations{Optimizations::None};
    FeatureOverrides feature_overrides{};

    [[nodiscard]] bool HasOpt(Optimizations cmp) const { return True(optimizations & cmp); }
};

[[nodiscard]] inline FeatureSet ResolveFeatureSet(const ModuleConfig& config) {
    auto features = GetSvmConfig().GetFeatureSet();
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        const auto id = static_cast<FeatureId>(i);
        if (const auto value = config.feature_overrides.Get(id)) {
            features.Set(id, *value);
        }
    }
    return features;
}

struct NoneAddressNode {};

using AddressNodeRef = boost::variant<NoneAddressNode, IntrusivePtr<ir::Block>, IntrusivePtr<ir::Function>>;

using AddressNodeRefs = StackVector<AddressNodeRef, 32>;

inline bool IsEmpty(const AddressNodeRef& node) { return node.empty() || node.which() == 0; }

inline bool IsBlock(const AddressNodeRef& node) { return !IsEmpty(node) && node.which() == 1; }

inline bool IsFunction(const AddressNodeRef& node) { return !IsEmpty(node) && node.which() == 2; }

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

    // The owner must first revoke users and establish their quiescence.
    [[nodiscard]] MemMap::ReleaseResult ReleaseAfterQuiescence();

    [[nodiscard]] void* Alloc(u32 size);

    void Free(void* ptr);

    [[nodiscard]] u32 GetSize() const { return mem_map->GetSize(); }

    [[nodiscard]] u8* GetBackend() const {
        return release_started ? nullptr : mem_map->GetMemory();
    }

    [[nodiscard]] bool IsOverlap(const u8 *ptr);
private:
    std::unique_ptr<MemMap> mem_map;
    mspace space{};
    bool release_started{};
};

class Module : DeleteCopyAndMove {
    struct ConstructionKey {
        friend class AddressSpace;
    private:
        ConstructionKey() = default;
    };
public:
    explicit Module(ConstructionKey, AddressSpace& space,
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

    [[nodiscard]] bool IsDirectLinkConfigured() const;
    // Ensures that at least one <=128MiB arena has its region trampoline
    // initialized before codegen chooses the 4-byte direct-link leaf.
    [[nodiscard]] bool PrepareDirectLinkRegion();
    [[nodiscard]] std::pair<u16, CodeBuffer> AllocCodeCache(
            u32 size, bool require_direct_link_region = false);
    [[nodiscard]] std::optional<CodeRegion> GetCodeRegion(const u8* exec_ptr);
    [[nodiscard]] std::optional<CodeRegion> GetCodeRegion(CodeRegionId region_id);
    [[nodiscard]] u64 PublishLinkTarget(ir::Location guest,
                                        void* host_pc,
                                        const void* allocation,
                                        void* direct_host_pc = nullptr,
                                        void* pending_flags_host_pc = nullptr,
                                        void* call_host_pc = nullptr,
                                        void* call_pending_flags_host_pc = nullptr,
                                        EdgeFlagsTargetContract
                                                pending_flags_contract = {});
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
                       u8* recovery = nullptr,
                       FaultRecoveryKind recovery_kind =
                               FaultRecoveryKind::GuestFault);

    // Finds copied metadata through the signal-safe immutable catalog.
    [[nodiscard]] bool LookupFault(const u8* host_pc, FaultEntry& out);

    // Serialization needs the complete allocation, not the smaller recovery
    // range LookupFault prefers. Resolve an interior entry to its owner range.
    [[nodiscard]] bool LookupCodeAllocation(const u8* host_pc, FaultEntry& out,
                                            std::vector<FaultEntry>* fault_entries = nullptr);

    // SMC invalidation is split in two for MT safety. DetachNode resets the
    // JitCache and removes the address-map node, but deliberately keeps the
    // executable allocation and its fault-table entry alive. SmcTracker
    // reclaims that pair only after every runtime has passed a quiescent
    // state, so another host thread can finish stale code without UAF and a
    // fault in that code can still be recovered.
    [[nodiscard]] u8* DetachNode(ir::AddressNode* node);
    void ReclaimCode(u8* exec_ptr);
    void RecordSerialLinkSites(u8* allocation, std::vector<SerialLinkSite> sites);
    [[nodiscard]] std::vector<SerialLinkSite> QuerySerialLinkSites(const u8* allocation);
    // RV64 semantic helpers borrow canonical IR. Keep it alive through the
    // same QSBR grace period as the executable allocation, including detach.
    void RetainCodeIR(u8* allocation, ir::AddressNode* node);

    void RegisterInstructionResume(u8* allocation, std::shared_ptr<const InstructionResumeDescriptor> descriptor);
    [[nodiscard]] std::unique_ptr<InstructionCodePin> PinInstructionCode(const std::shared_ptr<Module>& owner, u8* allocation);
    [[nodiscard]] bool ValidateInstructionPin(const InstructionCodePin& pin);
    void ReleaseInstructionPin(u8* allocation, u64 allocation_id);
    struct InstructionPinState { u64 id{}, pins{}, ir_owners{}; bool revoked{}, reclaim_pending{}; };
    [[nodiscard]] InstructionPinState InspectInstructionPin(u8* allocation);

    // Drops every fault-table subrange owned by the allocation at host_start.
    void RemoveFaultEntries(const u8* host_start);

    [[nodiscard]] AddressSpace& GetAddressSpace() const;

    [[nodiscard]] const std::shared_ptr<RetirementControl>& GetRetirementControl() const {
        return retirement_control;
    }

private:
    friend class AddressSpace;
    void RevokeAfterQuiescence();
    [[nodiscard]] MemMap::ReleaseResult ReleaseAfterQuiescence();
    std::map<u8*, std::vector<ir::NodeRef>> code_ir_owners;
    std::unique_ptr<std::map<u8*, std::vector<SerialLinkSite>>> serial_link_sites;
    const ModuleConfig module_config;
    std::atomic<AddressSpace*> address_space;
    std::shared_ptr<RetirementControl> retirement_control;
    std::shared_ptr<FaultCatalog> fault_catalog;
    ir::Location module_start;
    ir::Location module_end;
    std::shared_mutex inner_lock;
    std::shared_mutex module_lock;
    RangeMutex address_lock{};
    AddressHashMap<&ir::AddressNode::map_node> address_node_map;
    std::shared_mutex cache_lock;
    std::map<u16, CodeCache> code_caches{};
    std::list<DataAllocator> data_allocators{};
    u16 current_code_cache{};
    bool publications_revoked{};
    struct InstructionAllocation {
        std::shared_ptr<const InstructionResumeDescriptor> descriptor;
        u64 id{}, pins{};
        bool revoked{}, reclaim_pending{};
    };
    std::map<u8*, InstructionAllocation> instruction_allocations;
    u64 next_instruction_allocation{1};
};

}  // namespace swift::runtime::backend
