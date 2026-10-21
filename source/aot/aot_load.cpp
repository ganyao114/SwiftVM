//
// See aot_load.h.
//

#include "aot/aot_load.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <elfio/elfio.hpp>
#include "fmt/format.h"

#include "runtime/backend/address_space.h"
#include "runtime/backend/code_serial.h"
#include "runtime/backend/function_entry_contract.h"
#include "runtime/backend/module.h"
#include "runtime/backend/riscv64/link.h"
#include "runtime/ir/function.h"

namespace swift::aot {

namespace backend = swift::runtime::backend;
namespace ir = swift::runtime::ir;

class InstalledCodeMap final : public backend::TerminalAliasOwner {
public:
    explicit InstalledCodeMap(std::shared_ptr<backend::RetirementControl> control)
            : control(std::move(control)) {}

    void Insert(u64 offset, const std::shared_ptr<backend::CodeLifetime>& lifetime) {
        std::lock_guard guard(mutex);
        if (revoked || lifetime->Revoked()) throw std::runtime_error("AOT mapping is revoked");
        if (!entries.emplace(offset, lifetime).second)
            throw std::runtime_error("duplicate AOT allocation offset");
    }

    void* Resolve(u64 offset) const {
        backend::RetirementControl::Lease producer{control, backend::RetirementControl::Kind::Producer};
        if (!producer) return nullptr;
        std::lock_guard guard(mutex);
        if (revoked) return nullptr;
        auto it = entries.upper_bound(offset);
        if (it == entries.begin()) return nullptr;
        --it;
        const auto& lifetime = it->second;
        return lifetime->Live() && offset - it->first < lifetime->size
                ? lifetime->base + (offset - it->first) : nullptr;
    }

