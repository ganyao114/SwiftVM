#pragma once

#include <atomic>
#include <thread>
#include "runtime/common/types.h"

namespace swift::runtime::backend {

// AArch64 exclusive accesses require natural alignment, while x86 LOCK does
// not.  JITed and interpreted misaligned RMWs serialize through this same
// process-wide lock and perform their memory access with basic load/store.
inline constinit std::atomic<u32> unaligned_atomic_lock{0};
inline constinit thread_local std::atomic<bool> owns_unaligned_atomic_guard{false};
static_assert(std::atomic<bool>::is_always_lock_free);

// RV64 fault recovery can abandon a semantic helper's C++ frame. Only the
// interrupted thread's guard may be released; an unrelated thread's lock
// must never be cleared. SMC retry handlers do not call this function.
inline void ReleaseAbandonedUnalignedAtomicGuard() {
    if (owns_unaligned_atomic_guard.exchange(false, std::memory_order_relaxed))
        unaligned_atomic_lock.store(0, std::memory_order_release);
}

class UnalignedAtomicGuard {
public:
    UnalignedAtomicGuard() {
        u32 expected = 0;
        while (!unaligned_atomic_lock.compare_exchange_weak(expected,
                                                            1,
                                                            std::memory_order_acquire,
                                                            std::memory_order_relaxed)) {
            expected = 0;
            std::this_thread::yield();
        }
        owns_unaligned_atomic_guard.store(true, std::memory_order_relaxed);
    }

    ~UnalignedAtomicGuard() {
        owns_unaligned_atomic_guard.store(false, std::memory_order_relaxed);
        unaligned_atomic_lock.store(0, std::memory_order_release);
    }

    UnalignedAtomicGuard(const UnalignedAtomicGuard&) = delete;
    UnalignedAtomicGuard& operator=(const UnalignedAtomicGuard&) = delete;
};

}  // namespace swift::runtime::backend
