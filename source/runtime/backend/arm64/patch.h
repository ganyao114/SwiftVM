#pragma once

#include "runtime/backend/code.h"

namespace swift::runtime::backend::arm64 {

// The same bytes through writable and executable aliases. Call only after
// the final stores, before publishing/resuming execution of this range.
void SynchronizeCode(std::uint8_t* writable,
                     const std::uint8_t* executable,
                     std::size_t bytes) noexcept;

// back_bytes is the actual DATA-to-transfer-head distance. Static links only.
void PatchNativeLink(std::uintptr_t* writable,
                     const std::uintptr_t* executable,
                     std::uintptr_t target,
                     NativeOffset back_bytes) noexcept;
void UnlinkNativeLink(std::uintptr_t* writable,
                      const std::uintptr_t* executable,
                      NativeOffset back_bytes) noexcept;
// Each real resource is initialized once before publication. writable_alias=0
// denotes the single-mapping fallback: static sites are never prelinked to
// code that could not later be unlinked.
bool InitializeNativeLink(std::span<std::uint8_t> write,
                          std::uintptr_t execute,
                          const backend::LinkSite&,
                          std::uintptr_t ready_target,
                          std::uintptr_t writable_alias) noexcept;
bool PatchOrderedFallback(const backend::NativeCode&,
                          NativeOffset fast,
                          NativeOffset fallback,
                          std::uint8_t* writable_instruction) noexcept;

}  // namespace swift::runtime::backend::arm64
