#pragma once

#include <chrono>
#include <cstdint>

namespace swift::runtime::frontend {

// A null counter leaves ordinary builds without additional clock reads.
class ScopedBuildTimer {
public:
    explicit ScopedBuildTimer(std::uint64_t* counter) noexcept
            : counter(counter), begin(counter ? Clock::now() : Clock::time_point{}) {}

    ~ScopedBuildTimer() {
        if (counter)
            *counter += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin)
                            .count());
    }

private:
    using Clock = std::chrono::steady_clock;
    std::uint64_t* counter;
    Clock::time_point begin;
};

}  // namespace swift::runtime::frontend