    void Revoke() noexcept override {
        std::lock_guard guard(mutex);
        revoked = true;
        for (const auto& [offset, lifetime] : entries) lifetime->Revoke();
        entries.clear();
    }

private:
    std::shared_ptr<backend::RetirementControl> control;
    mutable std::mutex mutex;
    std::map<u64, std::shared_ptr<backend::CodeLifetime>> entries;
    bool revoked{};
};

namespace {

struct LatestInstallation {
    std::mutex mutex;
    std::weak_ptr<InstalledCodeMap> mapping;
};

LatestInstallation& Latest() {
    static LatestInstallation latest;
    return latest;
}

std::shared_ptr<InstalledCodeMap> LatestMap() {
    auto& latest = Latest();
    std::lock_guard guard(latest.mutex);
    return latest.mapping.lock();
}

bool HashGuestRange(void* base, u64 mask, u64 start, u64 end, u64& out) {
    return backend::HashGuestMemory(base, mask, start, end, out);
}

}  // namespace

void* ResolveCodeOffset(u64 offset) {
    return ResolveCodeOffset(LatestMap(), offset);
}

void* ResolveCodeOffset(const std::shared_ptr<InstalledCodeMap>& mapping, u64 offset) {
    return mapping ? mapping->Resolve(offset) : nullptr;
}

int InstallArtifact(backend::AddressSpace& address_space,
                    const AotImage& artifact,
                    void* guest_memory_base,
                    u64 guest_addr_mask,
                    LoadReport& report,
                    std::string& error) {
    backend::RetirementControl::Lease producer{
            address_space.GetRetirementControl(), backend::RetirementControl::Kind::Producer};
    if (!producer) {
        error = "runtime admission is closed";
        return kAotBadArgument;
    }
    const auto& config = address_space.GetConfig();
    if (artifact.host_isa != config.backend_isa) {
        error = "artifact host ISA differs from the runtime backend";
        return kAotKeyMismatch;
    }
    const auto& host_image = backend::GetHostImage();
    if (host_image.size == 0) {
        error = "cannot determine the SwiftVM host image span";
        return kAotBadArgument;
    }
    if (!guest_memory_base) {
        error = "AOT requires a biased guest window (Config::memory_base)";
        return kAotBadArgument;
    }

    // --- validity key -----------------------------------------------------
    // Every field must match; docs/aot-design.md §4 is explicit that a miss
    // here is the most dangerous silent-error class this project has.
    const backend::ValidityKey live{backend::kCacheFormatVersion,
                                    backend::ComputeBuildId(),
                                    backend::ComputeConfigHash(config),
                                    backend::ComputeEnvHash(),
                                    artifact.key.guest_id};
    if (artifact.key.format_version != live.format_version) {
        error = fmt::format("format version {} != {}", artifact.key.format_version,
                            live.format_version);
        return kAotKeyMismatch;
    }
    if (artifact.key.build_id != live.build_id) {
        error = "artifact was produced by a different SwiftVM build";
        return kAotKeyMismatch;
    }
    if (artifact.key.config_hash != live.config_hash) {
        error = "runtime Config differs from the one the artifact was compiled under";
        return kAotKeyMismatch;
    }
    if (artifact.key.env_hash != live.env_hash) {
        error = "SVM_*/SWIFT_* environment differs from the compile-time environment";
        return kAotKeyMismatch;
    }
    // Internal consistency of the carried guest image. The authoritative
    // guest check is the per-unit byte hash below; this catches an artifact
    // whose segment table was edited without touching the key.
    if (HashGuestImage(artifact.segments, artifact.guest.entry) != artifact.key.guest_id) {
        error = "guest image hash does not match the artifact key";
        return kAotKeyMismatch;
    }

    // Range invalidation cannot slip between guest-byte validation and commit.
    // Signal writers are excluded separately, only during each unit's commit.
    backend::SmcTracker::PublicationScope installation{address_space.GetSmcTracker()};

    // --- pass 1: nothing is installed until every unit checks out ---------
    std::vector<u64> block_hashes;
    std::unordered_set<u64> published_locations;
    const auto entry_owned = [&](u64 location) {
        return address_space.GetCodeCache(ir::Location{location}) ||
               address_space.GetCallCodeCacheTable().Lookup(location) ||
               address_space.GetPendingCallCodeCacheTable().Lookup(location) ||
               !backend::IsEmpty(address_space.GetModule(location)->GetNode(ir::Location{location}));
    };
    for (const auto& unit : artifact.units) {
        if (unit.blocks.empty() || unit.is_function > 1) {
            error = fmt::format("unit {:#x} has no blocks", unit.guest_start);
            return kAotBadFormat;
        }
        if (static_cast<u64>(unit.code_offset) + unit.code_size > artifact.code.size()) {
            error = fmt::format("unit {:#x} code range is out of bounds", unit.guest_start);
            return kAotBadFormat;
        }
        if (!unit.ValidFaultSites()) {
            error = fmt::format("unit {:#x} has invalid fault metadata", unit.guest_start);
            return kAotBadFormat;
        }
        if (!backend::ValidRegionBranches(unit.region_branches, unit.code_size) ||
            (!unit.region_branches.empty() && (config.backend_isa != runtime::kArm64 ||
             !address_space.GetModule(unit.guest_start)->IsDirectLinkConfigured()))) {
            error = "invalid shared region branch metadata";
            return kAotBadFormat;
        }
        for (const auto& branch : unit.region_branches) {
            u32 instruction{};
            std::memcpy(&instruction, artifact.code.data() + unit.code_offset + branch.code_offset, 4);
            if (instruction != 0x1400'0000u) {
                error = "changed shared region branch placeholder";
                return kAotBadFormat;
            }
        }
        if (!unit.ValidLinkSites() || (!unit.link_sites.empty() &&
             !address_space.GetModule(unit.guest_start)->IsDirectLinkConfigured())) {
            error = fmt::format("unit {:#x} has invalid direct-link metadata", unit.guest_start);
            return kAotBadFormat;
        }
        for (const auto& site : unit.link_sites) {
            u32 instruction;
            std::memcpy(&instruction, artifact.code.data() + unit.code_offset + site.code_offset, 4);
            if (instruction != site.unlinked_instruction ||
                (config.backend_isa == runtime::kArm64 ? instruction != 0x94000000u :
                    (instruction & 0xfffu) != (0x6fu | (28u << 7)))) {
                error = fmt::format("unit {:#x} has a changed direct-link instruction", unit.guest_start);
                return kAotBadFormat;
            }
            if (site.HasFlagsBypass()) {
                std::memcpy(&instruction, artifact.code.data() + unit.code_offset + site.flags_bypass_offset, 4);
                if (instruction != site.flags_bypass_instruction) {
                    error = "changed flags-bypass instruction";
                    return kAotBadFormat;
                }
                if (site.flags_merge_branch_offset != UINT32_MAX) {
                    std::memcpy(&instruction, artifact.code.data() + unit.code_offset + site.flags_merge_branch_offset, 4);
                    if (instruction != 0x14000000u) {
                        error = "changed flags-merge branch placeholder";
                        return kAotBadFormat;
                    }
                }
            }
        }
        // Audit every unit's relocations before any L2 assignment or code is
        // published. A corrupt later unit must not leave an earlier unit live.
        auto relocated = std::vector<u8>(artifact.code.begin() + unit.code_offset,
                                         artifact.code.begin() + unit.code_offset + unit.code_size);
        if (!backend::ApplyRelocations(relocated.data(), relocated.size(), unit.relocs,
                                      host_image, &error, config.backend_isa)) return kAotRelocFailed;
        const auto root = std::find_if(unit.blocks.begin(), unit.blocks.end(), [&](const auto& block) {
            return block.IsPublished() && block.guest_start == unit.guest_start &&
                   (config.backend_isa != runtime::kRiscv64 || block.code_offset == 0);
        });
        if (root == unit.blocks.end()) {
            error = fmt::format("unit {:#x} has no root entry", unit.guest_start);
            return kAotBadFormat;
        }
        std::unordered_set<u64> block_locations;
        for (const auto& block : unit.blocks) {
            if (!block.IsDependency() && !block_locations.insert(block.guest_start).second) {
                error = fmt::format("unit {:#x} has duplicate block metadata", unit.guest_start);
                return kAotBadFormat;
            }
            u64 hash{};
            if (!HashGuestRange(guest_memory_base, guest_addr_mask, block.guest_start,
                                block.guest_end, hash)) {
                error = fmt::format("guest range [{:#x},{:#x}) is not readable",
                                    block.guest_start, block.guest_end);
                return kAotGuestMismatch;
            }
            if (hash != block.guest_bytes_hash) {
                error = fmt::format(
                        "guest bytes at [{:#x},{:#x}) are not what unit {:#x} was compiled from",
                        block.guest_start, block.guest_end, unit.guest_start);
                return kAotGuestMismatch;
            }
            if (!block.ValidEntryContract(unit.code_size)) {
                error = fmt::format("unit {:#x} has an out-of-range block entry",
                                    unit.guest_start);
                return kAotBadFormat;
            }
            // A secondary entry can belong to a different node even when this
            // unit's root is free. Check the complete installation before any
            // dispatch replay or allocation changes an independent owner.
            if (block.IsPublished() &&
                (!published_locations.insert(block.guest_start).second || entry_owned(block.guest_start))) {
                error = fmt::format("another unit already owns entry {:#x}", block.guest_start);
                return kAotBadFormat;
            }
        }
    }

    // Reserve rollback ownership before any unit can commit. A later failure
    // must revoke the entire installation, including earlier committed units.
    std::vector<std::pair<std::shared_ptr<backend::Module>, ir::NodeRef>> installed_units;
    installed_units.reserve(artifact.units.size());

    // --- pass 2: replay the L2 dispatch assignment ------------------------
    // Generated code branches through raw slot indices and a slot's direct
    // depends on the insertion order of colliding keys, so the assignment is
    // reproduced verbatim rather than recomputed. In a fresh process the
    // table is empty, so a conflict here means the artifact is inconsistent.
    auto& table = address_space.GetCodeCacheTable();
    if (!table.ReplaySlots(artifact.dispatch_slots)) {
        error = "dispatch assignment conflicts with accepted slots or has an invalid probe chain";
        return kAotDispatchConflict;
    }
    report.dispatch_slots = static_cast<u32>(artifact.dispatch_slots.size());

    // --- pass 3: install ---------------------------------------------------
    auto mapping = std::make_shared<InstalledCodeMap>(address_space.GetRetirementControl());
    if (!address_space.RegisterTerminalAlias(mapping)) {
        error = "AOT alias registration is closed";
        return kAotBadArgument;
    }
    bool installation_committed = false;
    const auto rollback_installation = ScopeExit([&] {
        if (installation_committed) return;
        mapping->Revoke();
        // First remove every visible entry without allocating. Even if an
        // ordinary cleanup retry then fails, retained storage is unreachable.
        {
            backend::SmcTracker::PublicationScope::CommitScope revoke{installation};
            for (const auto& [module, owner] : installed_units) {
                auto* node = owner.Get();
                if (node->code_lifetime) for (const auto entry : node->code_lifetime->entries)
                    (void)address_space.GetLinkManager().SignalInvalidateTarget(entry);
                revoke.RevokeNode(address_space, module, node);
            }
        }
        for (const auto& [module, owner] : installed_units) {
            try { (void)installation.RetireNode(address_space, module, owner.Get()); }
            catch (...) {} // Module ownership retains failed cleanup for retry/shutdown.
        }
        report = {};
    });
    for (const auto& unit : artifact.units) {
        auto module = address_space.GetModule(unit.guest_start);
        ir::NodeRef node_owner;
        bool published = false;
        try {
            auto module_guard = module->ModuleLockWrite();
            // RV links use allocation-local cold stubs. Requiring an ARM
            // region trampoline there skips every reusable RV arena.
            const bool require_region = config.backend_isa == runtime::kArm64 &&
                    (!unit.region_branches.empty() || !unit.link_sites.empty());
            auto [idx, buffer] = module->AllocCodeCache(unit.code_size, require_region);
            if (idx == backend::INVALID_CACHE_ID) {
                error = fmt::format("no code cache room for unit {:#x}", unit.guest_start);
                return kAotNoRoom;
            }
            std::shared_ptr<backend::CodeLifetime> lifetime;
            const auto discard = ScopeExit([&] {
                if (published) return;
                if (lifetime) lifetime->Revoke();
                // If metadata cleanup itself fails, the Module still owns the
                // arena. Do not free bytes that could retain fault/link records.
                try {
                    module->DiscardLinkSource(buffer.exec_data);
                    module->ReclaimCode(buffer.exec_data);
                } catch (...) {
                }
            });
            std::memcpy(buffer.rw_data, artifact.code.data() + unit.code_offset, unit.code_size);
            std::string reloc_error;
            if (!backend::ApplyRelocations(buffer.rw_data,
                                           unit.code_size,
                                           unit.relocs,
                                           host_image,
                                           &reloc_error,
                                           config.backend_isa)) {
                error = fmt::format(
                        "relocation failed for unit {:#x}: {}", unit.guest_start, reloc_error);
                return kAotRelocFailed;
            }
            if (auto* cache = module->GetCodeCache(buffer.exec_data);
                !cache || !backend::RebindRegionBranches(*cache, buffer.exec_data,
                    {buffer.rw_data, buffer.size}, unit.region_branches)) {
                error = "cannot rebind shared region branches";
                return kAotRelocFailed;
            }
            if (config.backend_isa == runtime::kArm64 && !backend::RegisterArmSerialLinks(*module, buffer, unit.link_sites)) {
                error = fmt::format("cannot register ARM direct links for unit {:#x}", unit.guest_start);
                return kAotBadFormat;
            }
            buffer.Flush();
            if (config.backend_isa == runtime::kRiscv64 &&
                !backend::riscv64::RegisterLinkSites(module, buffer, unit.link_sites)) {
                error = fmt::format("cannot register direct links for unit {:#x}",
                                    unit.guest_start);
                return kAotBadFormat;
            }

            std::vector<VAddr> entries;
            for (const auto& block : unit.blocks)
                if (block.IsPublished()) entries.push_back(block.guest_start);
            lifetime = std::make_shared<backend::CodeLifetime>(
                    buffer.exec_data, buffer.size, std::move(entries));

            // Publish the same shape a normal compile would have left behind, so
            // SMC invalidation, the fault table and the dispatcher all see what
            // they expect. A block-mode unit stays a block: turning it into a
            // one-block function would claim a whole-function translation that
            // does not exist.
            ir::AddressNode* node = nullptr;
            backend::JitCache* jit_cache = nullptr;
            const auto root =
                    std::find_if(unit.blocks.begin(), unit.blocks.end(), [&](const auto& entry) {
                        return entry.IsPublished() && entry.guest_start == unit.guest_start;
                    });
            if (unit.is_function) {
                auto* function = new ir::Function(ir::Location{unit.guest_start});
                node_owner = ir::NodeRef{function};
                VAddr max_end = unit.guest_start;
                for (const auto& block : unit.blocks) {
                    if (block.IsDependency()) continue;
                    auto* ir_block = new ir::Block(ir::Location{block.guest_start});
                    ir_block->SetEndLocation(ir::Location{block.guest_end});
                    ir_block->code_lifetime = lifetime;
                    if (block.IsPublished() && config.backend_isa == runtime::kRiscv64) {
                        auto& cache = ir_block->GetJitCache();
                        cache.jit_state = backend::JitState::Cached;
                        cache.cache_id = idx;
                        cache.offset_in = buffer.offset + block.code_offset;
                        cache.cache_size = buffer.size - block.code_offset;
                    }
                    function->AddBlock(ir_block);
                    max_end = std::max<VAddr>(max_end, block.guest_end);
                }
                function->SetEndLocation(ir::Location{max_end});
                node = function;
                jit_cache = &function->GetJitCache();
            } else {
                auto* block = new ir::Block(ir::Location{unit.guest_start});
                node_owner = ir::NodeRef{block};
                block->SetEndLocation(ir::Location{root->guest_end});
                node = block;
                jit_cache = &block->GetJitCache();
            }
            jit_cache->jit_state = backend::JitState::Cached;
            jit_cache->cache_id = idx;
            jit_cache->offset_in = buffer.offset + root->code_offset;
            jit_cache->cache_size = buffer.size - root->code_offset;
            node->code_lifetime = lifetime;
            mapping->Insert(unit.code_offset, lifetime);

            auto node_guard = unit.is_function ? static_cast<ir::Function*>(node)->LockWrite()
                                               : static_cast<ir::Block*>(node)->LockWrite();
            backend::RetirementControl::PublicationScope admission{producer};
            if (!admission) throw std::runtime_error("AOT publication admission is closed");
            backend::SmcTracker::PublicationScope::CommitScope commit{installation};
            bool committed = false;
            const auto rollback = ScopeExit([&] {
                if (!committed) {
                    lifetime->Revoke();
                    if (published) {
                        // A later entry can fail after an earlier target was
                        // published. Revoke those targets before allocating
                        // cleanup work, which may itself fail independently.
                        for (const auto entry : lifetime->entries)
                            (void)address_space.GetLinkManager().SignalInvalidateTarget(entry);
                        commit.RevokeNode(address_space, module, node);
                    }
                }
            });

            for (const auto& block : unit.blocks) {
                u64 hash{};
                if (!HashGuestRange(guest_memory_base,
                                    guest_addr_mask,
                                    block.guest_start,
                                    block.guest_end,
                                    hash) ||
                    hash != block.guest_bytes_hash) {
                    error = "guest bytes changed before AOT publication";
                    return kAotGuestMismatch;
                }
            }

            for (const auto& block : unit.blocks) {
                if (block.IsPublished() && entry_owned(block.guest_start)) {
                    error = fmt::format("another unit acquired entry {:#x} before AOT publication", block.guest_start);
                    return kAotBadFormat;
                }
            }
            if (!module->Push(node)) {
                error = fmt::format("another node already owns {:#x}", unit.guest_start);
                return kAotBadFormat;
            }
            published = true;
            for (const auto& site : unit.fault_sites) {
                module->AddFaultEntry(buffer.exec_data + site.host_begin,
                                      buffer.exec_data + site.host_end,
                                      site.guest_start,
                                      buffer.exec_data,
                                      site.recovery_offset == UINT32_MAX
                                              ? nullptr
                                              : buffer.exec_data + site.recovery_offset,
                                      static_cast<backend::FaultRecoveryKind>(site.recovery_kind));
            }

            for (const auto& block : unit.blocks) {
                if (!module->GetModuleConfig().read_only)
                    commit.RegisterNode(module, node, block.guest_start, block.guest_end);
            }
            if (lifetime->Revoked()) throw std::runtime_error("AOT guest write window is open");
            for (const auto& block : unit.blocks) {
                u64 hash{};
                if (!HashGuestRange(guest_memory_base, guest_addr_mask, block.guest_start,
                                    block.guest_end, hash) || hash != block.guest_bytes_hash)
                    throw std::runtime_error("guest bytes changed before AOT commit");
            }
            for (const auto& block : unit.blocks) {
                if (block.IsPublished()) {
                    const auto contract = backend::FunctionEntryContract::Restore(
                        ir::Location{block.guest_start}, ir::Location{block.guest_end},
                        {.canonical = block.code_offset, .direct_link = block.direct_code_offset,
                         .pending_flags = block.pending_flags_code_offset,
                         .continuation = block.continuation_code_offset,
                         .pending_flags_continuation = block.pending_flags_continuation_code_offset},
                        block.pending_flags_contract, block.IsLinkable());
                    backend::FunctionEntryPublisher publisher{*module, buffer.exec_data, unit.code_size};
                    (void)publisher.Publish(contract);
                }
            }
            if (!lifetime->Commit()) throw std::runtime_error("AOT unit retired before commit");
            committed = true;
            installed_units.emplace_back(module, node_owner);
            report.units_installed++;
            report.code_bytes += unit.code_size;
        } catch (...) {
            if (published) {
                // The commit rollback revoked aliases/slots before releasing
                // metadata. Already visible code must use normal QSBR grace.
                try {
                    (void)installation.RetireNode(address_space, module, node_owner.Get());
                } catch (...) {
                }  // The attached Module retains failed cleanup for terminal release.
            }
            throw;
        }
    }
    report.mapping = mapping;
    {
        auto& latest = Latest();
        std::lock_guard guard(latest.mutex);
        latest.mapping = mapping;
    }
    installation_committed = true;
    return kAotOk;
}

// --------------------------------------------------------------------------
// Symbol index
// --------------------------------------------------------------------------
bool SymbolIndex::Build(const std::string& artifact_path,
                        const AotImage& artifact,
                        std::string& error,
                        const std::shared_ptr<InstalledCodeMap>& installed) {
    using namespace ELFIO;
    elfio r;
    if (!r.load(artifact_path)) {
        error = "cannot re-open the artifact for its symbol table";
        return false;
    }
    const section* symtab = nullptr;
    for (Elf_Half i = 0; i < r.sections.size(); ++i) {
        if (r.sections[i]->get_type() == SHT_SYMTAB) {
            symtab = r.sections[i];
            break;
        }
    }
    if (!symtab) {
        error = "artifact has no .symtab";
        return false;
    }
    auto next_mapping = installed ? installed : LatestMap();
    if (!next_mapping) {
        error = "no accepted AOT installation for symbols";
        return false;
    }
    decltype(by_name) next_names;
    const_symbol_section_accessor syms{r, const_cast<section*>(symtab)};
    for (Elf_Xword i = 0; i < syms.get_symbols_num(); ++i) {
        std::string name;
        Elf64_Addr value{};
        Elf_Xword size{};
        unsigned char bind{}, type{}, other{};
        Elf_Half shndx{};
        if (!syms.get_symbol(i, name, value, size, bind, type, shndx, other)) {
            continue;
        }
        // STT_GNU_IFUNC too: the writer rewrote it, so skipping it here would
        // make Lookup() silently miss `strlen`/`memcpy`/`strcmp` -- 37 names in
        // func_tests_x86_64, and exactly the ones a host is most likely to ask
        // for. What comes back is the compiled RESOLVER; the caller must run it
        // (AotGuestEnv::LookupSymbol does).
        if (!IsCompilableFuncType(type) || name.empty()) {
            continue;
        }
        if (value < kAotCodeVaddr || value >= kAotCodeVaddr + artifact.code.size()) {
            continue;
        }
        next_names.emplace(name, value - kAotCodeVaddr);
    }
    by_name = std::move(next_names);
    mapping = std::move(next_mapping);
    return true;
}

void* SymbolIndex::Lookup(const std::string& name) const {
    auto it = by_name.find(name);
    if (it == by_name.end()) {
        return nullptr;
    }
    return ResolveCodeOffset(mapping, it->second);
}

}  // namespace swift::aot

