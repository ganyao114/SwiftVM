#include "hir_storage.h"

#include <new>
#include <utility>
#include "runtime/ir/hir_pools.h"

namespace swift::runtime::ir {
struct HIRPoolCache {
    std::atomic<HIRPools*> idle{};
    ~HIRPoolCache() { delete idle.load(std::memory_order_relaxed); }
};

namespace {
thread_local std::shared_ptr<HIRPoolCache> thread_cache;
}

HIRPools::HIRPools(u32 function_capacity)
        : functions(function_capacity)
        , blocks(function_capacity * 8)
        , values(function_capacity * 256)
        , edges(function_capacity * 16)
        , uses(function_capacity * 256)
        , mem_arena(function_capacity * 512) {}

HIRPools::~HIRPools() {
    ASSERT(references.load(std::memory_order_relaxed) == 0);
    // HIRFunction checks its synthetic entry during destruction. Release it
    // before HIRBlock storage, including when construction or cache reset failed.
    functions.DestroyContents();
    blocks.DestroyContents();
}

void HIRPools::Reset() {
    ASSERT(references.load(std::memory_order_relaxed) == 0);
    functions.ReleaseContents();
    blocks.ReleaseContents();
    values.ReleaseContents();
    edges.ReleaseContents();
    uses.ReleaseContents();
    mem_arena.Reset();
}

HIRPoolLease::HIRPoolLease(u32 function_capacity) {
    if (!thread_cache) thread_cache = std::make_shared<HIRPoolCache>();
    std::unique_ptr<HIRPools> storage{
            thread_cache->idle.exchange(nullptr, std::memory_order_acq_rel)};
    if (!storage) {
        storage = std::make_unique<HIRPools>(function_capacity);
        storage->cache = thread_cache;
    }
    ASSERT(storage->references.load(std::memory_order_relaxed) == 0);
    storage->references.store(1, std::memory_order_relaxed);
    pools = storage.release();
}

HIRPoolLease HIRPoolLease::Retain(HIRPools& storage) {
    // Raw stack pools are useful for construction tests but cannot back an
    // escaping prepared result. Compilation inputs come from a builder lease.
    ASSERT(storage.references.load(std::memory_order_relaxed) != 0);
    storage.references.fetch_add(1, std::memory_order_relaxed);
    return HIRPoolLease{&storage};
}

HIRPoolLease::HIRPoolLease(const HIRPoolLease& other) : pools(other.pools) {
    if (pools) pools->references.fetch_add(1, std::memory_order_relaxed);
}

HIRPoolLease& HIRPoolLease::operator=(const HIRPoolLease& other) {
    if (this != &other) {
        HIRPoolLease copy{other};
        std::swap(pools, copy.pools);
    }
    return *this;
}

HIRPoolLease::HIRPoolLease(HIRPoolLease&& other) noexcept
        : pools(std::exchange(other.pools, nullptr)) {}

HIRPoolLease& HIRPoolLease::operator=(HIRPoolLease&& other) noexcept {
    if (this != &other) {
        Reset();
        pools = std::exchange(other.pools, nullptr);
    }
    return *this;
}

HIRPoolLease::~HIRPoolLease() { Reset(); }

void HIRPoolLease::Reset() noexcept {
    auto* storage = std::exchange(pools, nullptr);
    if (!storage || storage->references.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    if (auto cache = storage->cache.lock()) {
        try {
            storage->Reset();
        } catch (const std::bad_alloc&) {
            // Capacity maintenance is optional; teardown must not allocate.
            delete storage;
            return;
        }
        delete cache->idle.exchange(storage, std::memory_order_acq_rel);
    } else {
        delete storage;
    }
}

HIRPools& HIRPoolLease::Get() const {
    ASSERT(pools);
    return *pools;
}
}  // namespace swift::runtime::ir
