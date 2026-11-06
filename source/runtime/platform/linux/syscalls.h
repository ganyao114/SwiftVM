#pragma once

#include <cstdint>
#include <string>

namespace swift::runtime::execution {
class VM;
}
namespace swift::runtime::platform {

class LinuxSyscalls {
public:
    explicit LinuxSyscalls(execution::VM& vm) noexcept : vm(vm) {}
    void Service();

private:
    execution::VM& vm;
    bool reported_unknown{};
    std::int64_t Invoke(std::uint64_t, const std::uint64_t* arguments);
    bool ReadString(std::uint64_t, std::string&) const;
    std::int64_t Stat(int fd, std::uint64_t output, const char* path = nullptr, int flags = 0);
};
}  // namespace swift::runtime::platform
