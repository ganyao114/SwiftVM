
#include "runtime/compile/analysis/guest_state.h"
#include "base/logging.h"
//
// Created by 甘尧 on 2023/9/8.
//

#include "runtime/common/signal_diagnostic.h"
#include "address_space.h"
#include "runtime/backend/arm64/trampolines.h"
#include "runtime/backend/arm64/compile/target.h"
#include "runtime/backend/riscv64/trampolines.h"
#include "runtime/common/backedge_control.h"
#include "runtime/common/logging.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>

namespace swift::runtime::backend {

AddressSpace::AddressSpace(const Config& config)
        : config(config)
        , smc_tracker(reinterpret_cast<u64>(config.memory_base),
                      config.guest_addr_mask,
                      BackedgeLatchEnabled() || config.region_edges) {
    Init();
}

bool AddressSpace::ExitLatchEnabled() const {
    return BackedgeLatchEnabled() || config.region_edges;
}

void AddressSpace::Init() {
    const ModuleConfig default_module_config{.read_only = config.static_program,
                                             .optimizations = config.global_opts};
    default_module = std::make_shared<Module>(
            Module::ConstructionKey{}, *this, config.loc_start, config.loc_end, default_module_config);
    owned_modules.push_back(default_module);

    // build trampolines
    switch (config.backend_isa) {
        case kArm64:
            trampolines = std::make_unique<arm64::TrampolinesArm64>(
                    config, ResolveFeatureSet(default_module_config));
            break;
        case kRiscv64:
            trampolines = std::make_unique<riscv64::TrampolinesRiscv64>(config);
            break;
        default:
            PANIC();
    }

    // Keep empty metadata valid even when there is no uniform buffer.
    guest_layout = std::make_unique<compile::GuestStateLayout>();
    if (config.uniform_buffer_size) {
        guest_layout->uniform_size = config.uniform_buffer_size;
        for (const auto& range : config.xmm_uniform_ranges) {
            guest_layout->xmm_uniform_ranges.emplace_back(range.offset,
                                                           range.offset + range.size);
        }
        for (const auto& range : config.loop_gpr_uniform_ranges) {
            guest_layout->loop_gpr_uniform_ranges.emplace_back(
                    range.offset, range.offset + range.size);
        }
        for (auto& desc : config.buffers_static_alloc) {
            auto type = desc.is_float ? ir::GetVecIRValueType(desc.size)
                                      : ir::GetIRValueType(desc.size);
            compile::GuestRegisterBinding reg{.uniform = ir::Uniform{desc.offset, type}};
            reg.host_reg.is_fpr = desc.is_float;
            if (desc.is_float) {
                reg.host_reg.fpr = ir::HostFPR{desc.reg};
                guest_layout->uni_fprs.Mark(desc.reg);
            } else {
                reg.host_reg.gpr = ir::HostGPR{desc.reg};
                guest_layout->uni_gprs.Mark(desc.reg);
            }
            guest_layout->uniform_regs_map.Map(desc.offset, desc.offset + desc.size, reg);
        }
    }

    if (config.backend_isa == kArm64) {
        arm64::ConfigureGuestStatePolicy(*guest_layout);
    }

    // JIT disk cache (off unless SVM_JIT_CACHE names a directory). 这里只构造；
    // driver 必须先完成 MapModule，再显式 LoadJitCache，cache unit 才能按
    // guest 地址恢复到正确 module。
    if ((config.backend_isa == kArm64 || config.backend_isa == kRiscv64) &&
        JitDiskCache::Requested()) {
        jit_disk_cache = std::make_unique<JitDiskCache>(*this);
    }
    trampolines->CommitConstruction();
}

std::shared_ptr<Module> AddressSpace::MapModule(LocationDescriptor start,
                                                LocationDescriptor end,
                                                const ModuleConfig& m_config) {
    RetirementControl::Lease producer{retirement_control, RetirementControl::Kind::Producer};
    if (!producer) return nullptr;
    std::unique_lock guard(lock);
    auto module = std::make_shared<Module>(Module::ConstructionKey{}, *this, start, end, m_config);
    owned_modules.push_back(module);
    modules.Map(start, end, module);
    return module;
}

std::shared_ptr<Module> AddressSpace::GetModule(LocationDescriptor location) {
    std::shared_lock guard(lock);
    if (auto module = modules.GetValueAt(location); module) {
        return module;
    } else {
        return default_module;
    }
}

std::shared_ptr<Module> AddressSpace::GetDefaultModule() {
    std::shared_lock guard(lock);
    return default_module;
}

size_t AddressSpace::GetOwnedModuleCount() {
    std::shared_lock guard(lock);
    return owned_modules.size();
}

bool AddressSpace::RegisterTerminalAlias(const std::shared_ptr<TerminalAliasOwner>& owner) {
    RetirementControl::Lease producer{retirement_control, RetirementControl::Kind::Producer};
    if (!producer || !owner) return false;
    std::unique_lock guard(lock);
    terminal_aliases.push_back(owner);
    return true;
}

std::vector<std::shared_ptr<Module>> AddressSpace::GetMappedModules() {
    std::shared_lock guard(lock);
    std::vector<std::shared_ptr<Module>> result;
    modules.ForEachValue([&](const std::shared_ptr<Module>& module) {
        if (module && std::find(result.begin(), result.end(), module) == result.end()) {
            result.push_back(module);
        }
    });
    return result;
}

void AddressSpace::LoadJitCache() {
    if (jit_disk_cache) jit_disk_cache->Load();
}

void AddressSpace::UnmapModule(LocationDescriptor start, LocationDescriptor end) {
    std::unique_lock guard(lock);
    modules.Unmap(start, end);
}

u32 AddressSpace::PushCodeCache(ir::Location location, void* cache) {
    RetirementControl::Lease producer{retirement_control, RetirementControl::Kind::Producer};
    RetirementControl::PublicationScope publication{producer};
    if (!publication) return 0;
    return code_cache.Put(location.Value(), reinterpret_cast<size_t>(cache));
}

u32 AddressSpace::PushCallCodeCache(ir::Location location, void* cache) {
    RetirementControl::Lease producer{retirement_control, RetirementControl::Kind::Producer};
    RetirementControl::PublicationScope publication{producer};
    if (!publication) return 0;
    return call_code_cache.Put(location.Value(), reinterpret_cast<size_t>(cache));
}

u32 AddressSpace::PushPendingCallCodeCache(ir::Location location, void* cache) {
    RetirementControl::Lease producer{retirement_control, RetirementControl::Kind::Producer};
    RetirementControl::PublicationScope publication{producer};
    if (!publication) return 0;
    return pending_call_code_cache.Put(location.Value(),
                                       reinterpret_cast<size_t>(cache));
}

u32 AddressSpace::GetCodeCacheIndex(ir::Location location) {
    RetirementControl::Lease producer{retirement_control, RetirementControl::Kind::Producer};
    if (!producer) throw std::runtime_error("dispatch slot admission is closed");
    return code_cache.GetOrPut(location.Value(), 0);
}

void* AddressSpace::GetCodeCache(ir::Location location) {
    if (auto cache = code_cache.Lookup(location.Value()); cache) {
        return reinterpret_cast<void*>(cache);
    }
    auto module = GetModule(location.Value());
    if (!module) {
        return nullptr;
    }
    return module->GetJitCache(location);
}

bool AddressSpace::LookupFault(const u8* host_pc, FaultEntry& out) {
    return fault_catalog->Lookup(host_pc, out);
}

void AddressSpace::InvalidateCodeRange(VAddr guest_start, VAddr guest_end) {
    // Called by the syscall layer when the guest mprotects / remaps / unmaps
    // memory that may hold translated code. SmcTracker owns cross-runtime L1
    // and shared-L2 invalidation; there is no caller-specific L1 here.
    static std::atomic<uint32_t> inv_dbg{0};
    if (GetSvmConfig().inv_dbg && ClaimDiagnosticSample(inv_dbg, 60)) {
        SVM_DIAG_PRINT(Memory, "[inv-range] guest=[%#llx..%#llx]\n",
                (unsigned long long)guest_start, (unsigned long long)guest_end);
    }
    smc_tracker.InvalidateRange(*this, nullptr, guest_start, guest_end);
    // Rewritten guest code gets a fresh adaptive-RCpc fault budget: a
    // checks-forced mark earned by the old bytes must not tax the new
    // ones. (ServiceRcpcAdaptive invalidates through SmcTracker
    // directly, so its own marks survive this clearing.) Fault slots
    // are keyed by host PC and reset when serviced, so no guest-keyed
    // count survives to clear here.
    {
        std::lock_guard guard(rcpc_service_lock);
        std::erase_if(rcpc_forced_sites, [&](VAddr site) {
            return site >= guest_start && site < guest_end;
        });
    }
}

bool AddressSpace::CountRcpcEmulatedFault(std::uintptr_t host_pc) {
    rcpc_fault_total.fetch_add(1, std::memory_order_relaxed);
    const size_t home =
            std::hash<std::uintptr_t>{}(host_pc) & (kRcpcFaultSlots - 1);
    for (size_t probe = 0; probe < 16; ++probe) {
        auto& slot = rcpc_fault_slots[(home + probe) & (kRcpcFaultSlots - 1)];
        std::uintptr_t key = slot.key.load(std::memory_order_acquire);
        if (key != host_pc) {
            if (key != ~std::uintptr_t{0}) {
                continue;
            }
            std::uintptr_t expected = ~std::uintptr_t{0};
            if (!slot.key.compare_exchange_strong(
                        expected, host_pc, std::memory_order_acq_rel) &&
                expected != host_pc) {
                continue;
            }
        }
        const u32 count =
                slot.count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == kRcpcFaultThreshold) {
            rcpc_service_pending.store(true, std::memory_order_release);
            return true;
        }
        return false;
    }
    // Probe exhaustion only delays adaptation for this one fault; the
    // total above still records it.
    return false;
}

