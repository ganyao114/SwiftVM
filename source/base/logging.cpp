#include <atomic>
#include <cstdio>
#include <stdexcept>
#include "logging.h"

#if __ANDROID__
#include <android/log.h>
static android_LogPriority GetAndroidLogLevel(swift::log::Level log_level) {
    android_LogPriority priority = ANDROID_LOG_DEFAULT;
    switch (log_level) {
        case swift::log::Level::Debug:
            priority = ANDROID_LOG_DEBUG;
            break;
        case swift::log::Level::Info:
            priority = ANDROID_LOG_INFO;
            break;
        case swift::log::Level::Warning:
            priority = ANDROID_LOG_WARN;
            break;
        case swift::log::Level::Error:
            priority = ANDROID_LOG_ERROR;
            break;
        default:
            break;
    }
    return priority;
}
#endif

namespace swift::log {

static std::atomic<Level> log_level{Level::Warning};
static std::atomic<unsigned> diagnostic_channels{~0u};

void SetDiagnosticChannelEnabled(DiagnosticChannel channel, bool enabled) {
    const auto index = static_cast<unsigned>(channel);
    if (index >= static_cast<unsigned>(DiagnosticChannel::Count)) return;
    const auto bit = 1u << index;
    if (enabled) diagnostic_channels.fetch_or(bit, std::memory_order_relaxed);
    else diagnostic_channels.fetch_and(~bit, std::memory_order_relaxed);
}

bool DiagnosticEnabled(DiagnosticChannel channel) {
    return static_cast<unsigned>(channel) < static_cast<unsigned>(DiagnosticChannel::Count) &&
           (diagnostic_channels.load(std::memory_order_relaxed) &
            (1u << static_cast<unsigned>(channel)));
}

void WriteDiagnostic(DiagnosticChannel channel, std::string_view message) {
    if (!DiagnosticEnabled(channel)) return;
    std::fwrite(message.data(), 1, message.size(), stderr);
}

void SetLogLevel(Level level) { log_level.store(level, std::memory_order_relaxed); }
Level GetLogLevel() { return log_level.load(std::memory_order_relaxed); }
bool Enabled(Level level) { return level != Level::Max && level >= GetLogLevel(); }

void LogMessage(Level level, const std::string& message) {
    if (!Enabled(level)) {
        return;
    }
#if __ANDROID__
    auto android_level = GetAndroidLogLevel(level);
    __android_log_write(android_level, "Swift", message.c_str());
#else
    fmt::print(stderr, "{}\n", message);
#endif
}

void AssertFailed(const std::string& message) { throw std::logic_error(message); }

}  // namespace swift::log
