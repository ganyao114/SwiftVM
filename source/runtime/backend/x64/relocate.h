#pragma once
#include "compile.h"

namespace swift::runtime::backend::x64 {
HostSymbol CallbackSymbol(std::uintptr_t) noexcept;
std::uintptr_t ResolveSymbol(HostSymbol, const CompileOptions&) noexcept;
bool Relocate(std::span<std::uint8_t>, std::span<const Relocation>, const CompileOptions&) noexcept;
}  // namespace swift::runtime::backend::x64
