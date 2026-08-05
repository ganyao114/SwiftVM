//
// JIT disk cache: persist compiled host code across runs of the same guest.
//
// Off by default. `SVM_JIT_CACHE=<dir>` enables it; `SVM_JIT_CACHE_STATS=1`
// prints the counters at exit. It replaces behaviour that is already correct,
// so unlike the SVM_SSE4-class escape hatches it must be opted into, and every
// failure path (bad header, corrupt file, stale guest bytes, unrelocatable
// unit) silently degrades to normal JIT.
//
// Granularity: one file per (guest identity x validity key), holding one
// entry per compiled unit (a function in function mode, a block otherwise).
// Written once at AddressSpace teardown. The driver reads it once after all
// modules have been mapped, so each cached unit can be assigned to its current
// module before the load-time guest byte check and revive.
//
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "runtime/backend/code_serial.h"
#include "runtime/common/types.h"

namespace swift::runtime::backend {

class AddressSpace;
class Module;

struct JitCacheStats {
    std::atomic<u64> units_stored{};       // units written at exit
    std::atomic<u64> units_loaded{};       // units revived from disk
    std::atomic<u64> units_compiled{};     // units the JIT compiled this run
    std::atomic<u64> reject_header{};      // whole-file rejections
    std::atomic<u64> reject_guest_bytes{}; // per-unit: guest code changed
    std::atomic<u64> feature_match{};      // per-unit: current module feature match
    std::atomic<u64> reject_feature{};     // per-unit: current module feature mismatch
    std::atomic<u64> reject_reloc{};       // per-unit: relocation/audit failure
    std::atomic<u64> reject_alloc{};       // per-unit: no code cache room
    std::atomic<u64> reject_scan{};        // units the scanner refused to store
    std::atomic<u64> dispatch_slots{};     // L2 slot assignments replayed
};

class JitDiskCache {
public:
    explicit JitDiskCache(AddressSpace& space);
    ~JitDiskCache();

    // True when SVM_JIT_CACHE names a usable directory.
    [[nodiscard]] static bool Requested();
    [[nodiscard]] bool Enabled() const { return enabled; }

    // 在 driver 完成 MapModule 后调用一次。每个 unit 按 guest 地址重新查询
    // 当前 module，并校验其 resolved FeatureSet hash。
    void Load();

    // Called from TranslateIR right after a unit has been published. `blocks`
    // holds one entry per decoded guest block with its offset inside the unit.
    void RecordUnit(const std::shared_ptr<Module>& module,
                    VAddr guest_start,
                    bool is_function,
                    const u8* exec_data,
                    const u8* rw_data,
                    u32 code_size,
                    const std::vector<SerialBlock>& blocks,
                    const std::vector<SerialLinkSite>& link_sites);

    // Flush to disk (atomic rename). Called from ~AddressSpace.
    void Save();

    [[nodiscard]] JitCacheStats& Stats() { return stats; }

private:
    [[nodiscard]] std::string FilePath() const;
    [[nodiscard]] ValidityKey Key() const;
    // Hash of the guest bytes covering [start, end). Returns false when the
    // range is not addressable through the guest bias.
    [[nodiscard]] bool HashGuestRange(VAddr start, VAddr end, u64& out) const;
    bool ReviveUnit(const std::shared_ptr<Module>& module, const SerialUnit& unit);

    AddressSpace& address_space;
    HostImageInfo host_image;
    std::string dir;
    bool enabled{};
    bool print_stats{};
    std::atomic_bool load_attempted{};
    // Set when this run has something the file does not already contain. A
    // fully warm run leaves it false and skips the write entirely.
    bool dirty{};
    std::mutex lock;
    // Units to persist, keyed by guest start so a re-compiled location
    // replaces its predecessor rather than duplicating it.
    std::unordered_map<u64, SerialUnit> units;
    // L2 dispatch slot assignments observed in this run, replayed verbatim on
    // load so the dispatch indices baked into cached code stay valid.
    std::vector<std::pair<u64, u32>> dispatch_slots;
    JitCacheStats stats;
};

}  // namespace swift::runtime::backend
