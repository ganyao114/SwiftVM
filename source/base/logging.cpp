#include "logging.h"

#include <atomic>
#include <cstdio>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace swift::log {
namespace {
std::atomic<Level> threshold{Level::Warning};
std::atomic<unsigned> channels{(1u << static_cast<unsigned>(Channel::Count)) - 1};

void WriteStandardError(void*, Level level, Channel, std::string_view message) noexcept {
    constexpr const char* names[]{"debug: ", "info: ", "warning: ", "error: "};
    std::fputs(names[static_cast<unsigned>(level)], stderr);
    std::fwrite(message.data(), 1, message.size(), stderr);
    std::fputc('\n', stderr);
}

const Sink standard_error{nullptr, WriteStandardError};
#if defined(__ANDROID__)
void WriteAndroid(void*, Level level, Channel, std::string_view message) noexcept {
    constexpr int priorities[]{
            ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
    __android_log_print(priorities[static_cast<unsigned>(level)],
                        "SwiftVM",
                        "%.*s",
                        static_cast<int>(message.size()),
                        message.data());
}

const Sink default_sink{nullptr, WriteAndroid};
#else
const Sink& default_sink = standard_error;
#endif
std::atomic<const Sink*> sink{&default_sink};
}  // namespace

void SetLevel(Level level) noexcept { threshold.store(level, std::memory_order_relaxed); }

void SetChannelEnabled(Channel channel, bool enabled) noexcept {
    const auto bit = 1u << static_cast<unsigned>(channel);
    if (enabled)
        channels.fetch_or(bit, std::memory_order_relaxed);
    else
        channels.fetch_and(~bit, std::memory_order_relaxed);
}

bool Enabled(Level level, Channel channel) noexcept {
    return level != Level::Off && level >= threshold.load(std::memory_order_relaxed) &&
           (channels.load(std::memory_order_relaxed) & (1u << static_cast<unsigned>(channel)));
}

void SetSink(const Sink* output) noexcept {
    sink.store(output ? output : &default_sink, std::memory_order_release);
}

const Sink& StandardErrorSink() noexcept { return standard_error; }

void Write(Level level, Channel channel, std::string_view message) noexcept {
    if (!Enabled(level, channel)) return;
    const auto saved_errno = errno;
    const auto* output = sink.load(std::memory_order_acquire);
    output->write(output->context, level, channel, message);
    errno = saved_errno;
}
}  // namespace swift::log
