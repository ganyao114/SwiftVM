//
// See jit_cache.h.
//

#include "runtime/backend/jit_cache.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "runtime/backend/address_space.h"
#include "runtime/backend/module.h"
#include "runtime/common/logging.h"
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
    const auto& config = address_space.GetConfig();
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
    if (True(config.global_opts & Optimizations::DirectBlockLink) || config.static_program) {
        // Direct block links bake another buffer's address into the code; the
        // scanner would refuse every unit anyway.
        LOG_WARNING("SVM_JIT_CACHE: DirectBlockLink/static modules are unsupported; cache disabled");
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
                              const u8* code_bytes,
                              u32 code_size,
                              const std::vector<SerialBlock>& blocks) {
    stats.units_compiled.fetch_add(1, std::memory_order_relaxed);
    if (!enabled || !code_bytes || code_size == 0 || blocks.empty()) {
        return;
    }
    const u64 window =
            address_space.GetConfig().guest_addr_mask ? address_space.GetConfig().guest_addr_mask + 1
                                                      : 0;
    auto scan = ScanCodeUnit({code_bytes, code_size}, host_image, window);
    if (!scan.ok) {
        stats.reject_scan.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    SerialUnit unit{};
    unit.guest_start = guest_start;
    unit.is_function = is_function ? 1 : 0;
    unit.code.assign(code_bytes, code_bytes + code_size);
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

    // 2. Place the code.
    auto [idx, buffer] = module->AllocCodeCache(static_cast<u32>(unit.code.size()));
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
    module->AddFaultEntry(buffer.exec_data, buffer.exec_data + buffer.size, unit.guest_start);

    // 5. Make every block entry reachable from the dispatcher, and re-arm SMC.
    for (const auto& block : unit.blocks) {
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
