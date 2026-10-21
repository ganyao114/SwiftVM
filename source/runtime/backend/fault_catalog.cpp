#include "runtime/backend/fault_catalog.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace swift::runtime::backend {
namespace {
uintptr_t Address(const u8* pointer) {
    return reinterpret_cast<uintptr_t>(pointer);
}

const FaultEntry* BestEntry(const std::vector<FaultEntry>& entries, const u8* pc) {
    auto it = std::upper_bound(entries.begin(), entries.end(), pc,
                               [](const u8* point, const FaultEntry& entry) {
                                   return Address(point) < Address(entry.host_start);
                               });
    const FaultEntry* best{};
    while (it != entries.begin()) {
        --it;
        if (!it->Contains(pc)) continue;
        if (!best || Address(it->host_start) > Address(best->host_start) ||
            (it->host_start == best->host_start && it->recovery && !best->recovery)) {
            best = &*it;
        }
        if (best && Address(it->host_start) < Address(best->host_start)) break;
    }
    return best;
}
}

FaultCatalog::Reader::Reader(const FaultCatalog& catalog) noexcept : catalog(catalog) {
    // SC orders entry before pointer loads against writer publication and its
    // zero-reader check: a reader of any old pointer is visible to that check.
    catalog.readers.fetch_add(1, std::memory_order_seq_cst);
}

FaultCatalog::Reader::~Reader() {
    catalog.readers.fetch_sub(1, std::memory_order_seq_cst);
}

bool FaultCatalog::Reader::Lookup(const u8* host_pc, FaultEntry& out,
                                  const void* module) const noexcept {
    bool found{};
    for (auto* allocation = catalog.head.load(std::memory_order_seq_cst); allocation;
         allocation = allocation->next.load(std::memory_order_seq_cst)) {
        if (module && allocation->module != module) continue;
        const auto* snapshot = allocation->snapshot.load(std::memory_order_seq_cst);
        if (!snapshot) continue;
        const auto* entry = BestEntry(snapshot->entries, host_pc);
        if (entry && (!found || Address(entry->host_start) > Address(out.host_start) ||
                      (entry->host_start == out.host_start && entry->recovery && !out.recovery))) {
            out = *entry;
            found = true;
        }
    }
    return found;
}

bool FaultCatalog::Reader::LookupAllocation(const void* module, const u8* host_pc,
                                            FaultEntry& out,
                                            std::vector<FaultEntry>* entries) const {
    const Snapshot* selected{};
    const FaultEntry* selected_entry{};
    const u8* owner{};
    for (auto* allocation = catalog.head.load(std::memory_order_seq_cst); allocation;
         allocation = allocation->next.load(std::memory_order_seq_cst)) {
        if (allocation->module != module) continue;
        const auto* snapshot = allocation->snapshot.load(std::memory_order_seq_cst);
        if (!snapshot) continue;
        const auto* candidate = BestEntry(snapshot->entries, host_pc);
        if (candidate && (!selected_entry ||
                          Address(candidate->host_start) > Address(selected_entry->host_start))) {
            selected = snapshot;
            selected_entry = candidate;
            owner = allocation->owner;
        }
    }
    if (!selected) return false;
    const FaultEntry* whole{};
    for (const auto& entry : selected->entries) {
        if (entry.host_start == owner && entry.Contains(host_pc) &&
            (!whole || Address(entry.host_end) > Address(whole->host_end))) whole = &entry;
    }
    if (!whole) return false;
    out = *whole;
    if (entries) {
        entries->clear();
        for (const auto& entry : selected->entries) {
            if (Address(entry.host_start) >= Address(whole->host_start) &&
                Address(entry.host_start) < Address(whole->host_end)) entries->push_back(entry);
        }
    }
    return true;
}

bool FaultCatalog::Lookup(const u8* host_pc, FaultEntry& out, const void* module) const noexcept {
    return Reader{*this}.Lookup(host_pc, out, module);
}

void FaultCatalog::Retire(Snapshot* snapshot) {
    if (!snapshot) return;
    snapshot->retired_next = retired_snapshots;
    retired_snapshots = snapshot;
}

void FaultCatalog::Retire(Allocation* allocation) {
    Retire(allocation->snapshot.exchange(nullptr, std::memory_order_seq_cst));
    allocation->retired_next = retired_allocations;
    retired_allocations = allocation;
}

