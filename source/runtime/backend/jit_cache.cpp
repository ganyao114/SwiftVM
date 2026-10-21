#include "base/logging.h"
//
// See jit_cache.h.
//

#include "runtime/backend/jit_cache.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

#include "runtime/backend/address_space.h"
#include "base/scope_exit.h"
#include "runtime/backend/code_lifetime.h"
#include "runtime/backend/function_entry_contract.h"
#include "runtime/backend/module.h"
#include "runtime/backend/riscv64/link.h"
#include "runtime/common/backedge_control.h"
#include "runtime/common/hot_coalesce_prof.h"
#include "runtime/common/logging.h"
#include "runtime/common/perf_stats.h"
#include "runtime/ir/function.h"

namespace swift::runtime::backend {

namespace {

constexpr char kMagic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
}  // namespace

bool JitDiskCache::Requested() {
    return !GetSvmConfig().jit_cache.empty();
}

JitDiskCache::JitDiskCache(AddressSpace& space)
        : address_space(space), host_image(GetHostImage()) {
    const auto& svm_config = GetSvmConfig();
    if (svm_config.jit_cache.empty()) {
        return;
    }
    print_stats = svm_config.jit_cache_stats;
    dir = svm_config.jit_cache;
    // Profiled JIT units embed a process-local counter slot. Serializing one
    // would revive code with no matching metadata slot in the next process.
    // The check is measurement-only, so disable disk caching while it is on.
    if (HotCounterStorageEnabled()) {
        LOG_WARNING("SVM_JIT_CACHE: counter-slot profiling is incompatible; cache disabled");
        return;
    }
    const auto& config = address_space.GetConfig();
    if (config.backend_isa == kArm64 && (BackedgeFlagsEnabled() ||
        (config.region_edges && svm_config.backedge_flags) ||
        svm_config.flags_loop_lazy)) {
        // Legacy backedge/loop-lazy recipes have their own eligibility state.
        // Format29 carries the flags-register ABI's entries, precise recovery
        // metadata and shared-region branches, but does not admit that older ABI.
        LOG_WARNING("SVM_JIT_CACHE: lazy flags ABI is incompatible; cache disabled");
        return;
    }
    if (host_image.size == 0) {
        LOG_WARNING("SVM_JIT_CACHE: host image span unknown; cache disabled");
        return;
    }
    if (!config.memory_base) {
        // Without a bias the cache cannot read guest bytes safely, and guest
        // addresses become plausible memory bases (see code_serial.h).
        LOG_WARNING("SVM_JIT_CACHE: direct-mapped guest memory is unsupported; cache disabled");
        return;
    }
    const u64 window = config.guest_addr_mask ? config.guest_addr_mask + 1 : 0;
    if (window != 0 && host_image.base < window) {
        LOG_WARNING(
                "SVM_JIT_CACHE: guest window ({:#x}) overlaps the host image ({:#x}); "
                "cache disabled",
                window,
                host_image.base);
        return;
    }
    if (config.static_program) {
        LOG_WARNING("SVM_JIT_CACHE: static modules are unsupported; cache disabled");
        return;
    }
    struct stat st {};
    if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        LOG_WARNING("SVM_JIT_CACHE: {} is not a directory; cache disabled", dir);
        return;
    }
    enabled = true;
}

JitDiskCache::~JitDiskCache() {
    if (print_stats) {
        SVM_DIAG_FORMAT(Runtime,
                   "[jit-cache] loaded={} compiled={} stored={} "
                   "feature(match={} skip={}) "
                   "reject(header={} guest={} reloc={} alloc={} scan={}) "
                   "dispatch_slots={}\n",
                   stats.units_loaded.load(),
                   stats.units_compiled.load(),
                   stats.units_stored.load(),
                   stats.feature_match.load(),
                   stats.reject_feature.load(),
                   stats.reject_header.load(),
                   stats.reject_guest_bytes.load(),
                   stats.reject_reloc.load(),
                   stats.reject_alloc.load(),
                   stats.reject_scan.load(),
                   stats.dispatch_slots.load());
    }
}

