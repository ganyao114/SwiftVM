#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "runtime/backend/fault_entry.h"

namespace swift::runtime::backend {

// Host ownership is separate from Module and AddressSpace. Signal readers never
// dereference a Module, acquire a mutex, allocate or modify a shared_ptr.
class FaultCatalog {
public:
    class Reader {
    public:
        explicit Reader(const FaultCatalog& catalog) noexcept;
        ~Reader();
        Reader(const Reader&) = delete;
        Reader& operator=(const Reader&) = delete;

        [[nodiscard]] bool Lookup(const u8* host_pc, FaultEntry& out,
                                  const void* module = nullptr) const noexcept;
        [[nodiscard]] bool LookupAllocation(const void* module, const u8* host_pc,
                                            FaultEntry& out,
                                            std::vector<FaultEntry>* entries) const;

    private:
        const FaultCatalog& catalog;
    };

    struct Stats {
        size_t allocations{};
        size_t entries{};
        size_t readers{};
        size_t retired_snapshots{};
        size_t retired_allocations{};
    };

    FaultCatalog() = default;
    ~FaultCatalog();
    FaultCatalog(const FaultCatalog&) = delete;
    FaultCatalog& operator=(const FaultCatalog&) = delete;

    void Add(const void* module, const FaultEntry& entry);
    // Removal only unlinks preallocated records; it cannot fail allocating a
    // replacement snapshot after the caller starts executable/IR reclamation.
    void RemoveAllocation(const void* module, const u8* allocation);
    void RemoveModule(const void* module);
    // Terminal-only: revoke lookup/publication before observing reader grace.
    // A false result retains retired records; retry after the readers leave.
    [[nodiscard]] bool SealAndRevoke();
    void Collect();
    [[nodiscard]] Stats GetStats() const;
    [[nodiscard]] bool Lookup(const u8* host_pc, FaultEntry& out,
                              const void* module = nullptr) const noexcept;

private:
    struct Snapshot {
        std::vector<FaultEntry> entries;
        Snapshot* retired_next{};
    };
    struct Allocation {
        const void* module{};
        const u8* owner{};
        std::atomic<Allocation*> next{};
        std::atomic<Snapshot*> snapshot{};
        Allocation* retired_next{};
    };

    void Retire(Snapshot* snapshot);
    void Retire(Allocation* allocation);
    void CollectLocked();
    void RemoveLocked(const void* module, const u8* owner, bool all);

    static_assert(std::atomic<Allocation*>::is_always_lock_free);
    static_assert(std::atomic<Snapshot*>::is_always_lock_free);
    static_assert(std::atomic<size_t>::is_always_lock_free);
    mutable std::atomic<size_t> readers{};
    std::atomic<Allocation*> head{};
    mutable std::mutex writer_mutex;
    bool sealed{};
    Snapshot* retired_snapshots{};
    Allocation* retired_allocations{};
};

} // namespace swift::runtime::backend
