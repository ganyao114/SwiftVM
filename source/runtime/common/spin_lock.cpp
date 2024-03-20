#include <thread>
#include "spin_lock.h"

#if _MSC_VER
#include <intrin.h>
#if _M_AMD64
#define __x86_64__ 1
#endif
#if _M_ARM64
#define __aarch64__ 1
#endif
#else
#if __x86_64__
#include <xmmintrin.h>
#endif
#endif

namespace {

void ThreadPause() {
#if __x86_64__
    _mm_pause();
#elif __aarch64__ && _MSC_VER
    __yield();
#elif __aarch64__
    asm("yield");
#else
    std::this_thread::yield();
#endif
}

} // Anonymous namespace

namespace swift::runtime {

void SpinLock::lock() {
    while (lck.test_and_set(std::memory_order_acquire)) {
        ThreadPause();
    }
}

void SpinLock::unlock() {
    lck.clear(std::memory_order_release);
}

bool SpinLock::try_lock() {
    if (lck.test_and_set(std::memory_order_acquire)) {
        return false;
    }
    return true;
}

void RwSpinLock::lock() {
    while (writer.exchange(true, std::memory_order_acquire)) {
        ThreadPause();
    }
    while (reader_count.load(std::memory_order_relaxed) != 0) {
        ThreadPause();
    }
}

void RwSpinLock::unlock() {
    writer.store(false, std::memory_order_release);
}

bool RwSpinLock::try_lock() {
    if (std::atomic_exchange_explicit(&writer, true, std::memory_order_acquire)) {
        return false;
    }
    return true;
}

void RwSpinLock::lock_shared() {
    while (true) {
        while (writer.load(std::memory_order_relaxed)) {
            ThreadPause();
        }
        reader_count.fetch_add(1, std::memory_order_acquire);
        if (!writer.load(std::memory_order_relaxed)) {
            break;
        }
        reader_count.fetch_sub(1, std::memory_order_release);
        ThreadPause();
    }
}

bool RwSpinLock::try_lock_shared() {
    if (writer.load(std::memory_order_relaxed)) {
        return false;
    }
    reader_count.fetch_add(1, std::memory_order_acquire);
    if (!writer.load(std::memory_order_relaxed)) {
        return true;
    }
    reader_count.fetch_sub(1, std::memory_order_release);
    return false;
}

void RwSpinLock::unlock_shared() {
    reader_count.fetch_sub(1, std::memory_order_release);
}

} // namespace swift::runtime
