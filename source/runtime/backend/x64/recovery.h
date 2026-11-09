#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include "runtime/backend/code.h"

namespace swift::runtime::execution {
struct FaultContext;
}

namespace swift::runtime::backend::x64 {
bool RestoreGuestState(execution::FaultContext&,
                       const FaultSite&,
                       std::span<const std::byte> gprs) noexcept;
}  // namespace swift::runtime::backend::x64
