#pragma once

#include <fmt/format.h>
#include "types.h"

namespace swift::runtime::log {

enum class Level {
    Info,
    Debug,
    Warning,
    Error,
    Max,
};

void SetLogLevel(Level log_level);

void LogMessage(Level level, const std::string& message);

template <typename... Args> void LogMessage(Level log_level,
                                            const char* filename,
                                            unsigned int line_num,
                                            const char* function,
                                            const char* format,
                                            const Args&... args) {
    const auto info = fmt::vformat(format, fmt::make_format_args(args...));
#if CONFIG_DEBUG_MSG
    const auto message = fmt::format("{}:{}:{}: {}", filename, line_num, function, info);
    LogMessage(log_level, message);
#else
    LogMessage(log_level, info);
#endif
}

void AssertFailed(const std::string& message);

}  // namespace swift::runtime::log

#ifdef STRIP_LOG
#define LOG(level, ...) (void(0))
#else
#define LOG(level, ...)                                                                            \
    swift::runtime::log::LogMessage(                                                               \
            swift::runtime::log::Level::level, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#endif

#define LOG_INFO(...) LOG(Info, __VA_ARGS__)
#define LOG_DEBUG(...) LOG(Debug, __VA_ARGS__)
#define LOG_WARNING(...) LOG(Warning, __VA_ARGS__)
#define LOG_ERROR(...) LOG(Error, __VA_ARGS__)

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            swift::runtime::log::AssertFailed(fmt::format("Check failed: \"{}\"", #cond));         \
        }                                                                                          \
    } while (0)

#define ASSERT_MSG(cond, ...)                                                                      \
    do                                                                                             \
        if (!(cond)) {                                                                             \
            swift::runtime::log::AssertFailed(fmt::format("Check Failed! " __VA_ARGS__));          \
        }                                                                                          \
    while (0)

#define PANIC(...) ASSERT_MSG(false, __VA_ARGS__)
