#pragma once
#include "runtime/backend/context.h"

namespace swift::runtime::backend::loongarch64 {
// Shared reader protocol also covers the architecture-neutral unaligned
// atomic helpers. These functions never let exceptions cross generated code.
void AcquireMemory(State* state) noexcept;
void ReleaseMemory(State* state) noexcept;
u64 GuestAddress(State* state, u64 guest, u64 size) noexcept;
}  // namespace swift::runtime::backend::loongarch64
