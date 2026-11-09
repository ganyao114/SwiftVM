#pragma once

#include <array>
#include <cstdint>
#include <ctime>
#include "runtime/arch/arm32/profile.h"
#include "signal_frame.h"

struct stat;

namespace swift::runtime::execution {
class GuestMemory;
}

namespace swift::runtime::platform::abi32 {
struct Timespec {
    std::int32_t seconds{}, nanos{};
};

struct Timeval {
    std::int32_t seconds{}, micros{};
};

struct Iovec {
    std::uint32_t base{}, length{};
};

struct Stack {
    std::uint32_t address{}, flags{2}, size{};
};

struct Action {
    std::uint32_t handler{}, flags{}, restorer{};
    std::array<std::uint32_t, 2> mask{};
};

struct SignalInfo {
    std::int32_t number{}, error{}, code{};
    std::array<std::uint8_t, 116> payload{};
};

struct alignas(8) Stat64 {
    std::uint64_t device{};
    std::uint32_t pad0{}, old_inode{}, mode{}, links{}, uid{}, gid{};
    std::uint64_t rdevice{};
    std::uint32_t pad1{}, pad2{};
    std::int64_t size{};
    std::uint32_t block_size{}, pad3{};
    std::uint64_t blocks{};
    std::uint32_t atime{}, atime_nsec{}, mtime{}, mtime_nsec{}, ctime{}, ctime_nsec{};
    std::uint64_t inode{};
};

static_assert(sizeof(Timespec) == 8 && sizeof(Iovec) == 8 && sizeof(Stack) == 12);
static_assert(sizeof(Action) == 20 && sizeof(SignalInfo) == 128 && sizeof(Stat64) == 104);
GuestSignalAction Decode(const Action&) noexcept;
Action Encode(const GuestSignalAction&) noexcept;
GuestSignalStack Decode(const Stack&) noexcept;
Stack Encode(const GuestSignalStack&) noexcept;
GuestSignalInfo Decode(const SignalInfo&) noexcept;
SignalInfo Encode(const GuestSignalInfo&) noexcept;
Stat64 Encode(const struct stat&) noexcept;
bool ReadTimespec(const execution::GuestMemory&, std::uint64_t, timespec&, bool time64 = false);
std::uint32_t HardwareCapabilities(const arm32::Profile&) noexcept;
std::uint64_t SyscallOperation(std::uint64_t number) noexcept;
}  // namespace swift::runtime::platform::abi32
