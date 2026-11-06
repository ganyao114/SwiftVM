#pragma once

#include "runtime/backend/code.h"

namespace swift::runtime::backend::arm64 {

// The same bytes through writable and executable aliases. Call only after
// the final stores, before publishing/resuming execution of this range.
void SynchronizeCode(std::uint8_t* writable,
                     const std::uint8_t* executable,
                     std::size_t bytes) noexcept;

struct FaultRecovery {
    std::uint8_t test_bits{}, test_register{}, parity_register{}, carry_direct{};
};
constexpr FaultRecovery DecodeFaultRecovery(const backend::FaultSite& site) noexcept {
    return {site.recovery[0], site.recovery[1], site.recovery[2], site.recovery[3]};
}

// back_bytes is the actual DATA-to-transfer-head distance. Static links only.
void PatchNativeLink(std::uintptr_t* writable,
                     const std::uintptr_t* executable,
                     std::uintptr_t target,
                     NativeOffset back_bytes) noexcept;
void UnlinkNativeLink(std::uintptr_t* writable,
                      const std::uintptr_t* executable,
                      NativeOffset back_bytes) noexcept;
void ResetIndirectSite(std::uintptr_t* writable_array, const std::uintptr_t* fallback) noexcept;
// Each real resource is initialized once before publication. writable_alias=0
// denotes the single-mapping fallback: its dynamic sites remain cold and its
// static sites are never prelinked to code that could not later be unlinked.
bool InitializeNativeLink(std::span<std::uint8_t> write,
                          std::uintptr_t execute,
                          const backend::LinkSite&,
                          std::uintptr_t ready_target,
                          std::uintptr_t writable_alias) noexcept;
bool PatchOrderedFallback(const backend::NativeCode&,
                          NativeOffset fast,
                          NativeOffset fallback,
                          std::uintptr_t page_size) noexcept;

}  // namespace swift::runtime::backend::arm64
