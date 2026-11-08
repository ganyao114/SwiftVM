#pragma once

#include <mutex>

namespace swift::runtime::execution {

// One process-wide execution domain. Hold across native/direct execution and
// its owner services so software atomics also exclude ordinary guest accesses.
// Recursive acquisition lets a software commit reuse its caller's ownership.
class GuestExecutionLock {
public:
    GuestExecutionLock();
    GuestExecutionLock(const GuestExecutionLock&) = delete;
    GuestExecutionLock& operator=(const GuestExecutionLock&) = delete;

private:
    std::lock_guard<std::recursive_mutex> lock;
};

}  // namespace swift::runtime::execution
