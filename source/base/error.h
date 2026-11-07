#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace swift {
enum class ErrorKind : std::uint8_t {
    None,
    Runtime,
    System,
    InvalidArgument,
    InvalidState,
    ResourceLimit,
    GuestFault,
    Unsupported,
};

// A cold bridge can preserve a failure without allocating or throwing. This
// record never replaces the frontend/IR/backend's own typed rejection results.
struct Failure {
    ErrorKind kind{};
    int system_error{};
    // Set records a raw operation; Capture owns an already rendered what().
    bool system_text_complete{};
    std::array<char, 256> message{};

    void Clear() noexcept {
        kind = ErrorKind::None;
        system_error = 0;
        system_text_complete = false;
        message[0] = '\0';
    }

    void Set(ErrorKind, std::string_view, int saved_errno = 0) noexcept;
    void Capture(const std::exception&) noexcept;
    std::string_view Message() const noexcept;

    explicit operator bool() const noexcept { return kind != ErrorKind::None; }
};

class RuntimeError : public std::runtime_error {
public:
    RuntimeError(ErrorKind, std::string message, int saved_errno = 0);
    explicit RuntimeError(const Failure&);

    ErrorKind Kind() const noexcept { return kind; }

    int SystemError() const noexcept { return system_error; }

private:
    ErrorKind kind;
    int system_error;
};

[[noreturn]] void ThrowError(std::string message, ErrorKind = ErrorKind::Runtime);
[[noreturn]] void ThrowSystemError(std::string_view operation, int saved_errno);
}  // namespace swift
