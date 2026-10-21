//
// Created by 甘尧 on 2024/3/8.
//

#include "address_space.h"
#include "module.h"
#include "runtime/common/alignment.h"
#include <algorithm>
#include <stdexcept>
#include "base/scope_exit.h"

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
    auto rollback = ScopeExit([this] {
        if (space) destroy_mspace(space);
        (void)mem_map->ReleaseAfterQuiescence();
    });
    space = create_mspace_with_base(mem_map->GetMemory(), mem_map->GetSize(), 0);
    if (!space || !mspace_disable_growth(space)) throw std::bad_alloc{};
    rollback.Cancel();
}

DataAllocator::~DataAllocator() {
    if (space) {
        destroy_mspace(space);
    }
}

MemMap::ReleaseResult DataAllocator::ReleaseAfterQuiescence() {
    if (!release_started) {
        release_started = true;
        if (space) {
            destroy_mspace(space);
            space = nullptr;
        }
    }
    return mem_map->ReleaseAfterQuiescence();
}

void* DataAllocator::Alloc(u32 size) {
    if (release_started || size == 0) return nullptr;
    return mspace_malloc(space, size);
}

void DataAllocator::Free(void* ptr) {
    if (release_started || !ptr) return;
    mspace_free(space, ptr);
}

bool DataAllocator::IsOverlap(const u8* ptr) {
    if (release_started || !ptr) return false;
    auto mem = mem_map->GetMemory();
    auto size = mem_map->GetSize();
    return ptr >= mem && ptr < mem + size;
}

Module::Module(ConstructionKey, AddressSpace& space,
               const ir::Location& start,
               const ir::Location& end,
               const ModuleConfig& m_config)
        : address_space(&space)
        , retirement_control(space.GetRetirementControl())
        , fault_catalog(space.GetFaultCatalog())
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
    fault_catalog->RemoveModule(this);
    std::unique_lock guard(inner_lock);
    address_node_map.ReleaseAll(
            [](const ir::AddressNode* node) { return node->location.Value(); },
            &ReleaseNodeRef);
}

bool Module::Push(ir::AddressNode* node) {
    RetirementControl::Lease producer{retirement_control, RetirementControl::Kind::Producer};
    if (!producer) return false;
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
    // Direct-checked and idempotent. SMC invalidation can detach the same
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
// direct bug Module::Remove documents above -- if the map had since been
// repopulated at one of those locations with a different node, it evicted the
// new node and released the old node's reference a second time. Deleted rather
// than fixed: SMC invalidation reaches the same effect through
// SmcTracker::TakeRangeNodes + DetachNode, which is direct-checked.

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
    RetirementControl::Lease producer{retirement_control, RetirementControl::Kind::Producer};
    if (!producer) return NoneAddressNode{};
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
    if (!address_space.load(std::memory_order_seq_cst)) return nullptr;
    std::shared_lock guard(cache_lock);
    for (auto& [index, cache] : code_caches) {
        if (cache.Contain(exe_ptr)) {
            return &cache;
        }
    }
    return nullptr;
}

void* Module::GetJitCache(ir::Location location) {
    if (!address_space.load(std::memory_order_seq_cst)) return nullptr;
    auto node = GetNode(location);
    if (IsEmpty(node)) {
        return nullptr;
    }
    return VisitVariant<void*>(node, [this, location](auto x) -> void* {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, IntrusivePtr<ir::Function>>) {
            if (x->code_lifetime && !x->code_lifetime->Live()) return nullptr;
            auto guard = x->LockRead();
            if (GetAddressSpace().GetConfig().backend_isa == kRiscv64) {
                auto* block = x->FindBlock(location);
                return block ? GetJitCache(block->GetJitCache()) : nullptr;
            }
            return GetJitCache(x->GetJitCache());
        } else if constexpr (std::is_same_v<T, IntrusivePtr<ir::Block>>) {
            if (x->code_lifetime && !x->code_lifetime->Live()) return nullptr;
            auto guard = x->LockRead();
            return GetJitCache(x->GetJitCache());
        } else {
            return nullptr;
        }
    });
}