void AddressSpace::ServiceRcpcAdaptive(TranslateTable* current_l1) {
    if (!rcpc_service_pending.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    std::vector<VAddr> invalidate;
    {
        std::unique_lock guard(rcpc_service_lock, std::try_to_lock);
        if (!guard.owns_lock()) {
            rcpc_service_pending.store(true, std::memory_order_release);
            return;
        }
        for (auto& slot : rcpc_fault_slots) {
            const std::uintptr_t key = slot.key.load(std::memory_order_acquire);
            if (key == ~std::uintptr_t{0}) {
                continue;
            }
            if (slot.count.load(std::memory_order_relaxed) < kRcpcFaultThreshold) {
                continue;
            }
            // Resolve the guest block once, here in normal context, and
            // reset the slot: a racing unchecked recompile lands at a
            // new host PC and accumulates its own fresh budget, while a
            // recycled host address never inherits a stale count.
            slot.count.store(0, std::memory_order_relaxed);
            FaultEntry entry{};
            if (LookupFault(reinterpret_cast<const u8*>(key), entry)) {
                // Invalidate even when the mark already exists: a racing
                // unchecked recompile must be dropped again. Duplicate
                // ranges within one pass are harmless no-ops.
                rcpc_forced_sites.insert(entry.guest_loc);
                invalidate.push_back(entry.guest_loc);
            }
        }
    }
    // Invalidate outside rcpc_service_lock: the mark is already visible
    // to racing compiles, and SmcTracker takes its own metadata locks.
    for (const VAddr site : invalidate) {
        smc_tracker.InvalidateRange(*this, current_l1, site, site + 1);
    }
}

bool AddressSpace::RcpcChecksForced(VAddr guest_begin, VAddr guest_end) {
    // Degenerate ranges (hand-built IR nodes carry no byte size) act as
    // a point query on the block start.
    if (guest_end <= guest_begin) {
        guest_end = guest_begin + 1;
    }
    std::lock_guard guard(rcpc_service_lock);
    for (const VAddr site : rcpc_forced_sites) {
        if (site >= guest_begin && site < guest_end) {
            return true;
        }
    }
    return false;
}

size_t AddressSpace::RcpcForcedSiteCount() {
    std::lock_guard guard(rcpc_service_lock);
    return rcpc_forced_sites.size();
}

Trampolines& AddressSpace::GetTrampolines() { return *trampolines; }

Trampolines& AddressSpace::GetTrampolines() const { return *trampolines; }

const Config& AddressSpace::GetConfig() { return config; }

const Config& AddressSpace::GetConfig() const { return config; }

const compile::GuestStateLayout& AddressSpace::GetGuestStateLayout() { return *guest_layout; }

const compile::GuestStateLayout& AddressSpace::GetGuestStateLayout() const { return *guest_layout; }

AddressSpace::ShutdownResult AddressSpace::TryShutdown(const RetirementControl::Ticket& ticket) {
    if (ticket.owner != retirement_control) return {};
    const auto admission = retirement_control->CheckShutdown(ticket);
    if (admission != RetirementControl::DrainStatus::Ready) return {.admission = admission};
    std::lock_guard shutdown_guard(shutdown_mutex);
    if (shutdown_complete) return {.admission = admission, .stage = ShutdownStage::Complete};
    const auto preparation = smc_tracker.PrepareShutdown(*this, ticket);
    if (!preparation.Ready()) {
        return {.admission = admission, .stage = ShutdownStage::Smc,
                .smc = {.admission_ready = preparation.admission_ready,
                        .runtimes_attached = preparation.runtimes_attached}};
    }
    // Save may allocate and consult routing/slot assignments. Keep it before
    // irreversible revocation, outside the routing lock, and never repeat it.
    if (!persistence_finished) {
        if (jit_disk_cache) jit_disk_cache->SaveAfterDrain();
        persistence_finished = true;
    }
    const auto smc = smc_tracker.ShutdownAfterQuiescence(*this, ticket);
    if (!smc.Complete()) return {.admission = admission, .stage = ShutdownStage::Smc, .smc = smc};
    if (!aliases_revoked) {
        for (const auto& owner : terminal_aliases) owner->Revoke();
        terminal_aliases.clear();
        aliases_revoked = true;
    }
    for (auto* table : {&code_cache, &call_code_cache, &pending_call_code_cache}) {
        if (const auto error = table->ClearAfterQuiescence()) {
            return {.admission = admission, .stage = ShutdownStage::Dispatch, .dispatch_error = error};
        }
    }
    if (!link_manager.SealAndRevoke()) return {.admission = admission, .stage = ShutdownStage::LinkReaders};
    if (!fault_catalog->SealAndRevoke()) return {.admission = admission, .stage = ShutdownStage::FaultReaders};
    if (!modules_revoked) {
        for (const auto& module : owned_modules) module->RevokeAfterQuiescence();
        {
            std::unique_lock guard(lock);
            modules.Clear();
            default_module.reset();
        }
        modules_revoked = true;
    }
    for (const auto& module : owned_modules) {
        const auto mapping = module->ReleaseAfterQuiescence();
        if (!mapping.Complete()) return {.admission = admission, .stage = ShutdownStage::Modules,
                .mapping = mapping, .failed_module = module.get(), .remaining_modules = owned_modules.size()};
    }
    const auto mapping = trampolines->ReleaseAfterQuiescence();
    if (!mapping.Complete()) return {.admission = admission, .stage = ShutdownStage::Trampolines, .mapping = mapping};
    {
        std::unique_lock guard(lock);
        owned_modules.clear();
    }
    shutdown_complete = true;
    return {.admission = admission, .stage = ShutdownStage::Complete};
}

AddressSpace::~AddressSpace() {
    const auto shutdown = RetirementControl::RequestDrain(retirement_control, true);
    if (!retirement_control->SealPublications(shutdown) ||
        retirement_control->CheckShutdown(shutdown) != RetirementControl::DrainStatus::Ready)
        std::terminate();
    if (GetSvmConfig().exec_prof && default_module &&
        default_module->IsDirectLinkConfigured()) {
        const auto stats = link_manager.GetStats();
        const auto kind = [](const auto& values, LinkSiteKind site_kind) {
            return values[static_cast<size_t>(site_kind)];
        };
        SVM_DIAG_PRINT(Memory,
                     "[svm-direct-link] sites=%zu linked=%zu far=%zu retiring=%zu "
                     "registered=%llu linker_calls=%llu delinks=%llu max_in_degree=%zu "
                     "incoming_targets=%zu owners=%zu target_records=%zu bytes_est=%zu "
                     "signal_invalidations=%llu signal_targets_retained=%zu "
                     "signal_sites_retained=%zu epoch_sync=%llu "
                     "kinds=uncond:%zu/%zu/%zu,then:%zu/%zu/%zu,else:%zu/%zu/%zu,"
                     "switch:%zu/%zu/%zu,check_halt:%zu/%zu/%zu,backedge_cold:%zu/%zu/%zu,"
                     "call:%zu/%zu/%zu\n",
                     stats.sites,
                     stats.linked,
                     stats.far,
                     stats.retiring,
                     static_cast<unsigned long long>(stats.sites_registered),
                     static_cast<unsigned long long>(stats.linker_calls),
                     static_cast<unsigned long long>(stats.delinks),
                     stats.max_in_degree,
                     stats.incoming_targets,
                     stats.outgoing_owners,
                     stats.target_records,
                     stats.estimated_bytes,
                     static_cast<unsigned long long>(stats.signal_invalidations),
                     stats.signal_targets_retained,
                     stats.signal_sites_retained,
                     static_cast<unsigned long long>(smc_tracker.GetPatchSyncCount()),
                     kind(stats.sites_by_kind, LinkSiteKind::Unconditional),
                     kind(stats.linked_by_kind, LinkSiteKind::Unconditional),
                     kind(stats.far_by_kind, LinkSiteKind::Unconditional),
                     kind(stats.sites_by_kind, LinkSiteKind::ConditionalThen),
                     kind(stats.linked_by_kind, LinkSiteKind::ConditionalThen),
                     kind(stats.far_by_kind, LinkSiteKind::ConditionalThen),
                     kind(stats.sites_by_kind, LinkSiteKind::ConditionalElse),
                     kind(stats.linked_by_kind, LinkSiteKind::ConditionalElse),
                     kind(stats.far_by_kind, LinkSiteKind::ConditionalElse),
                     kind(stats.sites_by_kind, LinkSiteKind::SwitchArm),
                     kind(stats.linked_by_kind, LinkSiteKind::SwitchArm),
                     kind(stats.far_by_kind, LinkSiteKind::SwitchArm),
                     kind(stats.sites_by_kind, LinkSiteKind::CheckHalt),
                     kind(stats.linked_by_kind, LinkSiteKind::CheckHalt),
                     kind(stats.far_by_kind, LinkSiteKind::CheckHalt),
                     kind(stats.sites_by_kind, LinkSiteKind::BackedgeCold),
                     kind(stats.linked_by_kind, LinkSiteKind::BackedgeCold),
                     kind(stats.far_by_kind, LinkSiteKind::BackedgeCold),
                     kind(stats.sites_by_kind, LinkSiteKind::Call),
                     kind(stats.linked_by_kind, LinkSiteKind::Call),
                     kind(stats.far_by_kind, LinkSiteKind::Call));
    }
    if (!TryShutdown(shutdown).Complete()) std::terminate();
}

}  // namespace swift::runtime::backend
