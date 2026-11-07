#pragma once

#include <cstdint>
#include <cerrno>
#include <time.h>

namespace swift::runtime::platform {

// Linux signal paths use a fixed clock_gettime record, never a logger or an
// allocating formatter. Failure only omits this optional diagnostic interval.
inline std::uint64_t SignalClockNanoseconds() noexcept {
    const auto saved_error = errno;
    timespec value{};
    const bool valid = clock_gettime(CLOCK_MONOTONIC, &value) == 0;
    errno = saved_error;
    return valid ? static_cast<std::uint64_t>(value.tv_sec) * 1000000000 +
                           static_cast<std::uint64_t>(value.tv_nsec)
                 : 0;
}

struct SignalTimer {
    std::uint64_t& total;
    std::uint64_t begin{SignalClockNanoseconds()};

    ~SignalTimer() {
        const auto end = SignalClockNanoseconds();
        if (begin && end >= begin) total += end - begin;
    }
};
}  // namespace swift::runtime::platform
