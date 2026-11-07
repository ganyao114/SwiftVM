#pragma once

#include "format.h"

#include <cstdint>
#include <cerrno>
#include <string_view>

namespace swift::log {
enum class Level : std::uint8_t { Debug, Info, Warning, Error, Off };
enum class Channel : std::uint8_t { Runtime, Compile, Decode, Allocation, Memory, Syscall, Count };

struct Sink {
    void* context{};
    void (*write)(void*, Level, Channel, std::string_view) noexcept;
};

void SetLevel(Level) noexcept;
void SetChannelEnabled(Channel, bool) noexcept;
bool Enabled(Level, Channel) noexcept;
// Configure before execution. A supplied sink and its context must outlive all
// concurrent writers; nullptr restores the platform's default sink.
void SetSink(const Sink*) noexcept;
const Sink& StandardErrorSink() noexcept;
void Write(Level, Channel, std::string_view) noexcept;

template <class... Args> void WriteFormatted(Level level,
                                             Channel channel,
                                             fmt::format_string<Args...> pattern,
                                             Args&&... args) noexcept {
    const auto saved_errno = errno;
    try {
        Write(level, channel, Format(pattern, std::forward<Args>(args)...));
    } catch (...) {
        Write(level, channel, "log message formatting failed");
    }
    errno = saved_errno;
}
}  // namespace swift::log

// Filtering happens before formatting arguments are evaluated.
#define SVM_LOG(level, channel, ...)                                                               \
    do {                                                                                           \
        if (::swift::log::Enabled(::swift::log::Level::level, ::swift::log::Channel::channel))     \
            ::swift::log::WriteFormatted(                                                          \
                    ::swift::log::Level::level, ::swift::log::Channel::channel, __VA_ARGS__);      \
    } while (false)
