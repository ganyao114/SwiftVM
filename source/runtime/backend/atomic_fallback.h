#pragma once

#include <atomic>
#include <thread>
#include "runtime/common/types.h"

namespace swift::runtime::backend {

// AArch64 exclusive accesses require natural alignment, while x86 LOCK does
// not.  JITed and interpreted misaligned RMWs serialize through this same
// process-wide lock and perform their memory access with plain load/store.
inline constinit std::atomic<u32> unaligned_atomic_lock{0};

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
    }

    ~UnalignedAtomicGuard() {
        unaligned_atomic_lock.store(0, std::memory_order_release);
    }

    UnalignedAtomicGuard(const UnalignedAtomicGuard&) = delete;
    UnalignedAtomicGuard& operator=(const UnalignedAtomicGuard&) = delete;
};

}  // namespace swift::runtime::backend
