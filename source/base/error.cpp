#include "error.h"
#include "format.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace swift {
void Failure::Set(ErrorKind category, std::string_view text, int saved_errno) noexcept {
    kind = category;
    system_error = saved_errno;
    system_text_complete = false;
    const auto bytes = std::min(text.size(), message.size() - 1);
    if (bytes) std::memcpy(message.data(), text.data(), bytes);
    message[bytes] = '\0';
}

void Failure::Capture(const std::exception& error) noexcept {
    if (const auto* runtime = dynamic_cast<const RuntimeError*>(&error)) {
        Set(runtime->Kind(), error.what(), runtime->SystemError());
        system_text_complete = true;
    } else if (dynamic_cast<const std::bad_alloc*>(&error))
        Set(ErrorKind::ResourceLimit, error.what());
    else
        Set(ErrorKind::Runtime, error.what());
}

std::string_view Failure::Message() const noexcept {
    return {message.data(), std::char_traits<char>::length(message.data())};
}

RuntimeError::RuntimeError(ErrorKind category, std::string message, int saved_errno)
        : std::runtime_error(std::move(message)), kind(category), system_error(saved_errno) {}

RuntimeError::RuntimeError(const Failure& failure)
        : RuntimeError(failure.kind,
                       failure.kind == ErrorKind::System && failure.system_error &&
                                       !failure.system_text_complete
                               ? Format("{}: {} (errno={})",
                                        failure.Message(),
                                        std::strerror(failure.system_error),
                                        failure.system_error)
                               : std::string(failure.Message()),
                       failure.system_error) {}

[[noreturn]] void ThrowError(std::string message, ErrorKind kind) {
    throw RuntimeError(kind, std::move(message));
}

[[noreturn]] void ThrowSystemError(std::string_view operation, int saved_errno) {
    throw RuntimeError(
            ErrorKind::System,
            Format("{}: {} (errno={})", operation, std::strerror(saved_errno), saved_errno),
            saved_errno);
}
}  // namespace swift
