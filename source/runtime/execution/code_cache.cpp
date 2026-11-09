#include "code_cache.h"

#include <utility>
#include "base/error.h"

namespace swift::runtime::execution {
CodeCache::CodeCache(NativeCode::Function pending, const HostBackendOps& backend) noexcept
        : backend(backend), empty{.entry = pending} {}

NativeCode::Function CodeCache::Find(std::uint64_t pc, NativeCode* unit) const noexcept {
    if (slots.empty()) return {};
    auto at = Index(pc, slots.size() - 1);
    while (slots[at].occupied) {
        if (slots[at].pc == pc) {
            if (slots[at].code_id == UINT32_MAX) return {};
            if (unit) *unit = codes[slots[at].code_id];
            return slots[at].entry;
        }
        at = (at + 1) & (slots.size() - 1);
    }
    return {};
}

const NativeCode* CodeCache::FindHost(std::uintptr_t pc) const noexcept {
    // Exceptional-only borrow. The one guest is not executing while these
    // vectors grow, and retirement waits until its active borrow has ended.
    for (const auto& code : codes) {
        const auto base = reinterpret_cast<std::uintptr_t>(code.entry);
        if (pc >= base && pc - base < code.bytes) return &code;
    }
    return nullptr;
}

bool CodeCache::DeferCompilation(std::uint64_t pc, std::uint16_t threshold) {
    auto& slot = Reserve(pc);
    if (slot.jit_disabled) return true;
    if (slot.cold_entries < threshold) {
        ++slot.cold_entries;
        return true;
    }
    return false;
}

void CodeCache::DisableCompilation(std::uint64_t pc) { Reserve(pc).jit_disabled = true; }

void CodeCache::InsertReady(std::uint64_t pc,
                            NativeCode code,
                            std::span<const backend::EntryPoint> extra) {
    if (!code.entry) swift::ThrowError("cannot cache an empty native entry");
    if (codes.size() >= UINT32_MAX) throw std::length_error("native unit capacity overflow");
    const auto capacity = codes.capacity();
    const auto id = static_cast<std::uint32_t>(codes.size());
    // The minimum PC directory admits four entries before growing. Allocate
    // its first native rows together, only when a unit is actually published.
    if (!capacity) codes.reserve(4);
    codes.push_back(code);
    if (capacity != codes.capacity()) ++allocations;
    // Publication may have partially registered entries if a later allocation
    // fails. The VM aborts this resolver; it must revoke before heap release.
    BindReady(pc, id, code.entry);
    for (const auto& entry : extra) {
        if (entry.offset >= code.bytes) swift::ThrowError("entry outside native unit");
        if (entry.pc == pc) continue;  // The unit's canonical entry is already bound above.
        BindReady(entry.pc,
                  id,
                  reinterpret_cast<NativeCode::Function>(
                          reinterpret_cast<std::uintptr_t>(code.entry) + entry.offset));
    }
}

void CodeCache::BindReady(std::uint64_t pc, std::uint32_t id, NativeCode::Function entry) {
    auto& slot = Reserve(pc);
    if (slot.code_id != UINT32_MAX) return;  // First ready entry owns this PC.
    slot.code_id = id;
    slot.entry = entry;
    ++entries;
    auto head = slot.pending_head;
    slot.pending_head = UINT32_MAX;
    if (head != UINT32_MAX) --pending_targets;
    while (head != UINT32_MAX) {
        const auto& link = links[head];
        Patch(link, entry);
        --pending_count;
        head = link.next;
    }
    const auto home = Index(pc, slots.size() - 1);
    if (&slot != &slots[home] && slots[home].occupied && slots[home].code_id == UINT32_MAX)
        std::swap(slot, slots[home]);
}

std::size_t CodeCache::Index(std::uint64_t pc, std::size_t mask) noexcept {
    return DirectoryIndex(pc, mask);
}

std::size_t CodeCache::EmptyOrMatching(const std::vector<DispatchSlot>& table,
                                       std::uint64_t pc) noexcept {
    auto at = Index(pc, table.size() - 1);
    while (table[at].occupied && table[at].pc != pc) at = (at + 1) & (table.size() - 1);
    return at;
}

DispatchSlot& CodeCache::Reserve(std::uint64_t pc) {
    std::size_t at{};
    if (!slots.empty()) {
        at = EmptyOrMatching(slots, pc);
        if (slots[at].occupied) return slots[at];
    }
    if (slots.empty() || occupied == slots.size() / 4) {
        Grow();
        at = EmptyOrMatching(slots, pc);
    }
    auto& slot = slots[at];
    slot.pc = pc;
    slot.occupied = true;
    ++occupied;
    return slot;
}

void CodeCache::Grow() {
    if (slots.size() > slots.max_size() / 2) throw std::length_error("native PC capacity overflow");
    std::vector<DispatchSlot> next(slots.empty() ? 32 : slots.size() * 2, empty);
    for (const auto& slot : slots)
        if (slot.occupied) {
            const auto at = EmptyOrMatching(next, slot.pc);
            next[at] = slot;
            if (slot.code_id != UINT32_MAX) {
                const auto home = Index(slot.pc, next.size() - 1);
                if (at != home && next[home].occupied && next[home].code_id == UINT32_MAX)
                    std::swap(next[at], next[home]);
            }
        }
    slots.swap(next);
    ++allocations;
}

void CodeCache::Clear() noexcept {
    for (auto& slot : slots) slot = empty;
    codes.clear();
    links.clear();
    entries = occupied = pending_count = pending_targets = 0;
}

void CodeCache::Release() noexcept {
    std::vector<DispatchSlot>{}.swap(slots);
    std::vector<NativeCode>{}.swap(codes);
    std::vector<Relocation>{}.swap(links);
    entries = occupied = pending_count = pending_targets = 0;
}
}  // namespace swift::runtime::execution
