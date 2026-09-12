#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <unistd.h>

namespace swift::runtime {

// Bounded, allocation-free diagnostic output for signal callbacks. Callers
// supply saved values only; this helper never follows guest/host pointers.
class SignalDiagnostic {
public:
    explicit SignalDiagnostic(const char* prefix) { Append(prefix); }

    void Field(const char* name, std::uint64_t value) {
        Append(" ");
        Append(name);
        Append("=0x");
        bool started = false;
        for (int shift = 60; shift >= 0; shift -= 4) {
            const auto digit = (value >> shift) & 15;
            if (digit || started || shift == 0) {
                Put("0123456789abcdef"[digit]);
                started = true;
            }
        }
    }

    void Write(int fd = STDERR_FILENO) {
        const int saved_errno = errno;
        buffer[size++] = '\n';
        const auto ignored = ::write(fd, buffer, size);
        (void)ignored;
        --size;
        errno = saved_errno;
    }

private:
    void Put(char ch) {
        if (size < sizeof(buffer) - 1) buffer[size++] = ch;
    }
    void Append(const char* text) {
        while (*text && size < sizeof(buffer) - 1) Put(*text++);
    }
    char buffer[768];
    std::size_t size{};
};

// Saturates instead of wrapping, and remains safe across concurrent runtimes.
inline bool ClaimDiagnosticSample(std::atomic<std::uint32_t>& count,
                                  std::uint32_t limit) {
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    auto current = count.load(std::memory_order_relaxed);
    while (current < limit) {
        if (count.compare_exchange_weak(current, current + 1,
                                        std::memory_order_relaxed)) return true;
    }
    return false;
}

}  // namespace swift::runtime