void FaultCatalog::CollectLocked() {
    if (readers.load(std::memory_order_seq_cst) != 0) return;
    while (retired_snapshots) {
        auto* snapshot = retired_snapshots;
        retired_snapshots = snapshot->retired_next;
        delete snapshot;
    }
    while (retired_allocations) {
        auto* allocation = retired_allocations;
        retired_allocations = allocation->retired_next;
        delete allocation;
    }
}

void FaultCatalog::Add(const void* module, const FaultEntry& entry) {
    assert(module && Address(entry.host_start) < Address(entry.host_end));
    std::lock_guard guard(writer_mutex);
    if (sealed) throw std::runtime_error("fault catalog is sealed");
    Allocation* found{};
    for (auto* allocation = head.load(std::memory_order_seq_cst); allocation;
         allocation = allocation->next.load(std::memory_order_seq_cst)) {
        if (allocation->module == module && allocation->owner == entry.owner_start) {
            found = allocation;
            break;
        }
    }
    auto snapshot = std::make_unique<Snapshot>();
    if (found) snapshot->entries = found->snapshot.load(std::memory_order_seq_cst)->entries;
    auto position = std::lower_bound(snapshot->entries.begin(), snapshot->entries.end(),
                                     entry.host_start, [](const FaultEntry& item, const u8* pc) {
                                         return Address(item.host_start) < Address(pc);
                                     });
    snapshot->entries.insert(position, entry);
    if (found) {
        Retire(found->snapshot.exchange(snapshot.release(), std::memory_order_seq_cst));
    } else {
        auto allocation = std::make_unique<Allocation>();
        allocation->module = module;
        allocation->owner = entry.owner_start;
        allocation->snapshot.store(snapshot.release(), std::memory_order_seq_cst);
        allocation->next.store(head.load(std::memory_order_seq_cst), std::memory_order_seq_cst);
        head.store(allocation.release(), std::memory_order_seq_cst);
    }
    CollectLocked();
}

void FaultCatalog::RemoveLocked(const void* module, const u8* owner, bool all) {
    Allocation* previous{};
    auto* allocation = head.load(std::memory_order_seq_cst);
    while (allocation) {
        auto* next = allocation->next.load(std::memory_order_seq_cst);
        if (allocation->module == module && (all || allocation->owner == owner)) {
            if (previous) previous->next.store(next, std::memory_order_seq_cst);
            else head.store(next, std::memory_order_seq_cst);
            Retire(allocation);
        } else {
            previous = allocation;
        }
        allocation = next;
    }
    CollectLocked();
}

void FaultCatalog::RemoveAllocation(const void* module, const u8* allocation) {
    std::lock_guard guard(writer_mutex);
    RemoveLocked(module, allocation, false);
}

void FaultCatalog::RemoveModule(const void* module) {
    std::lock_guard guard(writer_mutex);
    RemoveLocked(module, nullptr, true);
}

void FaultCatalog::Collect() {
    std::lock_guard guard(writer_mutex);
    CollectLocked();
}

bool FaultCatalog::SealAndRevoke() {
    std::lock_guard guard(writer_mutex);
    sealed = true;
    auto* allocation = head.exchange(nullptr, std::memory_order_seq_cst);
    while (allocation) {
        auto* next = allocation->next.load(std::memory_order_seq_cst);
        Retire(allocation);
        allocation = next;
    }
    CollectLocked();
    return readers.load(std::memory_order_seq_cst) == 0 &&
           !retired_snapshots && !retired_allocations;
}

FaultCatalog::Stats FaultCatalog::GetStats() const {
    std::lock_guard guard(writer_mutex);
    Stats result;
    result.readers = readers.load(std::memory_order_seq_cst);
    for (auto* allocation = head.load(std::memory_order_seq_cst); allocation;
         allocation = allocation->next.load(std::memory_order_seq_cst)) {
        ++result.allocations;
        result.entries += allocation->snapshot.load(std::memory_order_seq_cst)->entries.size();
    }
    for (auto* snapshot = retired_snapshots; snapshot; snapshot = snapshot->retired_next)
        ++result.retired_snapshots;
    for (auto* allocation = retired_allocations; allocation; allocation = allocation->retired_next)
        ++result.retired_allocations;
    return result;
}

FaultCatalog::~FaultCatalog() {
    assert(readers.load(std::memory_order_seq_cst) == 0);
    while (auto* allocation = head.load(std::memory_order_seq_cst)) {
        head.store(allocation->next.load(std::memory_order_seq_cst), std::memory_order_seq_cst);
        Retire(allocation);
    }
    CollectLocked();
}

} // namespace swift::runtime::backend
