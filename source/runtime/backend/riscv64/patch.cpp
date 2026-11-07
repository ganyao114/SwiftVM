#include "patch.h"

#include <cstring>

namespace swift::runtime::backend::riscv64 {

bool InitializeNativeLink(std::span<std::uint8_t> writable,
                          std::uintptr_t executable,
                          const backend::LinkSite& site,
                          std::uintptr_t ready_target,
                          bool dual) noexcept {
    static_assert(sizeof(std::uintptr_t) == 8);
    const auto branch = std::size_t{site.branch_offset};
    // AUIPC/LD/FENCE/JR is 16B, followed by two 8B DATA slots and a
    // cold trampoline of at least LI/JR. Validate that actual resource once.
    if (site.kind != LinkKind::Static || (branch & 7) || (executable & 7) ||
        (reinterpret_cast<std::uintptr_t>(writable.data()) & 7) || branch > writable.size() ||
        writable.size() - branch < 40 || site.data_offset != branch + 16 ||
        site.fallback_offset != branch + 32)
        return false;
    const auto fallback = executable + site.fallback_offset;
    const std::uintptr_t slots[]{dual && ready_target ? ready_target : fallback, fallback};
    std::memcpy(writable.data() + site.data_offset, slots, sizeof slots);
    return true;
}

void PatchNativeLink(std::uintptr_t* writable,
                     const std::uintptr_t* executable,
                     std::uintptr_t target,
                     NativeOffset back_bytes) noexcept {
    (void)executable;
    (void)back_bytes;
    __atomic_store_n(writable, target, __ATOMIC_RELEASE);
}

void UnlinkNativeLink(std::uintptr_t* writable,
                      const std::uintptr_t* executable,
                      NativeOffset back_bytes) noexcept {
    (void)executable;
    (void)back_bytes;
    const auto fallback = __atomic_load_n(writable + 1, __ATOMIC_RELAXED);
    __atomic_store_n(writable, fallback, __ATOMIC_RELEASE);
}

}  // namespace swift::runtime::backend::riscv64
