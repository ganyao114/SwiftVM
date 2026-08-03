//
// Created by 甘尧 on 2023/9/8.
//

#include "address_space.h"
#include "runtime/backend/arm64/trampolines.h"
#include "runtime/backend/riscv64/trampolines.h"
#include "runtime/common/logging.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace swift::runtime::backend {

AddressSpace::AddressSpace(const Config& config)
        : config(config)
        , smc_tracker(reinterpret_cast<u64>(config.memory_base), config.guest_addr_mask) {
    Init();
}

void AddressSpace::Init() {
    const ModuleConfig default_module_config{.read_only = config.static_program,
                                             .optimizations = config.global_opts};
    default_module = std::make_shared<Module>(
            *this, config.loc_start, config.loc_end, default_module_config);

    // build trampolines
    switch (config.backend_isa) {
        case kArm64:
            trampolines = std::make_unique<arm64::TrampolinesArm64>(config);
            break;
        case kRiscv64:
            trampolines = std::make_unique<riscv64::TrampolinesRiscv64>(config);
            break;
        default:
            PANIC();
    }

    // build uniform info
    if (config.uniform_buffer_size) {
        uniform_info = std::make_unique<ir::UniformInfo>();
        uniform_info->uniform_size = config.uniform_buffer_size;
        for (const auto& range : config.xmm_uniform_ranges) {
            uniform_info->xmm_uniform_ranges.emplace_back(range.offset,
                                                           range.offset + range.size);
        }
        for (auto& desc : config.buffers_static_alloc) {
            auto type = desc.is_float ? ir::GetVecIRValueType(desc.size)
                                      : ir::GetIRValueType(desc.size);
            ir::UniformRegister reg{.uniform = ir::Uniform{desc.offset, type}};
            reg.host_reg.is_fpr = desc.is_float;
            if (desc.is_float) {
                reg.host_reg.fpr = ir::HostFPR{desc.reg};
                uniform_info->uni_fprs.Mark(desc.reg);
            } else {
                reg.host_reg.gpr = ir::HostGPR{desc.reg};
                uniform_info->uni_gprs.Mark(desc.reg);
            }
            uniform_info->uniform_regs_map.Map(desc.offset, desc.offset + desc.size, reg);
        }
    }

    // JIT disk cache (off unless SVM_JIT_CACHE names a directory). Loading
    // here is deliberate: the guest image is mapped before the AddressSpace is
    // built, so the per-unit guest-byte check has something to verify against,
    // and every revived unit is published before any guest code can run.
    if (JitDiskCache::Requested()) {
        jit_disk_cache = std::make_unique<JitDiskCache>(*this);
        jit_disk_cache->Load(default_module);
    }
}

std::shared_ptr<Module> AddressSpace::MapModule(LocationDescriptor start,
                                                LocationDescriptor end,
                                                const ModuleConfig& m_config) {
    if (jit_disk_cache && jit_disk_cache->Enabled()) {
        LOG_ERROR(
                "SVM_JIT_CACHE: MapModule({:#x}, {:#x}) creates a second module, but cache "
                "units have no module identity and are revived into default_module; cache "
                "ownership is ambiguous",
                start,
                end);
    }
    std::unique_lock guard(lock);
    auto module = std::make_shared<Module>(*this, start, end, m_config);
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

std::shared_ptr<Module> AddressSpace::GetDefaultModule() { return default_module; }

void AddressSpace::UnmapModule(LocationDescriptor start, LocationDescriptor end) {
    std::unique_lock guard(lock);
    modules.Unmap(start, end);
}

u32 AddressSpace::PushCodeCache(ir::Location location, void* cache) {
    return code_cache.Put(location.Value(), reinterpret_cast<size_t>(cache));
}

u32 AddressSpace::GetCodeCacheIndex(ir::Location location) {
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
    std::shared_lock guard(lock);
    bool found = false;
    modules.ForEachValue([&](const std::shared_ptr<Module>& module) {
        if (!found && module && module->LookupFault(host_pc, out)) {
            found = true;
        }
    });
    if (!found && default_module) {
        found = default_module->LookupFault(host_pc, out);
    }
    return found;
}

void AddressSpace::InvalidateCodeRange(VAddr guest_start, VAddr guest_end) {
    // Called by the syscall layer when the guest mprotects / remaps / unmaps
    // memory that may hold translated code. SmcTracker owns cross-runtime L1
    // and shared-L2 invalidation; there is no caller-specific L1 here.
    smc_tracker.InvalidateRange(*this, nullptr, guest_start, guest_end);
}

Trampolines& AddressSpace::GetTrampolines() { return *trampolines; }

Trampolines& AddressSpace::GetTrampolines() const { return *trampolines; }

const Config& AddressSpace::GetConfig() { return config; }

const Config& AddressSpace::GetConfig() const { return config; }

const ir::UniformInfo& AddressSpace::GetUniformInfo() { return *uniform_info; }

const ir::UniformInfo& AddressSpace::GetUniformInfo() const { return *uniform_info; }

AddressSpace::~AddressSpace() {
    const char* exec_prof = std::getenv("SVM_EXEC_PROF");
    if (exec_prof && std::strcmp(exec_prof, "0") != 0 && default_module &&
        default_module->IsDirectLinkConfigured()) {
        const auto stats = link_manager.GetStats();
        const auto kind = [](const auto& values, LinkSiteKind site_kind) {
            return values[static_cast<size_t>(site_kind)];
        };
        std::fprintf(stderr,
                     "[svm-direct-link] sites=%zu linked=%zu far=%zu retiring=%zu "
                     "registered=%llu linker_calls=%llu delinks=%llu max_in_degree=%zu "
                     "incoming_targets=%zu owners=%zu target_records=%zu bytes_est=%zu "
                     "signal_invalidations=%llu signal_targets_retained=%zu "
                     "signal_sites_retained=%zu epoch_sync=%llu "
                     "kinds=uncond:%zu/%zu/%zu,then:%zu/%zu/%zu,else:%zu/%zu/%zu,"
                     "switch:%zu/%zu/%zu,check_halt:%zu/%zu/%zu,backedge_cold:%zu/%zu/%zu\n",
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
                     kind(stats.far_by_kind, LinkSiteKind::BackedgeCold));
    }
    // Persist before anything is torn down: Save only touches the recorded
    // byte copies and the L2 slot assignment, both still intact here.
    if (jit_disk_cache) {
        jit_disk_cache->Save();
    }
}

}  // namespace swift::runtime::backend
