#pragma once

#include "runtime/common/types.h"

namespace swift::runtime {

constexpr u64 kBackedgeSignalRequest = u64{1} << 63;
constexpr u64 kRetirementExitRequest = u64{1} << 62;
constexpr u64 kBackedgeSmcRequestMask = ~(kBackedgeSignalRequest | kRetirementExitRequest);

[[nodiscard]] bool BackedgeLatchEnabled();
[[nodiscard]] bool BackedgeFlagsEnabled();

}  // namespace swift::runtime
