#pragma once

#include <cstdint>
#include <span>

#include "runtime/backend/relocation.h"

namespace swift::runtime::backend::arm64 {
struct CompileOptions;

HostSymbol CallbackSymbol(std::uintptr_t) noexcept;
std::uintptr_t ResolveSymbol(HostSymbol, const CompileOptions&) noexcept;
bool Relocate(std::span<std::uint8_t>, std::span<const Relocation>, const CompileOptions&) noexcept;

namespace detail {
// Private getters keep anonymous numerical adapters in their original TU.
std::uintptr_t ImplicitStringAddress() noexcept;
std::uintptr_t ExplicitStringAddress() noexcept;
std::uintptr_t CryptoAddress() noexcept;
std::uintptr_t DepositBitsAddress() noexcept;
std::uintptr_t ExtractBitsAddress() noexcept;
}  // namespace detail
}  // namespace swift::runtime::backend::arm64
