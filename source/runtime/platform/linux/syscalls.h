#pragma once

#include <cstdint>
#include <ctime>
#include <string>

namespace swift::runtime::execution {
class VM;
}

namespace swift::runtime::platform {

enum class SyscallServiceResult : std::uint8_t { Completed, ResumeGuestContext, Interrupted };

class LinuxSyscalls {
public:
    explicit LinuxSyscalls(execution::VM& vm) noexcept : vm(vm) {}

    SyscallServiceResult Service();

private:
    execution::VM& vm;
    bool reported_unknown{};
    bool resumed_deadline{};
    timespec restart_deadline{};
    std::int64_t HostCall(std::uint64_t number, const std::uint64_t* arguments);
    void RestartPolicy(std::uint64_t operation,
                       const std::uint64_t* arguments,
                       bool& with_handler,
                       bool& without_handler);
    std::int64_t Invoke(std::uint64_t, const std::uint64_t* arguments);
    bool ReadString(std::uint64_t, std::string&) const;
    std::int64_t Stat(int fd, std::uint64_t output, const char* path = nullptr, int flags = 0);
};
}  // namespace swift::runtime::platform
