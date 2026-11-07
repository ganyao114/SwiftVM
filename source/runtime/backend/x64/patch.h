#pragma once
#include "runtime/backend/code.h"

namespace swift::runtime::backend::x64 {
void SynchronizeCode(std::uint8_t*, const std::uint8_t*, std::size_t) noexcept;
bool InitializeNativeLink(
        std::span<std::uint8_t>, std::uintptr_t, const LinkSite&, std::uintptr_t, bool) noexcept;
void PatchNativeLink(std::uintptr_t*, const std::uintptr_t*, std::uintptr_t, NativeOffset) noexcept;
void UnlinkNativeLink(std::uintptr_t*, const std::uintptr_t*, NativeOffset) noexcept;
}  // namespace swift::runtime::backend::x64
