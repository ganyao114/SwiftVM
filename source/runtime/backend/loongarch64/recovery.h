#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include "runtime/backend/code.h"

namespace swift::runtime::execution {
struct FaultContext;
}

namespace swift::runtime::backend::loongarch64 {
inline constexpr std::uint8_t LbtRecoveryLive = 1u << 0;
inline constexpr std::uint8_t LbtRecoveryCarryXor = 1u << 1;

void RestoreGuestState(execution::FaultContext&,
                       std::span<const std::byte> gprs,
                       std::uint8_t lbt_recovery = 0,
                       std::uint8_t lbt_dirty_mask = 0,
                       std::uint32_t lbt_eflags = 0) noexcept;
}  // namespace swift::runtime::backend::loongarch64
