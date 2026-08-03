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
#include <sys/stat.h>
#include <unistd.h>

#include "runtime/backend/address_space.h"
#include "runtime/common/backedge_control.h"
#include "runtime/backend/module.h"
#include "runtime/common/fpcr_tax_prof.h"
#include "runtime/common/hot_coalesce_prof.h"
#include "runtime/common/logging.h"
#include "runtime/common/perf_stats.h"
#include "runtime/ir/function.h"

namespace swift::runtime::backend {

namespace {

constexpr char kMagic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
// A guest block longer than this is a decoder bug, not a basic block; refuse
// rather than hash (or probe) an arbitrary range out of a corrupt file.
constexpr u64 kMaxGuestBlockBytes = 1u << 20;

bool EnvOn(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && std::strcmp(v, "0") != 0;
}

// Readability probe that never faults: the kernel validates the buffer and
// returns EFAULT instead of delivering a signal. mincore() is not usable here
// because the guest window is one big PROT_NONE reservation -- its pages are
// "mapped" but unreadable.
bool RangeIsReadable(const void* p, std::size_t n) {
    static int devnull = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (devnull < 0 || n == 0) {
        return devnull >= 0;
    }
    const auto* cursor = static_cast<const u8*>(p);
    std::size_t left = n;
    while (left > 0) {
        const auto chunk = left > 4096 ? std::size_t{4096} : left;
        const auto written = ::write(devnull, cursor, chunk);
        if (written < 0) {
            return false;
        }
        cursor += written;
        left -= static_cast<std::size_t>(written);
    }
    return true;
}

}  // namespace

bool JitDiskCache::Requested() {
    const char* dir = std::getenv("SVM_JIT_CACHE");
    return dir && dir[0];
}

JitDiskCache::JitDiskCache(AddressSpace& space)
        : address_space(space), host_image(GetHostImage()) {
    const char* env = std::getenv("SVM_JIT_CACHE");
    if (!env || !env[0]) {
        return;
    }
    print_stats = EnvOn("SVM_JIT_CACHE_STATS");
    dir = env;
    // Profiled JIT units embed a process-local counter slot. Serializing one
    // would revive code with no matching metadata slot in the next process.
    // The probe is measurement-only, so disable disk caching while it is on.
    if (HotCoalesceProfEnabled()) {
        LOG_WARNING("SVM_JIT_CACHE: SVM_RA_HOT_COALESCE is incompatible; cache disabled");
        return;
    }
    if (FpcrTaxProfEnabled()) {
        LOG_WARNING("SVM_JIT_CACHE: SVM_FPCR_TAX_PROF is incompatible; cache disabled");
        return;
    }
    const auto& config = address_space.GetConfig();
    if (BackedgeFlagsEnabled() ||
        (config.region_edges && EnvOn("SVM_BACKEDGE_FLAGS"))) {
        // Recovery veneers are block-local code offsets. SerialBlock does
        // not yet serialize that relocation/eligibility contract, so refuse
        // disk reuse rather than reviving a unit with an imprecise recipe.
        LOG_WARNING("SVM_JIT_CACHE: SVM_BACKEDGE_FLAGS is incompatible; cache disabled");
        return;
    }
    if (host_image.size == 0) {
        LOG_WARNING("SVM_JIT_CACHE: host image span unknown; cache disabled");
        return;
    }
    if (!config.memory_base) {
        // Without a bias the cache cannot read guest bytes safely, and guest
        // addresses become plausible memory bases (see code_serial.h).
        LOG_WARNING("SVM_JIT_CACHE: identity-mapped guest memory is unsupported; cache disabled");
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
        fmt::print(stderr,
                   "[jit-cache] loaded={} compiled={} stored={} "
                   "reject(header={} guest={} reloc={} alloc={} scan={}) dispatch_slots={}\n",
                   stats.units_loaded.load(),
                   stats.units_compiled.load(),
                   stats.units_stored.load(),
                   stats.reject_header.load(),
                   stats.reject_guest_bytes.load(),
                   stats.reject_reloc.load(),
                   stats.reject_alloc.load(),
                   stats.reject_scan.load(),
                   stats.dispatch_slots.load());
    }
}

ValidityKey JitDiskCache::Key() const {
    return ValidityKey{kCacheFormatVersion,
                       ComputeBuildId(),
                       ComputeConfigHash(address_space.GetConfig()),
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
    if (end <= start || end - start > kMaxGuestBlockBytes) {
        return false;
    }
    const auto& config = address_space.GetConfig();
    const u64 mask = config.guest_addr_mask ? config.guest_addr_mask : UINT64_MAX;
    if (config.guest_addr_mask) {
        const u64 lo = start & mask;
        if (end - start > mask + 1 - lo) {
            return false;
        }
        if ((start & ~mask) != (end - 1 & ~mask)) {
            return false;
        }
    }
    const auto host = reinterpret_cast<const u8*>(config.memory_base) + (start & mask);
    const auto len = static_cast<std::size_t>(end - start);
    if (!RangeIsReadable(host, len)) {
        return false;
    }
    out = HashBytes(host, len, 0xCBF29CE484222325ull);
    return true;
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
                              const std::vector<SerialLinkSite>& link_sites) {
    stats.units_compiled.fetch_add(1, std::memory_order_relaxed);
    if (!enabled || !exec_data || !rw_data || code_size == 0 || blocks.empty()) {
        return;
    }

    SerialUnit unit{};
    unit.guest_start = guest_start;
    unit.is_function = is_function ? 1 : 0;
    unit.link_sites = link_sites;
    std::sort(unit.link_sites.begin(), unit.link_sites.end(),
              [](const auto& left, const auto& right) {
                  return left.code_offset < right.code_offset;
              });

    std::vector<u32> external_bl_offsets;
    external_bl_offsets.reserve(unit.link_sites.size());
    std::vector<u32> normalized_bl;
    normalized_bl.reserve(unit.link_sites.size());
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
        auto* trampoline = region->rx_base + region->trampoline_offset;
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
            const auto branch = EncodeBL(trampoline - rx_site);
            if (!branch) {
                stats.reject_scan.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            external_bl_offsets.push_back(site.code_offset);
            normalized_bl.push_back(*branch);
            previous_offset = site.code_offset;
            first = false;
        }
    }

    // Copy immutable spans only. A live linker or invalidator may atomically
    // patch a site at this exact moment; never read that word through memcpy.
    // Each skipped word is synthesized as the unlinked BL form, so the stored
    // snapshot is independent of Linked/Far/Unlinked runtime state.
    unit.code.resize(code_size);
    size_t cursor{};
    for (size_t i = 0; i < unit.link_sites.size(); ++i) {
        const size_t offset = unit.link_sites[i].code_offset;
        if (offset > cursor) {
            std::memcpy(unit.code.data() + cursor, rw_data + cursor, offset - cursor);
        }
        std::memcpy(unit.code.data() + offset, &normalized_bl[i], sizeof(u32));
        cursor = offset + sizeof(u32);
    }
    if (cursor < code_size) {
        std::memcpy(unit.code.data() + cursor, rw_data + cursor, code_size - cursor);
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
    // Snapshot the L2 dispatch-table assignment. The indices baked into the
    // cached code are table slots, and a slot's identity depends on the
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
    PerfScope2 perf_revive{GetPerfStats2().cache_revive};
    // 1. The guest bytes this code was produced from must still be there.
    for (const auto& block : unit.blocks) {
        u64 hash{};
        if (!HashGuestRange(block.guest_start, block.guest_end, hash) ||
            hash != block.guest_bytes_hash) {
            stats.reject_guest_bytes.fetch_add(1, std::memory_order_relaxed);
            dirty = true;
            return false;
        }
        if (block.code_offset % 4 != 0 || block.code_offset >= unit.code.size()) {
            stats.reject_reloc.fetch_add(1, std::memory_order_relaxed);
            dirty = true;
            return false;
        }
    }
    if (!unit.link_sites.empty() && !module->IsDirectLinkConfigured()) {
        stats.reject_reloc.fetch_add(1, std::memory_order_relaxed);
        dirty = true;
        return false;
    }
    for (const auto& site : unit.link_sites) {
        if ((site.code_offset & 3u) != 0 ||
            static_cast<size_t>(site.code_offset) + sizeof(u32) > unit.code.size() ||
            site.kind >= static_cast<u8>(LinkSiteKind::Count)) {
            stats.reject_reloc.fetch_add(1, std::memory_order_relaxed);
            dirty = true;
            return false;
        }
    }

    // 2. Place the code.
    auto [idx, buffer] = module->AllocCodeCache(
            static_cast<u32>(unit.code.size()), !unit.link_sites.empty());
    if (idx == INVALID_CACHE_ID) {
        stats.reject_alloc.fetch_add(1, std::memory_order_relaxed);
        dirty = true;
        return false;
    }
    std::memcpy(buffer.rw_data, unit.code.data(), unit.code.size());

    // 3. Re-bind every runtime address.
    std::string err;
    if (!ApplyRelocations(
                buffer.rw_data, unit.code.size(), unit.relocs, host_image, &err)) {
        LOG_WARNING("SVM_JIT_CACHE: relocation failed for {:#x}: {}", unit.guest_start, err);
        if (auto* cache = module->GetCodeCache(buffer.exec_data)) {
            cache->FreeCode(buffer.exec_data);
        }
        stats.reject_reloc.fetch_add(1, std::memory_order_relaxed);
        dirty = true;
        return false;
    }
    std::optional<CodeRegion> direct_region;
    std::vector<u32> unlinked_bl;
    if (!unit.link_sites.empty()) {
        direct_region = module->GetCodeRegion(buffer.exec_data);
        if (!direct_region ||
            direct_region->trampoline_offset == CodeRegion::kInvalidTrampolineOffset) {
            if (auto* cache = module->GetCodeCache(buffer.exec_data)) {
                cache->FreeCode(buffer.exec_data);
            }
            stats.reject_reloc.fetch_add(1, std::memory_order_relaxed);
            dirty = true;
            return false;
        }
        auto* trampoline = direct_region->rx_base + direct_region->trampoline_offset;
        unlinked_bl.reserve(unit.link_sites.size());
        for (const auto& site : unit.link_sites) {
            auto* rx_site = buffer.exec_data + site.code_offset;
            const auto branch = EncodeBL(trampoline - rx_site);
            if (!branch) {
                if (auto* cache = module->GetCodeCache(buffer.exec_data)) {
                    cache->FreeCode(buffer.exec_data);
                }
                stats.reject_reloc.fetch_add(1, std::memory_order_relaxed);
                dirty = true;
                return false;
            }
            std::memcpy(buffer.rw_data + site.code_offset, &*branch, sizeof(*branch));
            unlinked_bl.push_back(*branch);
        }
    }
    buffer.Flush();

    // 4. Publish an address node so SMC invalidation and the fault table see
    //    the same shape they would after a normal compile.
    ir::AddressNode* node{};
    JitCache* jit_cache{};
    if (unit.is_function) {
        auto* function = new ir::Function(ir::Location{unit.guest_start});
        VAddr max_end = unit.guest_start;
        for (const auto& block : unit.blocks) {
            auto* ir_block = new ir::Block(ir::Location{block.guest_start});
            ir_block->SetEndLocation(ir::Location{block.guest_end});
            function->AddBlock(ir_block);
            max_end = std::max<VAddr>(max_end, block.guest_end);
        }
        function->SetEndLocation(ir::Location{max_end});
        node = function;
        jit_cache = &function->GetJitCache();
    } else {
        auto* block = new ir::Block(ir::Location{unit.guest_start});
        block->SetEndLocation(ir::Location{unit.blocks.front().guest_end});
        node = block;
        jit_cache = &block->GetJitCache();
    }
    jit_cache->jit_state = JitState::Cached;
    jit_cache->cache_id = idx;
    jit_cache->offset_in = buffer.offset;
    jit_cache->cache_size = buffer.size;

    if (!module->Push(node)) {
        // Another entry already owns this location.
        if (unit.is_function) {
            delete static_cast<ir::Function*>(node);
        } else {
            delete static_cast<ir::Block*>(node);
        }
        if (auto* cache = module->GetCodeCache(buffer.exec_data)) {
            cache->FreeCode(buffer.exec_data);
        }
        stats.reject_reloc.fetch_add(1, std::memory_order_relaxed);
        dirty = true;
        return false;
    }
    module->AddFaultEntry(buffer.exec_data,
                          buffer.exec_data + buffer.size,
                          unit.guest_start,
                          buffer.exec_data);
    for (size_t i = 0;
         address_space.ExitLatchEnabled() && i < unit.blocks.size();
         ++i) {
        const u32 begin = unit.blocks[i].code_offset;
        const u32 end = i + 1 < unit.blocks.size()
                ? unit.blocks[i + 1].code_offset
                : static_cast<u32>(buffer.size);
        if (begin >= end || end > buffer.size) {
            // The serialized unit passed the structural reader but does not
            // describe non-overlapping emitted block subranges. Reject it
            // instead of falling back to imprecise function-entry recovery.
            module->Remove(node);
            if (unit.is_function) {
                delete static_cast<ir::Function*>(node);
            } else {
                delete static_cast<ir::Block*>(node);
            }
            if (auto* cache = module->GetCodeCache(buffer.exec_data)) {
                cache->FreeCode(buffer.exec_data);
            }
            stats.reject_reloc.fetch_add(1, std::memory_order_relaxed);
            dirty = true;
            return false;
        }
        module->AddFaultEntry(buffer.exec_data + begin,
                              buffer.exec_data + end,
                              unit.blocks[i].guest_start,
                              buffer.exec_data);
    }

    // 5. Install every source record only after all code words are in their
    // unlinked BL form, and before publishing any target or L2 entry. A partial
    // registration is rolled back as one owner transaction.
    if (direct_region) {
        const LinkSourceOwner owner{module.get(), buffer.exec_data};
        for (size_t i = 0; i < unit.link_sites.size(); ++i) {
            const auto& site = unit.link_sites[i];
            auto* rx_site = buffer.exec_data + site.code_offset;
            auto* rw_site = buffer.rw_data + site.code_offset;
            const LinkSiteKey key{direct_region->id, buffer.offset + site.code_offset};
            const LinkSignalPatchSite signal_patch{
                    .region = *direct_region,
                    .rx_site = rx_site,
                    .rw_site = rw_site,
                    .unlinked_bl = unlinked_bl[i],
            };
            if (!address_space.GetLinkManager().RegisterSite(
                        key,
                        site.guest_target,
                        owner,
                        &signal_patch,
                        static_cast<LinkSiteKind>(site.kind))) {
                module->DiscardLinkSource(buffer.exec_data);
                module->Remove(node);
                module->RemoveFaultEntries(buffer.exec_data);
                if (unit.is_function) {
                    delete static_cast<ir::Function*>(node);
                } else {
                    delete static_cast<ir::Block*>(node);
                }
                if (auto* cache = module->GetCodeCache(buffer.exec_data)) {
                    cache->FreeCode(buffer.exec_data);
                }
                stats.reject_reloc.fetch_add(1, std::memory_order_relaxed);
                dirty = true;
                return false;
            }
        }
    }

    // 6. Target publication is deliberately last: a revived site can be found
    // by the cold linker only after every source record and signal patch record
    // for this allocation exists. Linked state is never restored from disk.
    for (const auto& block : unit.blocks) {
        (void)module->PublishLinkTarget(ir::Location{block.guest_start},
                                        buffer.exec_data + block.code_offset,
                                        buffer.exec_data);
        address_space.PushCodeCache(ir::Location{block.guest_start},
                                    buffer.exec_data + block.code_offset);
        if (!module->GetModuleConfig().read_only) {
            address_space.GetSmcTracker().RegisterNode(
                    module, node, block.guest_start, block.guest_end);
        }
    }
    // Carry the entry forward: Save() writes `units`, so without this a warm
    // run would persist only what it *re-compiled* and the cache would shrink
    // to nothing over successive runs. The stored bytes and relocations are
    // the originals (ApplyRelocations patched the code cache copy, not
    // unit.code), so re-persisting them verbatim stays self-consistent.
    {
        std::lock_guard guard(lock);
        units[unit.guest_start] = unit;
    }
    stats.units_loaded.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void JitDiskCache::Load(const std::shared_ptr<Module>& module) {
    PerfScope2 perf_load{GetPerfStats2().cache_load};
    if (!enabled || !module) {
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
    auto& table = address_space.GetCodeCacheTable();
    for (const auto& [slot_key, index] : slots) {
        if (!table.PutAt(index, slot_key)) {
            LOG_WARNING("SVM_JIT_CACHE: dispatch slot replay conflict; cache ignored");
            table.Clear();
            stats.reject_header.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    stats.dispatch_slots.store(slots.size(), std::memory_order_relaxed);

    u64 unit_count{};
    if (!reader.U64(unit_count)) {
        table.Clear();
        stats.reject_header.fetch_add(1, std::memory_order_relaxed);
        return;
    }
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
        ReviveUnit(module, unit);
    }
}

}  // namespace swift::runtime::backend
