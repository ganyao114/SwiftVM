//
// Created by 甘尧 on 2023/9/8.
//

#pragma once

#include "runtime/compile/analysis/guest_state.h"

#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <unordered_set>
#include "runtime/backend/jit_cache.h"
#include "runtime/backend/link_manager.h"
#include "runtime/backend/module.h"
#include "runtime/backend/retirement_control.h"
#include "runtime/backend/smc_tracker.h"
#include "runtime/backend/translate_table.h"
#include "runtime/backend/trampolines.h"
#include "runtime/common/range_map.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/function.h"

namespace swift::runtime::backend {

constexpr static auto code_cache_bits = 23;

class AddressSpace : public runtime::Instance {
public:
    explicit AddressSpace(const Config& config);

    ~AddressSpace() override;

    enum class ShutdownStage { Admission, Smc, Dispatch, LinkReaders, FaultReaders,
                               Modules, Trampolines, Complete };
    struct ShutdownResult {
        RetirementControl::DrainStatus admission{RetirementControl::DrainStatus::Stale};
        ShutdownStage stage{ShutdownStage::Admission};
        SmcTracker::ShutdownResult smc{};
        MemMap::ReleaseResult mapping{};
        int dispatch_error{};
        const Module* failed_module{};
        size_t remaining_modules{};
        [[nodiscard]] bool Complete() const {
            return admission == RetirementControl::DrainStatus::Ready && stage == ShutdownStage::Complete;
        }
    };
    // Host callers/raw entries must be quiescent. Pending and failed owners
    // stay attached to this live AddressSpace for an explicit retry.
    [[nodiscard]] ShutdownResult TryShutdown(const RetirementControl::Ticket& ticket);
    [[nodiscard]] bool RegisterTerminalAlias(const std::shared_ptr<TerminalAliasOwner>& owner);
    [[nodiscard]] size_t GetOwnedModuleCount();

    std::shared_ptr<Module> MapModule(LocationDescriptor start,
                                      LocationDescriptor end,
                                      const ModuleConfig &m_config);

    [[nodiscard]] std::shared_ptr<Module> GetModule(LocationDescriptor loc);

    [[nodiscard]] std::shared_ptr<Module> GetDefaultModule();

    // 返回显式 MapModule 建立的唯一 module 集合；default_module 不在其中。
    [[nodiscard]] std::vector<std::shared_ptr<Module>> GetMappedModules();

    void UnmapModule(LocationDescriptor start, LocationDescriptor end);

    u32 PushCodeCache(ir::Location location, void* cache);
    u32 PushCallCodeCache(ir::Location location, void* cache);
    u32 PushPendingCallCodeCache(ir::Location location, void* cache);

    u32 GetCodeCacheIndex(ir::Location location);

    [[nodiscard]] void* GetCodeCache(ir::Location location);

    // Signal-safe lookup includes detached Modules that retain live code.
    [[nodiscard]] bool LookupFault(const u8* host_pc, FaultEntry& out);

    [[nodiscard]] const std::shared_ptr<FaultCatalog>& GetFaultCatalog() const {
        return fault_catalog;
    }

    [[nodiscard]] const std::shared_ptr<RetirementControl>& GetRetirementControl() const {
        return retirement_control;
    }

    // Self-modifying code tracker: write-protects guest pages holding
    // translated code, claims the resulting write faults from the host
    // signal handler chain and drives invalidation of stale translations.
    [[nodiscard]] SmcTracker& GetSmcTracker() { return smc_tracker; }
    [[nodiscard]] SmcTracker& GetSmcTracker() const { return smc_tracker; }
    [[nodiscard]] LinkManager& GetLinkManager() { return link_manager; }
    [[nodiscard]] const LinkManager& GetLinkManager() const { return link_manager; }

    // Syscall-layer hook (Phase 4): the guest is changing permissions or
    // unmapping/remapping [guest_start, guest_end) — synchronously
    // invalidate every tracked block overlapping the range and drop the
    // write protection we installed. The syscall layer
    // (translator/linux/syscalls.cpp) calls this from its
    // mprotect/mmap/munmap emulation paths whenever the range may contain
    // translated guest code. SmcTracker clears the shared L2 and every
    // registered Runtime's L1 dispatch cache.
    void InvalidateCodeRange(VAddr guest_start, VAddr guest_end);

