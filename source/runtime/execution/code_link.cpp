#include "code_cache.h"

#include <stdexcept>

namespace swift::runtime::execution {
void CodeCache::AddStaticLink(const NativeCode& owner,
                              const backend::LinkSite& site,
                              NativeCode::Function prepared_target) {
    if (!owner.writable) return;  // Single mapping keeps its original cold link.
    if (links.size() >= UINT32_MAX) throw std::length_error("native relocation capacity overflow");
    if (!links.capacity()) links.reserve(4);
    auto* writable = reinterpret_cast<std::uintptr_t*>(owner.writable + site.data_offset);
    const auto* executable = reinterpret_cast<const std::uintptr_t*>(
            reinterpret_cast<const std::uint8_t*>(owner.entry) + site.data_offset);
    const auto back = site.data_offset - site.branch_offset;
    if (prepared_target) {
        links.push_back({writable, executable, UINT32_MAX, back});
        ++patched;
        return;
    }
    auto& slot = Reserve(site.target_pc);
    if (slot.code_id != UINT32_MAX) {
        links.push_back({writable, executable, UINT32_MAX, back});
        Patch(links.back(), slot.entry);
        return;
    }
    links.push_back({writable, executable, slot.pending_head, back});
    if (slot.pending_head == UINT32_MAX) ++pending_targets;
    slot.pending_head = static_cast<std::uint32_t>(links.size() - 1);
    ++pending_count;
}

void CodeCache::Patch(const Relocation& link, NativeCode::Function entry) noexcept {
    const auto target = reinterpret_cast<std::uintptr_t>(entry);
    if (__atomic_load_n(link.writable, __ATOMIC_RELAXED) != target)
        backend.patch_link(link.writable, link.executable, target, link.branch_back_bytes);
    ++patched;
}

void CodeCache::ResetLinks() noexcept {
    for (const auto& link : links)
        backend.unlink(link.writable, link.executable, link.branch_back_bytes);
}
}  // namespace swift::runtime::execution