u32 Module::GetDispatchIndex(ir::Location location) {
    return GetAddressSpace().GetCodeCacheIndex(location);
}

void* Module::GetJitCache(const JitCache& jit_cache) {
    if (!address_space.load(std::memory_order_seq_cst)) return nullptr;
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
    const auto* owner = address_space.load(std::memory_order_seq_cst);
    if (!owner) return false;
    return (owner->GetConfig().backend_isa == kArm64 ||
            owner->GetConfig().backend_isa == kRiscv64) &&
           module_config.HasOpt(Optimizations::BlockLink);
}

bool Module::PrepareDirectLinkRegion() {
    RetirementControl::Lease producer{retirement_control, RetirementControl::Kind::Producer};
    if (!producer) return false;
    if (GetAddressSpace().GetConfig().backend_isa == kRiscv64) return IsDirectLinkConfigured();
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
            current_code_cache, GetAddressSpace().GetConfig(), kMinCodeCacheSize,
            ResolveFeatureSet(module_config));
    if (inserted) {
        ++current_code_cache;
    }
    const auto return_host = reinterpret_cast<void*>(
            GetAddressSpace().GetTrampolines().GetReturnHost());
    if (!it->second.InitializeRegionTrampoline(
                GetAddressSpace().GetLinkManager(), return_host, return_host, retirement_control)) {
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
    RetirementControl::Lease producer{retirement_control, RetirementControl::Kind::Producer};
    if (!producer) return {INVALID_CACHE_ID, CodeBuffer{nullptr, nullptr, 0, 0}};
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
            current_code_cache, GetAddressSpace().GetConfig(), arena_size,
            ResolveFeatureSet(module_config));
    if ((require_direct_link_region || IsDirectLinkConfigured()) &&
        GetAddressSpace().GetConfig().backend_isa == kArm64) {
        const auto return_host = reinterpret_cast<void*>(
                GetAddressSpace().GetTrampolines().GetReturnHost());
        if (!ref.first->second.InitializeRegionTrampoline(
                    GetAddressSpace().GetLinkManager(), return_host, return_host, retirement_control)) {
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
    if (!address_space.load(std::memory_order_seq_cst)) return std::nullopt;
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
    if (!address_space.load(std::memory_order_seq_cst)) return std::nullopt;
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
                              const void* allocation,
                              void* direct_host_pc,
                              void* pending_flags_host_pc,
                              void* call_host_pc,
                              void* call_pending_flags_host_pc,
                              EdgeFlagsTargetContract pending_flags_contract) {
    RetirementControl::Lease producer{retirement_control, RetirementControl::Kind::Producer};
    RetirementControl::PublicationScope publication{producer};
    if (!publication) return 0;
    if (!IsDirectLinkConfigured()) {
        return 0;
    }
    const auto region = GetCodeRegion(static_cast<u8*>(host_pc));
    if (!region || !region->ContainsRx(host_pc) ||
        (direct_host_pc && !region->ContainsRx(direct_host_pc)) ||
        (pending_flags_host_pc &&
         !region->ContainsRx(pending_flags_host_pc)) ||
        (call_host_pc && !region->ContainsRx(call_host_pc)) ||
        (call_pending_flags_host_pc &&
         !region->ContainsRx(call_pending_flags_host_pc)) ||
        (GetAddressSpace().GetConfig().backend_isa == kArm64 &&
         region->trampoline_offset == CodeRegion::kInvalidTrampolineOffset)) {
        return 0;
    }
    return GetAddressSpace().GetLinkManager().PublishTarget(
            guest.Value(),
            host_pc,
            region->id,
            {this, allocation},
            direct_host_pc,
            pending_flags_host_pc,
            call_host_pc,
            call_pending_flags_host_pc,
            pending_flags_contract);
}

void Module::DiscardLinkSource(const void* allocation) {
    auto* owner_space = address_space.load(std::memory_order_seq_cst);
    if (!owner_space) return;
    auto& manager = owner_space->GetLinkManager();
    const LinkSourceOwner owner{this, allocation};
    (void)manager.DetachSource(owner);
    (void)manager.PurgeSource(owner);
}

void Module::AddFaultEntry(u8* host_start,
                           u8* host_end,
                           VAddr guest_loc,
                           u8* owner_start,
                           u8* recovery,
                           FaultRecoveryKind recovery_kind) {
    RetirementControl::Lease producer{retirement_control, RetirementControl::Kind::Producer};
    RetirementControl::PublicationScope publication{producer};
    if (!publication) throw std::runtime_error("fault publication admission is closed");
    ASSERT(host_start < host_end);
    FaultEntry entry{host_start,
                     host_end,
                     owner_start ? owner_start : host_start,
                     recovery,
                     guest_loc,
                     recovery_kind};
    fault_catalog->Add(this, entry);
}

bool Module::LookupFault(const u8* host_pc, FaultEntry& out) {
    return fault_catalog->Lookup(host_pc, out, this);
}

bool Module::LookupCodeAllocation(const u8* host_pc, FaultEntry& out,
                                  std::vector<FaultEntry>* fault_entries) {
    return FaultCatalog::Reader{*fault_catalog}.LookupAllocation(this, host_pc, out, fault_entries);
}

void Module::RemoveFaultEntries(const u8* host_start) {
    fault_catalog->RemoveAllocation(this, host_start);
}

static u8* DetachBlock(Module& module, ir::Block* block) {
    u8* exec_ptr = nullptr;
    {
        auto block_guard = block->LockWrite();
        auto& jit_cache = block->GetJitCache();
        if (jit_cache.jit_state.get<JitState>() == JitState::Cached) {
            exec_ptr = static_cast<u8*>(module.GetJitCache(jit_cache));
            if (block->code_lifetime) exec_ptr = block->code_lifetime->base;
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
            if (function->code_lifetime) exec_ptr = function->code_lifetime->base;
            jit_cache.jit_state = JitState::None;
            jit_cache.cache_id = 0;
            jit_cache.offset_in = 0;
            jit_cache.cache_size = 0;
        }
        // RV publishes an entry descriptor on every child block. A retained
        // Function keeps those children alive after removal from the module;
        // none may keep resolving into the retired (and later reused) arena.
        for (auto& member : function->GetBlocks()) {
            auto& block = static_cast<ir::Block&>(member);
            auto block_guard = block.LockWrite();
            if (block.code_lifetime) block.code_lifetime->Revoke();
            block.GetJitCache() = {};
        }
    }
    module.Remove(function);
    return exec_ptr;
}

u8* Module::DetachNode(ir::AddressNode* node) {
    ASSERT(node);
    if (node->code_lifetime) node->code_lifetime->Revoke();
    auto module_guard = ModuleLockWrite();
    {
        std::unique_lock guard(cache_lock);
        for (auto& [allocation, entry] : instruction_allocations)
            if (entry.descriptor->original == node || entry.descriptor->emitted == node) entry.revoked = true;
    }
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
    // Destruction of the last IR owner takes the node lock. Drop it after the
    // cache lock to avoid inversion with readers that resolve a JitCache.
    std::vector<ir::NodeRef> owners;
    std::unique_lock guard(cache_lock);
    if (auto found = instruction_allocations.find(exec_ptr); found != instruction_allocations.end()) {
        found->second.revoked = found->second.reclaim_pending = true;
        if (found->second.pins) return;
        instruction_allocations.erase(found);
    }
    if (serial_link_sites) serial_link_sites->erase(exec_ptr);
    fault_catalog->RemoveAllocation(this, exec_ptr);
    if (auto found = code_ir_owners.find(exec_ptr); found != code_ir_owners.end()) {
        owners = std::move(found->second);
        code_ir_owners.erase(found);
    }
    for (auto& [index, cache] : code_caches) {
        if (cache.Contain(exec_ptr)) {
            cache.FreeCode(exec_ptr);
            return;
        }
    }
}

void Module::RecordSerialLinkSites(u8* allocation, std::vector<SerialLinkSite> sites) {
    if (!GetAddressSpace().SerializableCodeEmission()) throw std::runtime_error("serial link capture is not enabled");
    std::unique_lock guard(cache_lock);
    if (!serial_link_sites) serial_link_sites = std::make_unique<std::map<u8*, std::vector<SerialLinkSite>>>();
    serial_link_sites->insert_or_assign(allocation, std::move(sites));
}

std::vector<SerialLinkSite> Module::QuerySerialLinkSites(const u8* allocation) {
    std::shared_lock guard(cache_lock);
    if (serial_link_sites) {
        const auto found = serial_link_sites->find(const_cast<u8*>(allocation));
        if (found != serial_link_sites->end()) return found->second;
    }
    return {};
}

void Module::RetainCodeIR(u8* allocation, ir::AddressNode* node) {
    RetirementControl::Lease producer{retirement_control, RetirementControl::Kind::Producer};
    if (!producer) throw std::runtime_error("IR retention admission is closed");
    std::unique_lock guard(cache_lock);
    code_ir_owners[allocation].emplace_back(node);
}

void Module::RegisterInstructionResume(u8* allocation, std::shared_ptr<const InstructionResumeDescriptor> descriptor) {
    RetirementControl::Lease producer{retirement_control, RetirementControl::Kind::Producer};
    RetirementControl::PublicationScope publication{producer};
    if (!publication || !descriptor || !allocation) throw std::runtime_error("instruction descriptor publication rejected");
    std::unique_lock guard(cache_lock);
    if (!code_ir_owners.contains(allocation) || instruction_allocations.contains(allocation))
        throw std::runtime_error("instruction descriptor has no unique retained allocation");
    instruction_allocations.emplace(allocation, InstructionAllocation{std::move(descriptor), next_instruction_allocation++});
}

std::unique_ptr<InstructionCodePin> Module::PinInstructionCode(const std::shared_ptr<Module>& owner, u8* allocation) {
    if (owner.get() != this || !address_space.load(std::memory_order_seq_cst)) return {};
    {
        std::shared_lock guard(cache_lock);
        const auto found = instruction_allocations.find(allocation);
        if (found == instruction_allocations.end() || found->second.revoked || found->second.reclaim_pending) return {};
    }
    // Registration takes the control's state lock. It must happen before the
    // cache lock, matching publication's state -> cache lock order. Declare the
    // pin first so every failure destroys the cache guard before the pin.
    auto pin = std::unique_ptr<InstructionCodePin>(new InstructionCodePin);
    try { pin->terminal_owner_.emplace(retirement_control, pin->terminal_request_); }
    catch (const std::runtime_error&) { return {}; } // Terminal owner admission is sealed.
    std::unique_lock guard(cache_lock);
    auto found = instruction_allocations.find(allocation);
    if (!address_space.load(std::memory_order_seq_cst) || found == instruction_allocations.end() ||
        found->second.revoked || found->second.reclaim_pending) return {};
    pin->descriptor = found->second.descriptor;
    pin->allocation = allocation;
    pin->allocation_id = found->second.id;
    pin->ir_owners = code_ir_owners.at(allocation);
    ++found->second.pins;
    pin->module = owner;
    return pin;
}

bool Module::ValidateInstructionPin(const InstructionCodePin& pin) {
    std::shared_lock guard(cache_lock);
    const auto found = instruction_allocations.find(pin.allocation);
    if (!address_space.load(std::memory_order_seq_cst) || pin.module.get() != this ||
        found == instruction_allocations.end()) return false;
    const auto& entry = found->second;
    if (!entry.pins || entry.revoked || entry.reclaim_pending || entry.id != pin.allocation_id || entry.descriptor != pin.descriptor) return false;
    const auto& d = *entry.descriptor;
    if (!ValidateInstructionResumeHomes(d)) return false;
    bool original = false, emitted = false;
    for (const auto& owner : pin.ir_owners) { original |= owner.Get() == d.original; emitted |= owner.Get() == d.emitted; }
    FaultEntry allocation;
    return original && emitted && LookupCodeAllocation(pin.allocation, allocation) && allocation.owner_start == pin.allocation &&
           d.resume_offset < static_cast<u64>(allocation.host_end - allocation.host_start) &&
           d.head_offset < static_cast<u64>(allocation.host_end - allocation.host_start) &&
           InstructionIRIdentity(*d.emitted) == d.ir_identity && InstructionIRIdentity(*d.original) == d.original_identity;
}

void Module::ReleaseInstructionPin(u8* allocation, u64 allocation_id) {
    // A pending free comes only from ReclaimCode after its caller's QSBR and
    // signal-reader grace. A pin does not establish either kind of grace.
    bool reclaim{};
    {
        std::unique_lock guard(cache_lock);
        auto found = instruction_allocations.find(allocation);
        if (found == instruction_allocations.end() || found->second.id != allocation_id || !found->second.pins) std::terminate();
        reclaim = --found->second.pins == 0 && found->second.reclaim_pending;
    }
    if (reclaim) ReclaimCode(allocation);
}

Module::InstructionPinState Module::InspectInstructionPin(u8* allocation) {
    std::shared_lock guard(cache_lock);
    auto found = instruction_allocations.find(allocation);
    if (found == instruction_allocations.end()) return {};
    const auto& entry = found->second;
    return {entry.id, entry.pins, code_ir_owners.at(allocation).size(), entry.revoked, entry.reclaim_pending};
}

AddressSpace& Module::GetAddressSpace() const {
    auto* owner = address_space.load(std::memory_order_seq_cst);
    if (!owner) throw std::runtime_error("module is detached from its address space");
    return *owner;
}

static void ClearNodePublication(ir::AddressNode* node) {
    if (node->node_type == ir::AddressNode::Function) {
        auto* function = static_cast<ir::Function*>(node);
        auto guard = function->LockWrite();
        function->GetJitCache() = {};
        for (auto& member : function->GetBlocks()) {
            auto& block = static_cast<ir::Block&>(member);
            auto block_guard = block.LockWrite();
            block.GetJitCache() = {};
        }
    } else if (node->node_type == ir::AddressNode::Block) {
        auto* block = static_cast<ir::Block*>(node);
        auto guard = block->LockWrite();
        block->GetJitCache() = {};
    }
}

void Module::RevokeAfterQuiescence() {
    if (publications_revoked) return;
    address_space.store(nullptr, std::memory_order_seq_cst);
    auto module_guard = ModuleLockWrite();
    for (;;) {
        ir::AddressNode* node;
        {
            std::unique_lock guard(inner_lock);
            node = address_node_map.ExtractFirst();
        }
        if (!node) break;
        auto release = ScopeExit([node] { ReleaseNodeRef(node); });
        ClearNodePublication(node);
    }
    decltype(code_ir_owners) retained;
    {
        std::unique_lock guard(cache_lock);
        for (auto& [allocation, entry] : instruction_allocations) {
            if (entry.pins) std::terminate(); // Every pin is a terminal owner.
            entry.revoked = true;
        }
        instruction_allocations.clear();
        retained.swap(code_ir_owners);
    }
    for (const auto& [allocation, owners] : retained) {
        for (const auto& owner : owners) ClearNodePublication(owner.Get());
    }
    publications_revoked = true;
}

MemMap::ReleaseResult Module::ReleaseAfterQuiescence() {
    std::unique_lock guard(cache_lock);
    for (const auto& [allocation, entry] : instruction_allocations)
        if (entry.pins) std::terminate();
    for (auto it = code_caches.begin(); it != code_caches.end();) {
        const auto result = it->second.ReleaseAfterQuiescence();
        if (!result.Complete()) return result;
        it = code_caches.erase(it);
    }
    serial_link_sites.reset();
    for (auto it = data_allocators.begin(); it != data_allocators.end();) {
        const auto result = it->ReleaseAfterQuiescence();
        if (!result.Complete()) return result;
        it = data_allocators.erase(it);
    }
    return {};
}

}  // namespace swift::runtime::backend