    // --- Adaptive RCpc alignment fallback ---------------------------------
    // Under rcpc_fault_emulate, scalar RCpc TSO accesses are emitted
    // without alignment dispatch and faulting accesses are emulated by
    // the runtime signal handler. A guest block whose emulated-fault
    // count reaches kRcpcFaultThreshold is marked checks-forced and its
    // translation invalidated, so the recompile emits the checked form
    // (the same policy shape as XTAJIT's fault-count fallback).
    // Counting is signal-safe and keyed by host PC (a fault-catalog
    // lookup per fault would dwarf the emulation it replaces); the
    // guest block is resolved once, at service time. Marking and
    // invalidation run at dispatcher boundaries through
    // ServiceRcpcAdaptive. InvalidateCodeRange clears marks for
    // rewritten guest code, giving it a fresh fault budget.
    static constexpr u32 kRcpcFaultThreshold = 128;
    // Returns true exactly when this fault crosses the threshold, so
    // the caller can request a dispatcher boundary (steady-state loops
    // may not reach one for a very long time on their own).
    [[nodiscard]] bool CountRcpcEmulatedFault(std::uintptr_t host_pc);
    void ServiceRcpcAdaptive(TranslateTable* current_l1);
    [[nodiscard]] bool RcpcChecksForced(VAddr guest_begin, VAddr guest_end);
    [[nodiscard]] u64 RcpcEmulatedFaultTotal() const {
        return rcpc_fault_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] size_t RcpcForcedSiteCount();

    // The address-space wide (L2) translate table backing PushCodeCache /
    // GetCodeCache; the JIT dispatcher reads it directly from generated code.
    [[nodiscard]] TranslateTable& GetCodeCacheTable() { return code_cache; }
    [[nodiscard]] TranslateTable& GetCallCodeCacheTable() {
        return call_code_cache;
    }
    [[nodiscard]] TranslateTable& GetPendingCallCodeCacheTable() {
        return pending_call_code_cache;
    }

    // JIT disk cache (SVM_JIT_CACHE=<dir>, off by default). Null when the
    // switch is unset or the environment cannot support it; every caller must
    // handle null by simply compiling as usual.
    [[nodiscard]] JitDiskCache* GetJitDiskCache() const { return jit_disk_cache.get(); }
    // Driver 在完成 module 映射后调用。重复调用由 JitDiskCache 自身去重。
    void LoadJitCache();

    [[nodiscard]] Trampolines &GetTrampolines();
    [[nodiscard]] Trampolines &GetTrampolines() const;

    [[nodiscard]] const Config &GetConfig();
    [[nodiscard]] const Config &GetConfig() const;
    [[nodiscard]] bool ExitLatchEnabled() const;

    // Offline compilation captures immutable recipes for process-local links
    // and region trampolines so installation can bind a fresh cache. Select
    // this before compiling any code; it does not change guest semantics.
    void SetSerializableCodeEmission() {
        serializable_code_emission.store(true, std::memory_order_relaxed);
    }
    [[nodiscard]] bool SerializableCodeEmission() const {
        return serializable_code_emission.load(std::memory_order_relaxed);
    }

    [[nodiscard]] const compile::GuestStateLayout &GetGuestStateLayout();
    [[nodiscard]] const compile::GuestStateLayout &GetGuestStateLayout() const;

private:
    void Init();

    const Config config;
    std::shared_ptr<RetirementControl> retirement_control{std::make_shared<RetirementControl>()};
    std::atomic<bool> serializable_code_emission{false};
    std::shared_ptr<FaultCatalog> fault_catalog{std::make_shared<FaultCatalog>()};
    std::shared_mutex lock;
    RangeMap<LocationDescriptor, std::shared_ptr<Module>> modules{};
    std::shared_ptr<Module> default_module;
    std::vector<std::shared_ptr<Module>> owned_modules;
    std::vector<std::shared_ptr<TerminalAliasOwner>> terminal_aliases;
    std::mutex shutdown_mutex;
    bool persistence_finished{};
    bool aliases_revoked{};
    bool modules_revoked{};
    bool shutdown_complete{};
    std::unique_ptr<Trampolines> trampolines;
    TranslateTable code_cache{code_cache_bits};
    TranslateTable call_code_cache{18, TranslateTableHash::Direct};
    TranslateTable pending_call_code_cache{18, TranslateTableHash::Direct};
    // mutable: SMC page records are tracking metadata, mutated through the
    // signal-handler / const-access paths (same pattern as GetTrampolines).
    mutable SmcTracker smc_tracker;
    LinkManager link_manager{};
    std::unique_ptr<compile::GuestStateLayout> guest_layout{};
    std::unique_ptr<JitDiskCache> jit_disk_cache{};

    // Adaptive RCpc state. The fault table is written from signal
    // context; everything else is guarded by rcpc_service_lock.
    struct RcpcFaultSlot {
        std::atomic<std::uintptr_t> key{~std::uintptr_t{0}};  // host PC
        std::atomic<u32> count{0};
    };
    static constexpr size_t kRcpcFaultSlots = 1024;
    std::array<RcpcFaultSlot, kRcpcFaultSlots> rcpc_fault_slots{};
    std::atomic<u64> rcpc_fault_total{0};
    std::atomic<bool> rcpc_service_pending{false};
    std::mutex rcpc_service_lock;
    std::unordered_set<VAddr> rcpc_forced_sites;
};

}  // namespace swift::runtime::backend
