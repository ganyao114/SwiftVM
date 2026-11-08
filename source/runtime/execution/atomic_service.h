#pragma once

#include "runtime/atomic.h"

namespace swift::runtime::execution {
class GuestMemory;
class Smc;

// Call under FaultScope's process-wide execution ownership. Software commits
// reuse that lock, excluding ordinary JIT/direct accesses and smaller atomics;
// they require private stable pages. Shared containers use real hardware CAS.
// Qualification precedes SMC preparation and every possible guest write.
AtomicStatus ExecuteAtomic(GuestMemory&, Smc&, AtomicCall&) noexcept;
}  // namespace swift::runtime::execution
