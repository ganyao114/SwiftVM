#include "base/error.h"
#include "base/logging.h"

#include <catch2/catch_test_macros.hpp>
#include <cerrno>

namespace {
struct LogCapture {
    unsigned writes{};
    std::string message;
    swift::log::Sink sink{
            this,
            [](void* raw, swift::log::Level, swift::log::Channel, std::string_view text) noexcept {
                auto& capture = *static_cast<LogCapture*>(raw);
                ++capture.writes;
                // Test messages are short and capacity is reserved before logging.
                capture.message.assign(text);
                errno = ENOSPC;
            }};

    LogCapture() {
        message.reserve(128);
        swift::log::SetSink(&sink);
        swift::log::SetLevel(swift::log::Level::Warning);
    }

    ~LogCapture() {
        swift::log::SetChannelEnabled(swift::log::Channel::Compile, true);
        swift::log::SetLevel(swift::log::Level::Warning);
        swift::log::SetSink(nullptr);
    }
};
}  // namespace

TEST_CASE("disabled log messages do not evaluate their arguments", "[base][compile]") {
    LogCapture capture;
    unsigned evaluated{};
    SVM_LOG(Debug, Compile, "value {}", ++evaluated);
    swift::log::SetChannelEnabled(swift::log::Channel::Compile, false);
    SVM_LOG(Error, Compile, "value {}", ++evaluated);
    CHECK(evaluated == 0);
    CHECK(capture.writes == 0);
    swift::log::SetChannelEnabled(swift::log::Channel::Compile, true);
    errno = EACCES;
    SVM_LOG(Error, Compile, "value {}", ++evaluated);
    CHECK(errno == EACCES);
    CHECK(evaluated == 1);
    CHECK(capture.writes == 1);
    CHECK(capture.message == "value 1");
    errno = EBADF;
    swift::log::Write(swift::log::Level::Error, swift::log::Channel::Compile, "direct message");
    CHECK(errno == EBADF);
}

TEST_CASE("noexcept failure capture keeps its category and original system error",
          "[base][compile]") {
    swift::Failure failure;
    try {
        swift::ThrowSystemError("map native code", EACCES);
    } catch (const std::exception& error) {
        failure.Capture(error);
    }
    REQUIRE(failure);
    CHECK(failure.kind == swift::ErrorKind::System);
    CHECK(failure.system_error == EACCES);
    CHECK(failure.Message().find("map native code") != std::string_view::npos);
    const swift::RuntimeError delivered{failure};
    CHECK(delivered.Kind() == swift::ErrorKind::System);
    CHECK(delivered.SystemError() == EACCES);
    CHECK(std::string_view(delivered.what()) == failure.Message());
}

TEST_CASE("failure records own bounded text after the exception has unwound", "[base][compile]") {
    swift::Failure failure;
    try {
        swift::ThrowError(std::string(1024, 'x'), swift::ErrorKind::GuestFault);
    } catch (const std::exception& error) {
        failure.Capture(error);
    }
    CHECK(failure.kind == swift::ErrorKind::GuestFault);
    CHECK(failure.Message().size() == failure.message.size() - 1);
    CHECK(failure.message.back() == '\0');
}

TEST_CASE("a raw signal error is rendered once after leaving the signal path", "[base][compile]") {
    swift::Failure recorded;
    recorded.Set(swift::ErrorKind::System, "make native code writable", EACCES);
    CHECK(recorded.Message() == "make native code writable");
    const swift::RuntimeError delivered{recorded};
    CHECK(std::string_view(delivered.what()).find("errno=") != std::string_view::npos);
    swift::Failure captured;
    captured.Capture(delivered);
    const swift::RuntimeError restored{captured};
    CHECK(std::string_view(restored.what()) == delivered.what());
}