ValidityKey JitDiskCache::Key() const {
    std::vector<u64> mapped_feature_hashes;
    for (const auto& module : address_space.GetMappedModules()) {
        if (!module->GetModuleConfig().feature_overrides.Empty()) {
            mapped_feature_hashes.push_back(
                    HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig())));
        }
    }
    return ValidityKey{kCacheFormatVersion,
                       ComputeBuildId(),
                       ComputeConfigHash(
                               address_space.GetConfig(),
                               address_space.GetDefaultModule()->GetModuleConfig(),
                               mapped_feature_hashes),
                       ComputeEnvHash(),
                       ComputeGuestId()};
}

std::string JitDiskCache::FilePath() const {
    const auto key = Key();
    u64 name = HashU64(key.build_id, key.guest_id);
    name = HashU64(key.config_hash, name);
    name = HashU64(key.env_hash, name);
    return fmt::format("{}/svmjit-{:016x}.bin", dir, name);
}

bool JitDiskCache::HashGuestRange(VAddr start, VAddr end, u64& out) const {
    const auto& config = address_space.GetConfig();
    return HashGuestMemory(config.memory_base, config.guest_addr_mask, start, end, out);
}

// --------------------------------------------------------------------------
// Recording
// --------------------------------------------------------------------------
void JitDiskCache::RecordUnit(const std::shared_ptr<Module>& module,
                              VAddr guest_start,
                              bool is_function,
                              const u8* exec_data,
                              const u8* rw_data,
                              u32 code_size,
                              const std::vector<SerialBlock>& blocks,
                              const std::vector<SerialLinkSite>& link_sites,
                              const std::vector<SerialFaultSite>& fault_sites) {
    const auto& control = address_space.GetRetirementControl();
    RetirementControl::Lease producer{control, RetirementControl::Kind::Producer};
    if (!producer || !module || module->GetRetirementControl() != control) return;
    stats.units_compiled.fetch_add(1, std::memory_order_relaxed);
    if (!enabled || !exec_data || !rw_data || code_size == 0 || blocks.empty()) {
        return;
    }
    if (address_space.GetConfig().backend_isa == kRiscv64) {
        RecordRiscvUnit(module, guest_start, is_function, exec_data, rw_data, code_size,
                        blocks, link_sites, fault_sites);
        return;
    }
    // A module override can enable the older loop-lazy ABI after construction.
    const auto resolved_features = ResolveFeatureSet(module->GetModuleConfig());
    if (resolved_features.flags_loop_lazy) {
        return;
    }

    SerialUnit unit{};
    unit.guest_start = guest_start;
    unit.feature_hash = HashFeatureSet(resolved_features);
    unit.is_function = is_function ? 1 : 0;
    unit.link_sites = link_sites;
    unit.fault_sites = fault_sites;
    std::sort(unit.link_sites.begin(), unit.link_sites.end(),
              [](const auto& left, const auto& right) {
                  return left.code_offset < right.code_offset;
              });

    std::vector<u32> external_bl_offsets;
    external_bl_offsets.reserve(unit.link_sites.size());
    std::map<u32, u32> adjusted_words;
    if (!unit.link_sites.empty()) {
        // A serialized site list is also a defensive format check: only an
        // ARM64 BlockLink module may produce or revive these relocations.
        if (!module->IsDirectLinkConfigured()) {
            stats.reject_scan.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto region = module->GetCodeRegion(exec_data);
        if (!region || region->trampoline_offset == CodeRegion::kInvalidTrampolineOffset) {
            stats.reject_scan.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        u32 previous_offset{};
        bool first = true;
        for (const auto& site : unit.link_sites) {
            if ((site.code_offset & 3u) != 0 ||
                static_cast<size_t>(site.code_offset) + sizeof(u32) > code_size ||
                (!first && site.code_offset == previous_offset) ||
                site.kind >= static_cast<u8>(LinkSiteKind::Count)) {
                stats.reject_scan.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            auto* rx_site = exec_data + site.code_offset;
            if (!region->ContainsRx(rx_site)) {
                stats.reject_scan.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const bool has_flags_bypass = site.HasFlagsBypass();
            const bool has_flags_outline =
                    site.flags_merge_branch_offset != UINT32_MAX;
            if (!site.ValidFlagsBypass(code_size) ||
                (has_flags_bypass &&
                 region->pending_flags_trampoline_offset ==
                         CodeRegion::kInvalidTrampolineOffset)) {
                stats.reject_scan.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            auto* trampoline = region->rx_base +
                    (has_flags_bypass
                             ? region->pending_flags_trampoline_offset
                             : region->trampoline_offset);
            const auto branch = EncodeBL(trampoline - rx_site);
            if (!branch) {
                stats.reject_scan.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            external_bl_offsets.push_back(site.code_offset);
            if (!adjusted_words.emplace(site.code_offset, *branch).second) {
                stats.reject_scan.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (has_flags_bypass) {
                const auto [it, inserted] = adjusted_words.emplace(
                        site.flags_bypass_offset,
                        site.flags_bypass_instruction);
                if (!inserted && it->second != site.flags_bypass_instruction) {
                    stats.reject_scan.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (has_flags_outline) {
                    const auto [branch_it, branch_inserted] =
                            adjusted_words.emplace(
                                    site.flags_merge_branch_offset,
                                    0x1400'0000u);
                    if (!branch_inserted &&
                        branch_it->second != 0x1400'0000u) {
                        stats.reject_scan.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                }
            }
            previous_offset = site.code_offset;
            first = false;
        }
    }

    // Copy immutable spans only. A live linker or invalidator may atomically
    // patch a site at this exact moment; never read that word through memcpy.
    // Each skipped word is synthesized as the unlinked BL form, so the stored
    // capture is independent of Linked/Far/Unlinked runtime state.
    unit.code.resize(code_size);
    size_t cursor{};
    for (const auto& [word_offset, instruction] : adjusted_words) {
        const size_t offset = word_offset;
        if (offset > cursor) {
            std::memcpy(unit.code.data() + cursor, rw_data + cursor, offset - cursor);
        }
        std::memcpy(unit.code.data() + offset, &instruction, sizeof(u32));
        cursor = offset + sizeof(u32);
    }
    if (cursor < code_size) {
        std::memcpy(unit.code.data() + cursor, rw_data + cursor, code_size - cursor);
    }
    auto* owning_cache = module->GetCodeCache(const_cast<u8*>(exec_data));
    if (!owning_cache || !FreezeRegionBranches(*owning_cache, exec_data, unit.code, unit.region_branches)) {
        stats.reject_scan.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const u64 window =
            address_space.GetConfig().guest_addr_mask ? address_space.GetConfig().guest_addr_mask + 1
                                                      : 0;
    auto scan = ScanCodeUnit(unit.code, host_image, window, external_bl_offsets);
    if (!scan.ok) {
        stats.reject_scan.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    unit.relocs = std::move(scan.relocs);
    unit.blocks.reserve(blocks.size());
    for (auto block : blocks) {
        if (!HashGuestRange(block.guest_start, block.guest_end, block.guest_bytes_hash)) {
            stats.reject_scan.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        unit.blocks.push_back(block);
    }

    std::lock_guard guard(lock);
    units[guest_start] = std::move(unit);
    dirty = true;
}

// --------------------------------------------------------------------------
// Saving
// --------------------------------------------------------------------------
void JitDiskCache::Save() {
    RetirementControl::Lease producer{address_space.GetRetirementControl(), RetirementControl::Kind::Producer};
    if (!producer) return;
    SaveImpl();
}

void JitDiskCache::SaveAfterDrain() {
    // Only AddressSpace teardown calls this after terminal admission closure.
    SaveImpl();
}

void JitDiskCache::SaveImpl() {
    if (!enabled) {
        return;
    }
    std::lock_guard guard(lock);
    // A fully warm run adds nothing: rewriting an identical file would charge
    // every warm run for serializing the whole cache, which is exactly the
    // cost this feature exists to remove. `dirty` is set by RecordUnit and by
    // any per-unit rejection during Load (so a file holding stale entries is
    // rewritten without them).
    if (units.empty() || !dirty) {
        return;
    }
    // Capture the L2 dispatch-table assignment. The indices baked into the
    // cached code are table slots, and a slot's direct depends on the
    // insertion order of colliding keys -- replaying the assignment verbatim
    // is what keeps those immediates valid without patching them.
    std::vector<std::pair<u64, u32>> slots;
    address_space.GetCodeCacheTable().ForEachEntry(
            [&](u32 index, size_t key, size_t) { slots.emplace_back(key, index); });

    BlobWriter payload;
    payload.U64(slots.size());
    for (const auto& [key, index] : slots) {
        payload.U64(key);
        payload.U32(index);
    }
    payload.U64(units.size());
    for (const auto& [start, unit] : units) {
        WriteUnit(payload, unit);
    }

    const auto key = Key();
    BlobWriter header;
    header.Bytes(kMagic, sizeof(kMagic));
    header.U64(key.format_version);
    header.U64(key.build_id);
    header.U64(key.config_hash);
    header.U64(key.env_hash);
    header.U64(key.guest_id);
    header.U64(payload.Size());
    header.U64(HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull));

    const auto path = FilePath();
    const auto tmp = fmt::format("{}.tmp{}", path, static_cast<int>(::getpid()));
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        LOG_WARNING("SVM_JIT_CACHE: cannot write {}: {}", tmp, std::strerror(errno));
        return;
    }
    const bool ok = std::fwrite(header.Data().data(), 1, header.Size(), f) == header.Size() &&
                    std::fwrite(payload.Data().data(), 1, payload.Size(), f) == payload.Size();
    std::fclose(f);
    if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        LOG_WARNING("SVM_JIT_CACHE: failed to publish {}", path);
        return;
    }
    stats.units_stored.store(units.size(), std::memory_order_relaxed);
}

// --------------------------------------------------------------------------
// Loading
// --------------------------------------------------------------------------
bool JitDiskCache::ReviveUnit(const std::shared_ptr<Module>& module, const SerialUnit& unit) {
    RetirementControl::Lease producer{module->GetRetirementControl(), RetirementControl::Kind::Producer};
    if (!producer) return false;
    PerfScope2 perf_revive{GetPerfStats2().cache_revive};
    SmcTracker::PublicationScope installation{address_space.GetSmcTracker()};
    ir::NodeRef node_owner;
    bool published{}, committed{};
    const auto retire_failed = ScopeExit([&] {
        if (!published || committed) return;
        try { (void)installation.RetireNode(address_space, module, node_owner.Get()); }
        catch (...) {} // The Module retains inert storage for retry or shutdown.
    });
    const auto reject = [&] {
        stats.reject_reloc.fetch_add(1, std::memory_order_relaxed);
        dirty = true;
        return false;
    };
    const auto entry_owned = [&](VAddr location) {
        return address_space.GetCodeCache(ir::Location{location}) ||
               address_space.GetCallCodeCacheTable().Lookup(location) ||
               address_space.GetPendingCallCodeCacheTable().Lookup(location) ||
               !IsEmpty(address_space.GetModule(location)->GetNode(ir::Location{location}));
    };
    try {
        const bool rv = address_space.GetConfig().backend_isa == kRiscv64;
        if (unit.blocks.empty() || unit.is_function > 1 || unit.code.empty() ||
            unit.code.size() > UINT32_MAX || (unit.code.size() & 3u)) return reject();
        const SerialBlock* root{};
        std::vector<VAddr> entries, locations;
        std::vector<const SerialBlock*> emitted;
        for (const auto& block : unit.blocks) {
            if (!block.ValidEntryContract(unit.code.size())) return reject();
            if (!block.IsDependency()) {
                if (std::find(locations.begin(), locations.end(), block.guest_start) != locations.end())
                    return reject();
                locations.push_back(block.guest_start);
            }
            u64 hash{};
            if (!HashGuestRange(block.guest_start, block.guest_end, hash) ||
                hash != block.guest_bytes_hash) {
                stats.reject_guest_bytes.fetch_add(1, std::memory_order_relaxed);
                dirty = true;
                return false;
            }
            if (block.IsPublished()) {
                if (entry_owned(block.guest_start)) return reject();
                entries.push_back(block.guest_start);
                emitted.push_back(&block);
                if (block.guest_start == unit.guest_start) root = &block;
            }
        }
        if (!root || (rv && root->code_offset != 0) ||
            (!unit.is_function && (locations.size() != 1 || unit.blocks.front().guest_start != unit.guest_start)) ||
            ((!unit.link_sites.empty() || !unit.region_branches.empty()) && !module->IsDirectLinkConfigured()) ||
            !ValidRegionBranches(unit.region_branches, unit.code.size()) ||
            (rv && !unit.region_branches.empty())) return reject();
        for (const auto& site : unit.fault_sites) {
            if ((site.host_begin & 3u) || (site.host_end & 3u) ||
                site.host_begin >= site.host_end || site.host_end > unit.code.size() ||
                (!rv && site.host_end != site.host_begin + sizeof(u32)) ||
                (site.recovery_offset != UINT32_MAX &&
                 ((site.recovery_offset & 3u) || site.recovery_offset >= unit.code.size())) ||
                site.recovery_kind > static_cast<u8>(FaultRecoveryKind::ExternalContinuation))
                return reject();
        }
        std::sort(emitted.begin(), emitted.end(), [](const auto* a, const auto* b) {
            return a->code_offset < b->code_offset;
        });
        if (!rv && address_space.ExitLatchEnabled()) {
            for (size_t i = 0; i + 1 < emitted.size(); ++i)
                if (emitted[i]->code_offset == emitted[i + 1]->code_offset) return reject();
        }

        // Copy persistence ownership before publication. The final map insertion
        // remains inside the transaction, including a possible rehash failure.
        SerialUnit saved_unit = unit;
        auto module_guard = module->ModuleLockWrite();
        auto [idx, buffer] = module->AllocCodeCache(static_cast<u32>(unit.code.size()),
                !rv && (!unit.link_sites.empty() || !unit.region_branches.empty()));
        if (idx == INVALID_CACHE_ID) {
            stats.reject_alloc.fetch_add(1, std::memory_order_relaxed);
            dirty = true;
            return false;
        }
        std::shared_ptr<CodeLifetime> lifetime;
        const auto discard = ScopeExit([&] {
            if (published) return;
            if (lifetime) lifetime->Revoke();
            try {
                module->DiscardLinkSource(buffer.exec_data);
                module->ReclaimCode(buffer.exec_data);
            } catch (...) {} // Arena ownership survives metadata cleanup failure.
        });
        std::memcpy(buffer.rw_data, unit.code.data(), unit.code.size());
        std::string error;
        if (!ApplyRelocations(buffer.rw_data, unit.code.size(), unit.relocs, host_image,
                              &error, address_space.GetConfig().backend_isa)) return reject();
        if (rv) {
            if (!riscv64::RegisterLinkSites(module, buffer, unit.link_sites)) return reject();
        } else {
            // Disk capture freezes an allocation-relative cold BL, whereas the
            // shared installer consumes the address-independent BL placeholder.
            // Normalize only validated source words before rebinding them.
            auto arm_links = unit.link_sites;
            for (auto& site : arm_links) {
                if ((site.code_offset & 3u) || size_t(site.code_offset) + sizeof(u32) > unit.code.size())
                    return reject();
                if (site.unlinked_instruction != 0 && site.unlinked_instruction != 0x94000000u)
                    return reject();
                site.unlinked_instruction = 0x94000000u;
                u32 instruction{};
                std::memcpy(&instruction, buffer.rw_data + site.code_offset, sizeof(instruction));
                if ((instruction & 0xfc000000u) != 0x94000000u) return reject();
                instruction = 0x94000000u;
                std::memcpy(buffer.rw_data + site.code_offset, &instruction, sizeof(instruction));
            }
            auto* cache = module->GetCodeCache(buffer.exec_data);
            if (!cache || !RebindRegionBranches(*cache, buffer.exec_data,
                    {buffer.rw_data, buffer.size}, unit.region_branches) ||
                !RegisterArmSerialLinks(*module, buffer, arm_links)) return reject();
        }
        buffer.Flush();
        lifetime = std::make_shared<CodeLifetime>(buffer.exec_data, buffer.size, std::move(entries));
        ir::AddressNode* node{};
        JitCache* cache{};
        if (unit.is_function) {
            auto* function = new ir::Function(ir::Location{unit.guest_start});
            node_owner = ir::NodeRef{function};
            VAddr max_end = unit.guest_start;
            for (const auto& block : unit.blocks) {
                if (block.IsDependency()) continue;
                auto* child = new ir::Block(ir::Location{block.guest_start});
                child->SetEndLocation(ir::Location{block.guest_end});
                child->code_lifetime = lifetime;
                if (rv && block.IsPublished()) {
                    auto& child_cache = child->GetJitCache();
                    child_cache.jit_state = JitState::Cached;
                    child_cache.cache_id = idx;
                    child_cache.offset_in = buffer.offset + block.code_offset;
                    child_cache.cache_size = buffer.size - block.code_offset;
                }
                function->AddBlock(child);
                max_end = std::max(max_end, block.guest_end);
            }
            function->SetEndLocation(ir::Location{max_end});
            node = function; cache = &function->GetJitCache();
        } else {
            auto* block = new ir::Block(ir::Location{unit.guest_start});
            node_owner = ir::NodeRef{block};
            block->SetEndLocation(ir::Location{root->guest_end});
            node = block; cache = &block->GetJitCache();
        }
        cache->jit_state = JitState::Cached;
        cache->cache_id = idx;
        // The node cache identifies the complete allocation, as in normal ARM
        // compilation; dispatch entries below carry their own canonical offsets.
        cache->offset_in = buffer.offset;
        cache->cache_size = buffer.size;
        node->code_lifetime = lifetime;
        auto node_guard = unit.is_function ? static_cast<ir::Function*>(node)->LockWrite()
                                           : static_cast<ir::Block*>(node)->LockWrite();
        RetirementControl::PublicationScope admission{producer};
        if (!admission) return reject();
        SmcTracker::PublicationScope::CommitScope commit{installation};
        const auto rollback = ScopeExit([&] {
            if (committed) return;
            lifetime->Revoke();
            if (published) {
                for (const auto entry : lifetime->entries)
                    (void)address_space.GetLinkManager().SignalInvalidateTarget(entry);
                commit.RevokeNode(address_space, module, node);
            }
        });
        const auto guest_matches = [&] {
            for (const auto& block : unit.blocks) {
                u64 hash{};
                if (!HashGuestRange(block.guest_start, block.guest_end, hash) ||
                    hash != block.guest_bytes_hash) return false;
            }
            return true;
        };
        if (!guest_matches()) return reject();
        for (const auto& block : unit.blocks)
            if (block.IsPublished() && entry_owned(block.guest_start)) return reject();
        if (!module->Push(node)) return reject();
        published = true;
        if (!rv)
            module->AddFaultEntry(buffer.exec_data, buffer.exec_data + buffer.size,
                                  unit.guest_start, buffer.exec_data);
        for (const auto& site : unit.fault_sites)
            module->AddFaultEntry(buffer.exec_data + site.host_begin, buffer.exec_data + site.host_end,
                    site.guest_start, buffer.exec_data,
                    site.recovery_offset == UINT32_MAX ? nullptr : buffer.exec_data + site.recovery_offset,
                    static_cast<FaultRecoveryKind>(site.recovery_kind));
        if (!rv && address_space.ExitLatchEnabled()) {
            for (size_t i = 0; i < emitted.size(); ++i) {
                const u32 end = i + 1 < emitted.size() ? emitted[i + 1]->code_offset : buffer.size;
                module->AddFaultEntry(buffer.exec_data + emitted[i]->code_offset, buffer.exec_data + end,
                                      emitted[i]->guest_start, buffer.exec_data);
            }
        }
        for (const auto& block : unit.blocks)
            if (!module->GetModuleConfig().read_only)
                commit.RegisterNode(module, node, block.guest_start, block.guest_end);
        if (lifetime->Revoked()) throw std::runtime_error("disk guest protection failed or write window is open");
        if (!guest_matches()) throw std::runtime_error("guest bytes changed before disk commit");
        FunctionEntryPublisher publisher{*module, buffer.exec_data, static_cast<u32>(buffer.size)};
        for (const auto& block : unit.blocks) {
            if (!block.IsPublished()) continue;
            const auto contract = FunctionEntryContract::Restore(
                ir::Location{block.guest_start}, ir::Location{block.guest_end},
                {.canonical = block.code_offset, .direct_link = block.direct_code_offset,
                 .pending_flags = block.pending_flags_code_offset, .continuation = block.continuation_code_offset,
                 .pending_flags_continuation = block.pending_flags_continuation_code_offset},
                block.pending_flags_contract, block.IsLinkable());
            (void)publisher.Publish(contract);
        }
        {
            std::lock_guard guard(lock);
            const auto [position, inserted] = units.try_emplace(unit.guest_start, std::move(saved_unit));
            if (!inserted) throw std::runtime_error("disk unit persistence owner already exists");
            if (!lifetime->Commit()) {
                units.erase(position);
                throw std::runtime_error("disk unit retired before commit");
            }
            committed = true;
        }
        stats.units_loaded.fetch_add(1, std::memory_order_relaxed);
        return true;
    } catch (const std::bad_alloc&) {
        stats.reject_alloc.fetch_add(1, std::memory_order_relaxed);
        dirty = true;
        return false;
    } catch (...) {
        return reject();
    }
}

void JitDiskCache::Load() {
    RetirementControl::Lease producer{address_space.GetRetirementControl(), RetirementControl::Kind::Producer};
    if (!producer) return;
    PerfScope2 perf_load{GetPerfStats2().cache_load};
    if (load_attempted.exchange(true, std::memory_order_acq_rel) || !enabled) {
        return;
    }
    const auto path = FilePath();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return;  // cold run
    }
    std::vector<u8> raw;
    {
        std::fseek(f, 0, SEEK_END);
        const auto size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size <= 0) {
            std::fclose(f);
            stats.reject_header.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        raw.resize(static_cast<std::size_t>(size));
        const bool ok = std::fread(raw.data(), 1, raw.size(), f) == raw.size();
        std::fclose(f);
        if (!ok) {
            stats.reject_header.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    BlobReader reader{raw.data(), raw.size()};
    char magic[sizeof(kMagic)];
    ValidityKey key{};
    u64 payload_size{};
    u64 payload_hash{};
    if (!reader.Bytes(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 ||
        !reader.U64(key.format_version) || !reader.U64(key.build_id) ||
        !reader.U64(key.config_hash) || !reader.U64(key.env_hash) || !reader.U64(key.guest_id) ||
        !reader.U64(payload_size) || !reader.U64(payload_hash)) {
        stats.reject_header.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (key != Key() || payload_size != reader.Remaining()) {
        stats.reject_header.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (HashBytes(raw.data() + raw.size() - payload_size,
                  static_cast<std::size_t>(payload_size),
                  0xCBF29CE484222325ull) != payload_hash) {
        stats.reject_header.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Replay the L2 dispatch-slot assignment first, all-or-nothing: cached
    // code branches through raw slot indices, so a slot that ends up holding a
    // different guest location is a wild jump, not a miss.
    u64 slot_count{};
    if (!reader.U64(slot_count) || slot_count > reader.Remaining() / 12) {
        stats.reject_header.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::vector<std::pair<u64, u32>> slots;
    slots.reserve(static_cast<std::size_t>(slot_count));
    for (u64 i = 0; i < slot_count; ++i) {
        u64 slot_key{};
        u32 index{};
        if (!reader.U64(slot_key) || !reader.U32(index)) {
            stats.reject_header.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        slots.emplace_back(slot_key, index);
    }
    u64 unit_count{};
    if (!reader.U64(unit_count)) {
        stats.reject_header.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& table = address_space.GetCodeCacheTable();
    if (!table.ReplaySlots(slots)) {
        LOG_WARNING("SVM_JIT_CACHE: dispatch slot replay conflict; cache ignored");
        stats.reject_header.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    stats.dispatch_slots.store(slots.size(), std::memory_order_relaxed);
    for (u64 i = 0; i < unit_count; ++i) {
        SerialUnit unit{};
        if (!ReadUnit(reader, unit)) {
            // Truncated/garbled tail: keep what already loaded (each unit was
            // fully validated on its own) and stop.
            stats.reject_header.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        if (unit.blocks.empty()) {
            stats.reject_reloc.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        auto module = address_space.GetModule(unit.guest_start);
        const auto current_hash =
                HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
        if (unit.feature_hash != current_hash) {
            stats.reject_feature.fetch_add(1, std::memory_order_relaxed);
            dirty = true;
            continue;
        }
        stats.feature_match.fetch_add(1, std::memory_order_relaxed);
        ReviveUnit(module, unit);
    }
}

}  // namespace swift::runtime::backend
