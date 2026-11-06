#include "code_cache.h"

#include <stdexcept>

namespace swift::runtime::execution {
void CodeCache::AddStaticLink(const NativeCode& owner, const backend::LinkSite& site) {
    if (!owner.writable) return;  // Single mapping keeps its original cold link.
    auto& slot = Reserve(site.target_pc);
    if (links.size() >= UINT32_MAX) throw std::length_error("native relocation capacity overflow");
    auto* writable = reinterpret_cast<std::uintptr_t*>(owner.writable + site.data_offset);
    const auto* executable = reinterpret_cast<const std::uintptr_t*>(
            reinterpret_cast<const std::uint8_t*>(owner.entry) + site.data_offset);
    const auto back = site.data_offset - site.branch_offset;
    if (slot.code_id != UINT32_MAX) {
        links.push_back({writable, executable, UINT32_MAX, back, backend::LinkKind::Static});
        if (__atomic_load_n(writable, __ATOMIC_RELAXED) ==
            reinterpret_cast<std::uintptr_t>(slot.entry))
            ++patched;
        else
            Patch(links.back(), slot.entry);
        return;
    }
    links.push_back({writable, executable, slot.pending_head, back, backend::LinkKind::Static});
    if (slot.pending_head == UINT32_MAX) ++pending_targets;
    slot.pending_head = static_cast<std::uint32_t>(links.size() - 1);
    ++pending_count;
}
void CodeCache::AddIndirectSite(const NativeCode& owner, const backend::LinkSite& site) {
    if (!owner.writable) return;
    links.push_back(
            {reinterpret_cast<std::uintptr_t*>(owner.writable + site.data_offset),
             reinterpret_cast<const std::uintptr_t*>(
                     reinterpret_cast<const std::uint8_t*>(owner.entry) + site.fallback_offset),
             UINT32_MAX,
             0,
             backend::LinkKind::Dynamic});
}
void CodeCache::Patch(const Relocation& link, NativeCode::Function entry) noexcept {
    backend.patch_link(link.writable,
                       link.executable,
                       reinterpret_cast<std::uintptr_t>(entry),
                       link.branch_back_bytes);
    ++patched;
}
void CodeCache::ResetLinks() noexcept {
    for (const auto& link : links) {
        if (link.kind == backend::LinkKind::Static)
            backend.unlink(link.writable, link.executable, link.branch_back_bytes);
        else
            backend.reset_indirect(link.writable, link.executable);
    }
}
}  // namespace swift::runtime::execution
