#pragma once

#include <chrono>
#include <cstdint>

namespace swift::runtime::execution {
using RuntimeClock = std::chrono::steady_clock;
struct RuntimeTimer {
    std::uint64_t& counter;
    RuntimeClock::time_point begin{RuntimeClock::now()};
    ~RuntimeTimer() {
        counter += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(RuntimeClock::now() - begin)
                        .count());
    }
};
}  // namespace swift::runtime::execution
