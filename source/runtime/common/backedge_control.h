#pragma once

#include "runtime/common/types.h"

namespace swift::runtime {

constexpr u64 kBackedgeSignalRequest = u64{1} << 63;
constexpr u64 kBackedgeSmcRequestMask = ~kBackedgeSignalRequest;

[[nodiscard]] bool BackedgeLatchEnabled();
[[nodiscard]] bool BackedgeFlagsEnabled();

}  // namespace swift::runtime
