#pragma once

#include <chrono>
#include <cstdint>

namespace swift::runtime::execution {
using RuntimeClock = std::chrono::steady_clock;

struct RuntimeTimer {
    explicit RuntimeTimer(std::uint64_t& counter) noexcept : RuntimeTimer(&counter) {}

    explicit RuntimeTimer(std::uint64_t* counter) noexcept
            : counter(counter), begin(counter ? RuntimeClock::now() : RuntimeClock::time_point{}) {}

    ~RuntimeTimer() {
        if (counter)
            *counter +=
                    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                       RuntimeClock::now() - begin)
                                                       .count());
    }

private:
    std::uint64_t* counter;
    RuntimeClock::time_point begin;
};

struct PublicationTiming {
    std::uint64_t mapping_ns{}, metadata_ns{}, transfer_ns{}, synchronize_ns{}, registration_ns{},
            dispatch_cache_ns{};
};
}  // namespace swift::runtime::execution
