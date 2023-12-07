#include <iostream>
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

static Level log_level = Level::Info;

void SetLogLevel(Level level) { log_level = level; }

void LogMessage(Level level, const std::string& message) {
    if (log_level >= level) {
        return;
    }
#if __ANDROID__
    auto android_level = GetAndroidLogLevel(level);
    __android_log_write(android_level, "Swift", message.c_str());
#else
    std::cout << message << std::endl;
#endif
}

void AssertFailed(const std::string& message) { throw std::logic_error(message); }

}  // namespace swift::log