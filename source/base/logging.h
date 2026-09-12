#pragma once

#include <fmt/format.h>
#include <fmt/printf.h>
#include "types.h"

namespace swift::log {

enum class Level {
    Debug,
    Info,
    Warning,
    Error,
    Max,
};

void SetLogLevel(Level log_level);
Level GetLogLevel();
bool Enabled(Level level);

enum class DiagnosticChannel { Syscall, Decode, RegisterAllocation, Codegen, Memory, Runtime, Count };
void SetDiagnosticChannelEnabled(DiagnosticChannel channel, bool enabled);
bool DiagnosticEnabled(DiagnosticChannel channel);
void WriteDiagnostic(DiagnosticChannel channel, std::string_view message);

template<class... Args>
void DiagnosticFormat(DiagnosticChannel channel, const char* format, const Args&... args) {
    if (DiagnosticEnabled(channel))
        WriteDiagnostic(channel, fmt::vformat(format, fmt::make_format_args(args...)));
}

template<class... Args>
void DiagnosticPrint(DiagnosticChannel channel, const char* format, const Args&... args) {
    if (DiagnosticEnabled(channel)) WriteDiagnostic(channel, fmt::sprintf(format, args...));
}

void LogMessage(Level level, const std::string& message);

template <typename... Args> void LogMessage(Level log_level,
                                            const char* filename,
                                            unsigned int line_num,
                                            const char* function,
                                            const char* format,
                                            const Args&... args) {
    if (!Enabled(log_level)) return;
    const auto info = fmt::vformat(format, fmt::make_format_args(args...));
#if CONFIG_DEBUG_MSG
    const auto message = fmt::format("{}:{}:{}: {}", filename, line_num, function, info);
    LogMessage(log_level, message);
#else
    LogMessage(log_level, info);
#endif
}

void AssertFailed(const std::string& message);

}  // namespace swift::log

#ifdef STRIP_LOG
#define LOG(level, ...) (void(0))
#else
#define LOG(level, ...)                                                                            \
    do { \
        if (::swift::log::Enabled(::swift::log::Level::level)) \
            ::swift::log::LogMessage(::swift::log::Level::level, \
                                     __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__); \
    } while (0)
#endif

#define LOG_INFO(...) LOG(Info, __VA_ARGS__)
#define LOG_DEBUG(...) LOG(Debug, __VA_ARGS__)
#define LOG_WARNING(...) LOG(Warning, __VA_ARGS__)
#define LOG_ERROR(...) LOG(Error, __VA_ARGS__)

// Existing diagnostic switches select records; channel filtering can silence
// a whole subsystem without evaluating its formatting arguments.
#define SVM_DIAG_FORMAT(channel, ...) \
    do { \
        if (::swift::log::DiagnosticEnabled(::swift::log::DiagnosticChannel::channel)) \
            ::swift::log::DiagnosticFormat(::swift::log::DiagnosticChannel::channel, __VA_ARGS__); \
    } while (0)
#define SVM_DIAG_PRINT(channel, ...) \
    do { \
        if (::swift::log::DiagnosticEnabled(::swift::log::DiagnosticChannel::channel)) \
            ::swift::log::DiagnosticPrint(::swift::log::DiagnosticChannel::channel, __VA_ARGS__); \
    } while (0)
#define SVM_DIAG_TEXT(channel, message) \
    do { \
        if (::swift::log::DiagnosticEnabled(::swift::log::DiagnosticChannel::channel)) \
            ::swift::log::WriteDiagnostic(::swift::log::DiagnosticChannel::channel, message); \
    } while (0)

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            swift::log::AssertFailed(fmt::format("Check failed: \"{}\"", #cond));         \
        }                                                                                          \
    } while (0)

#define ASSERT_MSG(cond, ...)                                                                      \
    do                                                                                             \
        if (!(cond)) {                                                                             \
            swift::log::AssertFailed(fmt::format("Check Failed! " __VA_ARGS__));          \
        }                                                                                          \
    while (0)

#define PANIC(...) ASSERT_MSG(false, __VA_ARGS__)
