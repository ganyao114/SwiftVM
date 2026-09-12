#pragma once

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <optional>
#include <string>
#include "base/types.h"

namespace swift::linux {

struct RegisterDumpSpec {
    u64 syscall;
    u64 occurrence;

    static std::optional<RegisterDumpSpec> Parse(const std::string& text) {
        // Preserve decimal/octal/hex syntax, but require two complete unsigned
        // numbers. A missing colon must never advance beyond the terminator.
        const int saved_errno = errno;
        const char* cursor = text.c_str();
        auto number = [&](u64& value) {
            if (*cursor < '0' || *cursor > '9') return false;
            char* end = nullptr;
            errno = 0;
            value = std::strtoull(cursor, &end, 0);
            const bool valid = errno != ERANGE && end != cursor;
            cursor = end;
            return valid;
        };
        RegisterDumpSpec spec{};
        const bool valid = number(spec.syscall) && *cursor == ':' &&
                           (++cursor, number(spec.occurrence)) &&
                           *cursor == '\0' && spec.occurrence != 0;
        errno = saved_errno;
        return valid ? std::optional{spec} : std::nullopt;
    }
};

class RegisterDumpTrigger {
public:
    explicit RegisterDumpTrigger(const std::string& text)
        : spec(RegisterDumpSpec::Parse(text)) {}

    bool Take(u64 syscall) {
        if (!spec || syscall != spec->syscall) return false;
        auto count = seen.load(std::memory_order_relaxed);
        while (count < spec->occurrence) {
            if (seen.compare_exchange_weak(count, count + 1,
                                            std::memory_order_relaxed)) {
                return count + 1 == spec->occurrence;
            }
        }
        return false;
    }

private:
    const std::optional<RegisterDumpSpec> spec;
    std::atomic<u64> seen{0};
};

} // namespace swift::linux
