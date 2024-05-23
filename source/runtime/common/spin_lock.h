#pragma once

#include <atomic>
#include "base/types.h"

namespace swift::runtime {

#pragma pack(push, 1)
class SpinLock {
public:
    SpinLock() = default;

    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    SpinLock(SpinLock&&) = delete;
    SpinLock& operator=(SpinLock&&) = delete;

    void lock();
    void unlock();
    [[nodiscard]] bool try_lock();

private:
    std::atomic_flag lck = ATOMIC_FLAG_INIT;
};

class RwSpinLock {
public:
    RwSpinLock() = default;

    RwSpinLock(const RwSpinLock&) = delete;
    RwSpinLock& operator=(const RwSpinLock&) = delete;

    RwSpinLock(RwSpinLock&&) = delete;
    RwSpinLock& operator=(RwSpinLock&&) = delete;

    void lock();
    void unlock();
    [[nodiscard]] bool try_lock();
    void lock_shared();
    [[nodiscard]] bool try_lock_shared();
    void unlock_shared();

private:
    std::atomic<bool> writer{false};
    std::atomic<u16> reader_count{0};
};

class UseReference {
public:

    void UseObject();
    void UnUseObject();
    void UnUseObjectAndFree();
    void WaitUnUse();

private:
    std::atomic<u16> ref_count{0};
};
#pragma pack(pop)

} // namespace swift::runtime