// --------------------------------------------------------------------------
// C handshake (aot_format.h)
// --------------------------------------------------------------------------
namespace {
std::string g_last_error{"no error"};
unsigned long g_installed_units{};
}  // namespace

extern "C" int swift_aot_init(const SwiftAotRuntime* rt) {
    using namespace swift::aot;
    g_installed_units = 0;
    if (!rt || rt->struct_size != sizeof(SwiftAotRuntime) ||
        rt->abi_version != kSwiftAotAbiVersion || !rt->address_space || !rt->artifact_path) {
        g_last_error = "SwiftAotRuntime is malformed or from a different ABI";
        return kAotBadArgument;
    }
    AotImage artifact{};
    std::string error;
    if (!ReadArtifact(rt->artifact_path, artifact, error)) {
        g_last_error = error;
        return kAotUnreadable;
    }
    LoadReport report{};
    const int status = InstallArtifact(
            *static_cast<swift::runtime::backend::AddressSpace*>(rt->address_space),
            artifact,
            rt->guest_memory_base,
            rt->guest_addr_mask,
            report,
            error);
    if (status != kAotOk) {
        g_last_error = error;
        return status;
    }
    g_last_error = "no error";
    g_installed_units = report.units_installed;
    return kAotOk;
}

extern "C" const char* swift_aot_last_error(void) { return g_last_error.c_str(); }

extern "C" unsigned long swift_aot_installed_units(void) { return g_installed_units; }
