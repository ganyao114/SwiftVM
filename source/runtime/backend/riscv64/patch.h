#pragma once

#include "runtime/backend/code.h"

namespace swift::runtime::backend::riscv64 {

// Static exits load one aligned target DATA word, then tail to that address.
// The second immutable word retains this unit's cold dispatcher trampoline.
bool InitializeNativeLink(std::span<std::uint8_t> writable,
                          std::uintptr_t executable,
                          const backend::LinkSite&,
                          std::uintptr_t ready_target,
                          bool dual) noexcept;

// Target replacement changes DATA only. CodeHeap has synchronized every
// target before publication; the native load is followed by an acquire fence.
void PatchNativeLink(std::uintptr_t* writable,
                     const std::uintptr_t* executable,
                     std::uintptr_t target,
                     NativeOffset back_bytes) noexcept;
void UnlinkNativeLink(std::uintptr_t* writable,
                      const std::uintptr_t* executable,
                      NativeOffset back_bytes) noexcept;

}  // namespace swift::runtime::backend::riscv64
