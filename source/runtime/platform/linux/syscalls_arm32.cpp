#include "syscalls.h"

#include "abi_arm32.h"
#include "runtime/execution/vm.h"
#include "wait_syscall.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

namespace swift::runtime::platform {
namespace {
using u64 = std::uint64_t;
using s64 = std::int64_t;

u64 OpenFlags(u64 flags) {
    constexpr u64 target = 040000 | 0100000 | 0200000 | 0400000;
    return (flags & ~target) | ((flags & 040000) ? O_DIRECTORY : 0) |
           ((flags & 0100000) ? O_NOFOLLOW : 0) | ((flags & 0200000) ? O_DIRECT : 0);
}

u64 Pair(u64 low, u64 high) { return std::uint32_t(low) | (u64(std::uint32_t(high)) << 32); }
}  // namespace

s64 LinuxSyscalls::InvokeArm32(u64 number, const u64* input) {
    u64 args[6];
    std::copy_n(input, 6, args);
    time64_arguments = number == 403 || number == 421 || number == 422;
    switch (number) {
        case 0xf0005:
            vm.Arm32State().tpidruro = args[0];
            return 0;
        case 0xf0006:
            return vm.Arm32State().tpidruro;
        case 0xf0002:
            if (args[2] || args[1] < args[0]) return -EINVAL;
            if (!vm.memory.Contains(args[0], args[1] - args[0], PROT_EXEC)) return -EFAULT;
            vm.Invalidate();
            return 0;
        case 5:
            args[3] = args[2];
            args[2] = OpenFlags(args[1]);
            args[1] = args[0];
            args[0] = static_cast<u64>(AT_FDCWD);
            return InvokeOperation(257, args);
        case 322:
            args[2] = OpenFlags(args[2]);
            break;
        case 19: {
            args[1] = static_cast<s64>(static_cast<std::int32_t>(args[1]));
            const auto result = InvokeOperation(8, args);
            return result > INT32_MAX ? -EOVERFLOW : result;
        }
        case 55:
        case 221: {
            if (args[1] == F_SETFL) args[2] = OpenFlags(args[2]);
            const auto result = InvokeOperation(72, args);
            if (result < 0 || args[1] != F_GETFL) return result;
            constexpr u64 translated = O_DIRECTORY | O_NOFOLLOW | O_DIRECT | 0100000;
            return (u64(result) & ~translated) | 0400000 | ((result & O_DIRECTORY) ? 040000 : 0) |
                   ((result & O_NOFOLLOW) ? 0100000 : 0) | ((result & O_DIRECT) ? 0200000 : 0);
        }
        case 93:
            args[1] = static_cast<s64>(static_cast<std::int32_t>(args[1]));
            break;
        case 140: {
            const auto result = lseek(static_cast<int>(args[0]),
                                      static_cast<off_t>(Pair(args[2], args[1])),
                                      static_cast<int>(args[4]));
            if (result == off_t(-1)) return -errno;
            const u64 offset = result;
            return vm.GuestWrite(args[3], &offset, sizeof(offset)) ? 0 : -EFAULT;
        }
        case 181:
            args[3] = Pair(args[4], args[5]);
            break;
        case 194:
            args[1] = Pair(args[2], args[3]);
            break;
        case 192:
            args[5] <<= 12;  // mmap2 uses 4096-byte units on every host page size.
            break;
        case 195:
        case 196: {
            std::string path;
            if (!ReadString(args[0], path)) return -EFAULT;
            return Stat(AT_FDCWD, args[1], path.c_str(), number == 196 ? AT_SYMLINK_NOFOLLOW : 0);
        }
        case 78: {
            timeval now{};
            if (gettimeofday(&now, nullptr)) return -errno;
            if (now.tv_sec > INT32_MAX) return -EOVERFLOW;
            const abi32::Timeval value{std::int32_t(now.tv_sec), std::int32_t(now.tv_usec)};
            const std::int32_t zone[2]{};
            if (args[0] && !vm.GuestWrite(args[0], &value, sizeof(value))) return -EFAULT;
            if (args[1] && !vm.GuestWrite(args[1], zone, sizeof(zone))) return -EFAULT;
            return 0;
        }
        case 191: {
            rlimit limit{};
            if (getrlimit(static_cast<decltype(RLIMIT_STACK)>(args[0]), &limit)) return -errno;
            const std::uint32_t value[]{std::uint32_t(std::min<u64>(limit.rlim_cur, UINT32_MAX)),
                                        std::uint32_t(std::min<u64>(limit.rlim_max, UINT32_MAX))};
            return vm.GuestWrite(args[1], value, sizeof(value)) ? 0 : -EFAULT;
        }
        case 263:
        case 403: {
            timespec now{};
            if (clock_gettime(static_cast<clockid_t>(args[0]), &now)) return -errno;
            if (number == 403) {
                const std::int64_t value[]{now.tv_sec, now.tv_nsec};
                return vm.GuestWrite(args[1], value, sizeof(value)) ? 0 : -EFAULT;
            }
            if (now.tv_sec > INT32_MAX || now.tv_sec < INT32_MIN) return -EOVERFLOW;
            const abi32::Timespec value{std::int32_t(now.tv_sec), std::int32_t(now.tv_nsec)};
            return vm.GuestWrite(args[1], &value, sizeof(value)) ? 0 : -EFAULT;
        }
        case 162: {
            timespec duration{}, remaining{};
            if (!abi32::ReadTimespec(vm.memory, args[0], duration)) return -EFAULT;
            if (duration.tv_sec < 0 || duration.tv_nsec < 0 || duration.tv_nsec >= 1000000000)
                return -EINVAL;
            const u64 host[]{CLOCK_MONOTONIC,
                             0,
                             reinterpret_cast<u64>(&duration),
                             reinterpret_cast<u64>(&remaining),
                             0,
                             0};
            const auto result = HostCall(SYS_clock_nanosleep, host);
            if (result == -EINTR && args[1]) {
                const abi32::Timespec value{std::int32_t(remaining.tv_sec),
                                            std::int32_t(remaining.tv_nsec)};
                if (!vm.GuestWrite(args[1], &value, sizeof(value))) return -EFAULT;
            }
            return result;
        }
        case 146: {
            if (args[2] > 1024) return -EINVAL;
            std::vector<abi32::Iovec> guest(args[2]);
            if (!vm.memory.CopyIn(guest.data(), args[1], guest.size() * sizeof(abi32::Iovec)))
                return -EFAULT;
            std::vector<iovec> host;
            host.reserve(guest.size());
            for (const auto& v : guest) {
                if (!vm.memory.Contains(v.base, v.length, PROT_READ)) return -EFAULT;
                host.push_back({reinterpret_cast<void*>(u64(v.base)), v.length});
            }
            args[1] = reinterpret_cast<u64>(host.data());
            const auto result = HostCall(SYS_writev, args);
            if (result > 0) vm.FileChanged(static_cast<int>(args[0]));
            return result;
        }
        case 421:
            return vm.signals.WaitInfo(args[0], args[1], args[2], args[3], true);
        default:
            break;
    }
    return InvokeOperation(abi32::SyscallOperation(number), args);
}

SyscallServiceResult LinuxSyscalls::ServiceArm32() {
    auto& state = vm.Arm32State();
    const u64 arguments[]{std::uint32_t(state.r[0]),
                          std::uint32_t(state.r[1]),
                          std::uint32_t(state.r[2]),
                          std::uint32_t(state.r[3]),
                          std::uint32_t(state.r[4]),
                          std::uint32_t(state.r[5])};
    const auto number = std::uint32_t(state.r[7]);
    if (number == 173 || number == 119) {
        if (!vm.signals.ReturnFrame(number == 173)) {
            GuestExceptionInfo fault;
            fault.isa = GuestISA::Arm32;
            fault.origin = GuestExceptionOrigin::Semantic;
            fault.signal_number = 11;
            fault.instruction_pc = std::uint32_t(state.service_pc);
            fault.address = state.r[13];
            fault.address_valid = true;
            vm.DeliverException(fault);
        }
        return SyscallServiceResult::ResumeGuestContext;
    }
    resumed_deadline = vm.signals.ConsumeRestartDeadline(number, arguments, restart_deadline);
    current_guest_number = number;
    auto result = InvokeArm32(number, arguments);
    if (result == CancelledBeforeCall) result = -EINTR;
    state.r[0] = std::uint32_t(result);
    if (result == -EINTR && vm.signals.Pending()) {
        bool with_handler{}, without_handler{};
        RestartPolicy(abi32::SyscallOperation(number), arguments, with_handler, without_handler);
        vm.signals.InterruptSyscall(number, arguments, with_handler, without_handler);
    }
    return result == -EINTR ? SyscallServiceResult::Interrupted : SyscallServiceResult::Completed;
}
}  // namespace swift::runtime::platform
