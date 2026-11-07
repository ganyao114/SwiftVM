#include "patch.h"
#include <cstring>
#include <atomic>

namespace swift::runtime::backend::x64 {
void SynchronizeCode(std::uint8_t*, const std::uint8_t*, std::size_t) noexcept {
    std::atomic_thread_fence(std::memory_order_release);
}

bool InitializeNativeLink(std::span<std::uint8_t> w,
                          std::uintptr_t rx,
                          const LinkSite& s,
                          std::uintptr_t ready,
                          bool dual) noexcept {
    if (s.kind != LinkKind::Static || (s.data_offset & 7) || s.data_offset > w.size() ||
        w.size() - s.data_offset < 16 || s.branch_offset > s.data_offset ||
        s.data_offset - s.branch_offset != 8 || s.fallback_offset != s.data_offset + 16 ||
        s.fallback_offset > w.size() || w.size() - s.fallback_offset < 13)
        return false;
    const auto off = s.branch_offset;
    if (w[off] != 0xff || w[off + 1] != 0x25) return false;
    std::int32_t displacement;
    std::memcpy(&displacement, w.data() + off + 2, 4);
    if (displacement != 2) return false;
    const std::uintptr_t cold = rx + s.fallback_offset, slots[]{dual && ready ? ready : cold, cold};
    std::memcpy(w.data() + s.data_offset, slots, sizeof slots);
    return true;
}

void PatchNativeLink(std::uintptr_t* w,
                     const std::uintptr_t*,
                     std::uintptr_t target,
                     NativeOffset) noexcept {
    __atomic_store_n(w, target, __ATOMIC_RELEASE);
}

void UnlinkNativeLink(std::uintptr_t* w, const std::uintptr_t*, NativeOffset) noexcept {
    __atomic_store_n(w, __atomic_load_n(w + 1, __ATOMIC_RELAXED), __ATOMIC_RELEASE);
}
}  // namespace swift::runtime::backend::x64
