#include "abi_arm32.h"
#include "runtime/arch/x86/flags.h"
#include "runtime/execution/vm.h"
#include "syscall_abi.h"
#include "syscalls.h"
#include "wait_syscall.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <fcntl.h>
#include <linux/futex.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include "base/logging.h"

namespace swift::runtime::platform {
namespace {
using u64 = std::uint64_t;
using s64 = std::int64_t;
using namespace ::swift::runtime::x86;

s64 Result(s64 result) { return result < 0 ? -errno : result; }

struct GuestStat {
    u64 device, inode, links;
    std::uint32_t mode, uid, gid, padding;
    u64 rdevice;
    s64 size, block_size, blocks, atime;
    u64 atime_nsec;
    s64 mtime;
    u64 mtime_nsec;
    s64 ctime;
    u64 ctime_nsec;
    s64 unused[3];
};

static_assert(sizeof(GuestStat) == 144);

struct ArmGuestStat {
    u64 device, inode;
    std::uint32_t mode, links, uid, gid;
    u64 rdevice, padding;
    s64 size;
    std::int32_t block_size, padding2;
    s64 blocks, atime;
    u64 atime_nsec;
    s64 mtime;
    u64 mtime_nsec;
    s64 ctime;
    u64 ctime_nsec;
    std::uint32_t unused[2];
};

static_assert(sizeof(ArmGuestStat) == 128);

}  // namespace

bool LinuxSyscalls::ReadString(u64 address, std::string& value) const {
    const auto& memory = vm.memory;
    value.clear();
    for (std::size_t i = 0; i < 4096; ++i) {
        if (!memory.Contains(address + i, 1, PROT_READ)) return false;
        char byte;
        if (!memory.CopyIn(&byte, address + i, 1)) return false;
        if (!byte) return true;
        value += byte;
    }
    return false;
}

s64 LinuxSyscalls::Stat(int fd, u64 output, const char* path, int flags) {
    struct stat status{};
    const auto result = path ? fstatat(fd, path, &status, flags) : fstat(fd, &status);
    if (result) return -errno;
    if (vm.ISA() == GuestISA::Arm32) {
        const auto guest = abi32::Encode(status);
        return vm.GuestWrite(output, &guest, sizeof(guest)) ? 0 : -EFAULT;
    }
    if (vm.ISA() == GuestISA::Arm64) {
        const ArmGuestStat guest{static_cast<u64>(status.st_dev),
                                 static_cast<u64>(status.st_ino),
                                 status.st_mode,
                                 static_cast<std::uint32_t>(status.st_nlink),
                                 status.st_uid,
                                 status.st_gid,
                                 static_cast<u64>(status.st_rdev),
                                 0,
                                 status.st_size,
                                 static_cast<std::int32_t>(status.st_blksize),
                                 0,
                                 status.st_blocks,
                                 status.st_atim.tv_sec,
                                 static_cast<u64>(status.st_atim.tv_nsec),
                                 status.st_mtim.tv_sec,
                                 static_cast<u64>(status.st_mtim.tv_nsec),
                                 status.st_ctim.tv_sec,
                                 static_cast<u64>(status.st_ctim.tv_nsec),
                                 {0, 0}};
        return vm.GuestWrite(output, &guest, sizeof(guest)) ? 0 : -EFAULT;
    }
    const GuestStat guest{static_cast<u64>(status.st_dev),
                          static_cast<u64>(status.st_ino),
                          status.st_nlink,
                          status.st_mode,
                          status.st_uid,
                          status.st_gid,
                          0,
                          static_cast<u64>(status.st_rdev),
                          status.st_size,
                          status.st_blksize,
                          status.st_blocks,
                          status.st_atim.tv_sec,
                          static_cast<u64>(status.st_atim.tv_nsec),
                          status.st_mtim.tv_sec,
                          static_cast<u64>(status.st_mtim.tv_nsec),
                          status.st_ctim.tv_sec,
                          static_cast<u64>(status.st_ctim.tv_nsec),
                          {0, 0, 0}};
    return vm.GuestWrite(output, &guest, sizeof(guest)) ? 0 : -EFAULT;
}

s64 LinuxSyscalls::HostCall(u64 number, const u64* arguments) {
    u64 adjusted[6];
    std::copy_n(arguments, 6, adjusted);
    const bool relative_wait =
            number == SYS_futex && (arguments[1] & FUTEX_CMD_MASK) == FUTEX_WAIT && arguments[3];
    timespec deadline{}, remaining{};
    if (relative_wait) {
        if (resumed_deadline) {
            deadline = restart_deadline;
        } else {
            timespec duration{};
            if (!(vm.ISA() == GuestISA::Arm32
                          ? abi32::ReadTimespec(vm.memory, arguments[3], duration, time64_arguments)
                          : vm.memory.CopyIn(&duration, arguments[3], sizeof(duration))))
                return -EFAULT;
            if (duration.tv_sec < 0 || duration.tv_nsec < 0 || duration.tv_nsec >= 1000000000)
                return -EINVAL;
            if (clock_gettime(CLOCK_MONOTONIC, &deadline)) return -errno;
            const auto carry = (deadline.tv_nsec + duration.tv_nsec) / 1000000000;
            if (duration.tv_sec > std::numeric_limits<time_t>::max() - deadline.tv_sec - carry)
                return -EINVAL;
            deadline.tv_sec += duration.tv_sec + carry;
            deadline.tv_nsec = (deadline.tv_nsec + duration.tv_nsec) % 1000000000;
        }
        adjusted[3] = reinterpret_cast<u64>(&remaining);
    }
    timespec absolute{};
    if (number == SYS_futex && arguments[3] &&
        (arguments[1] & FUTEX_CMD_MASK) == FUTEX_WAIT_BITSET && vm.ISA() == GuestISA::Arm32) {
        if (!abi32::ReadTimespec(vm.memory, arguments[3], absolute, time64_arguments))
            return -EFAULT;
        adjusted[3] = reinterpret_cast<u64>(&absolute);
    }
    resumed_deadline = false;
    for (;;) {
        if (relative_wait) {
            timespec now{};
            if (clock_gettime(CLOCK_MONOTONIC, &now)) return -errno;
            remaining.tv_sec = deadline.tv_sec - now.tv_sec;
            remaining.tv_nsec = deadline.tv_nsec - now.tv_nsec;
            if (remaining.tv_nsec < 0) {
                --remaining.tv_sec;
                remaining.tv_nsec += 1000000000;
            }
            if (remaining.tv_sec < 0) remaining = {};
        }
        const auto result = vm.signals.WaitSyscall(number, adjusted);
        if ((result == -EINTR || result == CancelledBeforeCall) && !vm.signals.Pending()) continue;
        if (relative_wait && (result == -EINTR || result == CancelledBeforeCall))
            vm.signals.PreserveFutexDeadline(deadline);
        return result;
    }
}

void LinuxSyscalls::RestartPolicy(u64 operation,
                                  const u64* arguments,
                                  bool& with_handler,
                                  bool& without_handler) {
    with_handler = operation == 0 || operation == 1 || operation == 18 || operation == 20 ||
                   operation == 318 || (operation == 202 && !arguments[3]);
    without_handler = with_handler || operation == 202 || operation == 130;
    if (operation == 0 || operation == 1 || operation == 20) {
        timeval timeout{};
        socklen_t size = sizeof(timeout);
        const int option = operation == 0 ? SO_RCVTIMEO : SO_SNDTIMEO;
        if (!getsockopt(static_cast<int>(arguments[0]), SOL_SOCKET, option, &timeout, &size)) {
            if (timeout.tv_sec || timeout.tv_usec) with_handler = without_handler = false;
        } else if (errno != ENOTSOCK) {
            swift::ThrowSystemError("query interrupted descriptor restart policy", errno);
        }
    }
}

s64 LinuxSyscalls::Invoke(u64 guest_number, const u64* args) {
    const auto number =
            vm.ISA() == GuestISA::Arm64 ? Arm64SyscallOperation(guest_number) : guest_number;
    return InvokeOperation(number, args);
}

s64 LinuxSyscalls::InvokeOperation(u64 number, const u64* args) {
    auto& memory = vm.memory;
    auto& stats = vm.stats;
    auto& options = vm.options;
    auto& clear_child_tid = vm.thread.clear_child_tid;
    auto& exited = vm.thread.exited;
    auto& exit_code = vm.thread.exit_code;
    switch (number) {
        case 0:
            if (!vm.PrepareGuestWrite(args[1], args[2])) return -EFAULT;
            return HostCall(SYS_read, args);
        case 1: {
            if (!memory.Contains(args[1], args[2], PROT_READ)) return -EFAULT;
            const auto count = HostCall(SYS_write, args);
            if (count > 0) vm.FileChanged(static_cast<int>(args[0]));
            return count;
        }
        case 3:
            return Result(close(static_cast<int>(args[0])));
        case 87: {
            std::string path;
            if (!ReadString(args[0], path)) return -EFAULT;
            return Result(unlink(path.c_str()));
        }
        case 88: {
            std::string target, link;
            if (!ReadString(args[0], target) || !ReadString(args[1], link)) return -EFAULT;
            return Result(symlink(target.c_str(), link.c_str()));
        }
        case 217:
            if (!vm.PrepareGuestWrite(args[1], args[2])) return -EFAULT;
            return Result(syscall(SYS_getdents64,
                                  static_cast<int>(args[0]),
                                  reinterpret_cast<void*>(args[1]),
                                  static_cast<std::size_t>(args[2])));
        case 263: {
            std::string path;
            if (!ReadString(args[1], path)) return -EFAULT;
            return Result(unlinkat(static_cast<int>(args[0]), path.c_str(), static_cast<int>(args[2])));
        }
        case 266: {
            std::string target, link;
            if (!ReadString(args[0], target) || !ReadString(args[2], link)) return -EFAULT;
            return Result(symlinkat(target.c_str(), static_cast<int>(args[1]), link.c_str()));
        }
        case 32:
            return Result(dup(static_cast<int>(args[0])));
        case 33:
            return Result(dup2(static_cast<int>(args[0]), static_cast<int>(args[1])));
        case 292:
            return Result(syscall(SYS_dup3,
                                  static_cast<int>(args[0]),
                                  static_cast<int>(args[1]),
                                  static_cast<int>(args[2])));
        case 5:
            return Stat(static_cast<int>(args[0]), args[1]);
        case 8:
            return Result(lseek(static_cast<int>(args[0]),
                                static_cast<off_t>(args[1]),
                                static_cast<int>(args[2])));
        case 9: {
            constexpr u64 mapping_flags = MAP_SHARED | MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS |
                                          MAP_DENYWRITE | MAP_EXECUTABLE | MAP_NORESERVE;
            if ((args[2] & ~u64{7}) || (args[3] & ~mapping_flags)) return -EINVAL;
            const bool invalidate =
                    (args[3] & MAP_FIXED) && memory.WriteAffectsCode(args[0], args[1]);
            const auto address = memory.Map(args[0],
                                            args[1],
                                            static_cast<int>(args[2]),
                                            static_cast<int>(args[3]),
                                            static_cast<int>(args[4]),
                                            args[5]);
            if (address == UINT64_MAX) return -errno;
            if (invalidate || (args[2] & PROT_EXEC) ||
                ((args[2] & PROT_WRITE) && memory.WriteAffectsCode(address, args[1])))
                vm.Invalidate();
            return static_cast<s64>(address);
        }
        case 10: {
            if (args[2] & ~u64{7}) return -EINVAL;
            const bool invalidate =
                    memory.WriteAffectsCode(args[0], args[1]) || (args[2] & PROT_EXEC);
            const auto result = memory.Protect(args[0], args[1], static_cast<int>(args[2]));
            if (!result && invalidate) vm.Invalidate();
            return Result(result);
        }
        case 11: {
            const bool invalidate = memory.WriteAffectsCode(args[0], args[1]);
            const auto result = memory.Unmap(args[0], args[1]);
            if (!result && invalidate) vm.Invalidate();
            return Result(result);
        }
        case 12: {
            const auto previous = memory.Brk(0);
            if (args[0] && args[0] != previous &&
                memory.WriteAffectsCode(std::min(previous, args[0]),
                                        std::max(previous, args[0]) - std::min(previous, args[0])))
                vm.Invalidate();
            return static_cast<s64>(memory.Brk(args[0]));
        }
        case 13:
            return vm.signals.Action(static_cast<int>(args[0]), args[1], args[2], args[3]);
        case 14:
            return vm.signals.Mask(static_cast<int>(args[0]), args[1], args[2], args[3]);
        case 127:
            return vm.signals.PendingMask(args[0], args[1]);
        case 128:
            return vm.signals.WaitInfo(args[0], args[1], args[2], args[3]);
        case 130:
            return vm.signals.Suspend(args[0], args[1]);
        case 131:
            return vm.signals.AltStack(args[0], args[1]);
        case 62:
        case 200:
        case 234:
        case 129:
        case 297: {
            const auto target = static_cast<s64>(args[0]);
            const auto tid = number == 234 || number == 297 ? static_cast<s64>(args[1]) : 0;
            if ((target != 0 && target != getpid() && target != syscall(SYS_gettid)) ||
                (tid && tid != syscall(SYS_gettid)))
                return -ESRCH;
            const auto requested = args[number == 234 || number == 297 ? 2 : 1];
            if (requested > 64) return -EINVAL;
            const auto signal = static_cast<int>(requested);
            if (!signal) return 0;
            GuestSignalInfo info{};
            const bool queued = number == 129 || number == 297;
            if (queued) {
                const auto input = args[number == 297 ? 3 : 2];
                if (vm.ISA() == GuestISA::Arm32) {
                    abi32::SignalInfo compact;
                    if (!memory.CopyIn(&compact, input, sizeof(compact))) return -EFAULT;
                    info = abi32::Decode(compact);
                } else if (!memory.CopyIn(&info, input, sizeof(info)))
                    return -EFAULT;
            }
            if (queued && info.code >= 0) return -EPERM;
            info.number = signal;
            if (!queued) {
                info.code = number == 62 ? 0 : -6;
                const auto pid = static_cast<std::int32_t>(getpid());
                const auto uid = static_cast<std::uint32_t>(getuid());
                std::memcpy(info.payload.data(), &pid, 4);
                std::memcpy(info.payload.data() + 4, &uid, 4);
            }
            return vm.signals.Queue(signal, info);
        }
        case 16: {
            if (args[1] == 0x5413) {
                struct winsize size{};
                if (ioctl(static_cast<int>(args[0]), TIOCGWINSZ, &size)) return -errno;
                return vm.GuestWrite(args[2], &size, sizeof(size)) ? 0 : -EFAULT;
            }
            if (args[1] == 0x541b) {
                int bytes{};
                if (ioctl(static_cast<int>(args[0]), FIONREAD, &bytes)) return -errno;
                return vm.GuestWrite(args[2], &bytes, sizeof(bytes)) ? 0 : -EFAULT;
            }
            break;
        }
        case 18: {
            if (!memory.Contains(args[1], args[2], PROT_READ)) return -EFAULT;
            if (args[3] > static_cast<u64>(std::numeric_limits<off_t>::max())) return -EINVAL;
            const auto count = HostCall(SYS_pwrite64, args);
            if (count > 0) vm.FileChanged(static_cast<int>(args[0]));
            return count;
        }
        case 20: {
            if (args[2] > 1024 ||
                !memory.Contains(
                        args[1], static_cast<std::size_t>(args[2]) * sizeof(iovec), PROT_READ))
                return -EINVAL;
            const auto count = HostCall(SYS_writev, args);
            if (count > 0) vm.FileChanged(static_cast<int>(args[0]));
            return count;
        }
        case 24:
            return Result(sched_yield());
        case 28:
            if (!memory.Contains(args[0], args[1], 0)) return -ENOMEM;
            if (const auto result = madvise(
                        reinterpret_cast<void*>(args[0]), args[1], static_cast<int>(args[2]));
                result)
                return -errno;
            if (memory.WriteAffectsCode(args[0], args[1])) vm.Invalidate();
            return 0;
        case 39:
            return getpid();
        case 60:
        case 231: {
            if (clear_child_tid) {
                const std::uint32_t zero{};
                vm.GuestWrite(clear_child_tid, &zero, sizeof(zero));
            }
            exited = true;
            exit_code = static_cast<int>(args[0] & 255);
            return 0;
        }
        case 63: {
            struct utsname name{};
            if (uname(&name)) return -errno;
            std::strcpy(name.machine,
                        vm.ISA() == GuestISA::Arm64   ? "aarch64"
                        : vm.ISA() == GuestISA::Arm32 ? "armv7l"
                                                      : "x86_64");
            return vm.GuestWrite(args[0], &name, sizeof(name)) ? 0 : -EFAULT;
        }
        case 72: {
            // Commands with scalar arguments forward directly; pointer-argument
            // commands stay outside the modelled set.
            const auto command = static_cast<int>(args[1]);
            if ((command >= F_GETFD && command <= F_SETFL) || command == F_DUPFD ||
                command == F_DUPFD_CLOEXEC || command == F_SETOWN || command == F_GETOWN)
                return Result(fcntl(static_cast<int>(args[0]), command, args[2]));
            break;
        }
        case 77: {
            if (args[1] > static_cast<u64>(std::numeric_limits<off_t>::max())) return -EINVAL;
            const auto result = ftruncate(static_cast<int>(args[0]), static_cast<off_t>(args[1]));
            if (!result) vm.FileChanged(static_cast<int>(args[0]));
            return Result(result);
        }
        case 96: {
            timeval now{};
            if (gettimeofday(&now, nullptr)) return -errno;
            const s64 value[2]{now.tv_sec, now.tv_usec};
            const int timezone[2]{};
            if (args[0] && !vm.GuestWrite(args[0], value, sizeof(value))) return -EFAULT;
            if (args[1] && !vm.GuestWrite(args[1], timezone, sizeof(timezone))) return -EFAULT;
            return 0;
        }
        case 97: {
            rlimit limit{};
            if (getrlimit(static_cast<decltype(RLIMIT_STACK)>(args[0]), &limit)) return -errno;
            const u64 value[2]{limit.rlim_cur, limit.rlim_max};
            return vm.GuestWrite(args[1], value, sizeof(value)) ? 0 : -EFAULT;
        }
        case 102:
            return getuid();
        case 104:
            return getgid();
        case 107:
            return geteuid();
        case 108:
            return getegid();
        case 143:
        case 145: {
            const auto pid = static_cast<pid_t>(args[0]);
            if (number == 143 && !args[1]) return -EINVAL;
            // This guest has one execution thread. Its existing getpid /
            // gettid identities both refer to that thread, never another
            // task which merely shares the host's PID namespace.
            const auto own_thread = vm.thread.faults.thread_id
                                            ? vm.thread.faults.thread_id
                                            : static_cast<pid_t>(syscall(SYS_gettid));
            if (pid > 0 && pid != getpid() && pid != own_thread) return -ESRCH;
            const auto thread = pid > 0 ? own_thread : pid;
            if (number == 145) return Result(sched_getscheduler(thread));
            sched_param parameter{};
            if (sched_getparam(thread, &parameter)) return -errno;
            // Linux x86-64 sched_param is one signed 32-bit priority;
            // convert the field rather than copying the host structure.
            const std::int32_t priority = parameter.sched_priority;
            return vm.GuestWrite(args[1], &priority, sizeof(priority)) ? 0 : -EFAULT;
        }
        case 157: {
            if (args[0] != PR_SET_VMA || args[1] != PR_SET_VMA_ANON_NAME) return -EINVAL;
            if (!memory.Contains(args[2], args[3], 0)) return -ENOMEM;
            std::string name;
            if (args[4] && !ReadString(args[4], name)) return -EFAULT;
            return Result(prctl(PR_SET_VMA,
                                PR_SET_VMA_ANON_NAME,
                                args[2],
                                args[3],
                                args[4] ? name.c_str() : nullptr));
        }
        case 158: {
            if (vm.ISA() != GuestISA::X86_64) break;
            auto& state = vm.thread.native.state;
            if (args[0] == 0x1001 || args[0] == 0x1002) {
                if (args[1] >= (u64{1} << 48)) return -EPERM;
                (args[0] == 0x1002 ? state.fs_base : state.gs_base) = args[1];
                return 0;
            }
            if (args[0] == 0x1003 || args[0] == 0x1004) {
                const auto value = args[0] == 0x1003 ? state.fs_base : state.gs_base;
                return vm.GuestWrite(args[1], &value, sizeof(value)) ? 0 : -EFAULT;
            }
            return -EINVAL;
        }
        case 186:
            return syscall(SYS_gettid);
        case 201: {
            const auto value = static_cast<s64>(time(nullptr));
            if (args[0] && !vm.GuestWrite(args[0], &value, sizeof(value))) return -EFAULT;
            return value;
        }
        case 202: {
            const auto operation = static_cast<int>(args[1]);
            const auto command = operation & FUTEX_CMD_MASK;
            const bool timed = command == FUTEX_WAIT || command == FUTEX_WAIT_BITSET;
            const bool requeue = command == FUTEX_REQUEUE || command == FUTEX_CMP_REQUEUE;
            if (!timed && !requeue && command != FUTEX_WAKE && command != FUTEX_WAKE_BITSET &&
                command != FUTEX_WAKE_OP)
                break;
            if (!memory.Contains(args[0], sizeof(std::uint32_t), PROT_READ)) return -EFAULT;
            if (command == FUTEX_WAKE_OP) {
                if (!vm.PrepareGuestWrite(args[4], sizeof(std::uint32_t))) return -EFAULT;
            } else if (requeue && !memory.Contains(args[4], sizeof(std::uint32_t), PROT_READ))
                return -EFAULT;
            // Requeue's fourth argument is an integer. WAIT consumes a
            // guest timespec, normalized by HostCall before entering Linux.
            const auto time_bytes = vm.ISA() == GuestISA::Arm32 && !time64_arguments
                                            ? sizeof(abi32::Timespec)
                                            : sizeof(timespec);
            if (timed && args[3] && !memory.Contains(args[3], time_bytes, PROT_READ))
                return -EFAULT;
            if (timed) return HostCall(SYS_futex, args);
            return Result(syscall(SYS_futex, args[0], args[1], args[2], args[3], args[4], args[5]));
        }
        case 204:
#if defined(SYS_sched_getaffinity)
            if (!vm.PrepareGuestWrite(args[2], args[1])) return -EFAULT;
            return Result(syscall(SYS_sched_getaffinity,
                                  static_cast<pid_t>(args[0]),
                                  args[1],
                                  reinterpret_cast<void*>(args[2])));
#else
            break;
#endif
        case 218:
            clear_child_tid = args[0];
            return syscall(SYS_gettid);
        case 228: {
            timespec now{};
            if (clock_gettime(static_cast<clockid_t>(args[0]), &now)) return -errno;
            const s64 value[2]{now.tv_sec, now.tv_nsec};
            return vm.GuestWrite(args[1], value, sizeof(value)) ? 0 : -EFAULT;
        }
        case 2:
        case 21:
        case 89:
        case 257:
        case 262:
        case 269: {
            const u64 pointer = number == 257 || number == 262 || number == 269 ? args[1] : args[0];
            std::string path;
            if (!ReadString(pointer, path)) return -EFAULT;
            if (number == 2) {
                const auto fd =
                        open(path.c_str(), static_cast<int>(args[1]), static_cast<mode_t>(args[2]));
                if (fd >= 0 && (args[1] & O_TRUNC)) vm.FileChanged(fd);
                return Result(fd);
            }
            if (number == 21) return Result(access(path.c_str(), static_cast<int>(args[1])));
            if (number == 269)
                return Result(faccessat(
                        static_cast<int>(args[0]), path.c_str(), static_cast<int>(args[2]), 0));
            if (number == 89) {
                if (!vm.PrepareGuestWrite(args[1], args[2])) return -EFAULT;
                return Result(readlink(path.c_str(), reinterpret_cast<char*>(args[1]), args[2]));
            }
            if (number == 257) {
                const auto fd = openat(static_cast<int>(args[0]),
                                       path.c_str(),
                                       static_cast<int>(args[2]),
                                       static_cast<mode_t>(args[3]));
                if (fd >= 0 && (args[2] & O_TRUNC)) vm.FileChanged(fd);
                return Result(fd);
            }
            return Stat(
                    static_cast<int>(args[0]), args[2], path.c_str(), static_cast<int>(args[3]));
        }
        case 267: {
            std::string path;
            if (!ReadString(args[1], path) || !vm.PrepareGuestWrite(args[2], args[3]))
                return -EFAULT;
            return Result(readlinkat(static_cast<int>(args[0]),
                                     path.c_str(),
                                     reinterpret_cast<char*>(args[2]),
                                     args[3]));
        }
        case 302: {
            if (args[0] || args[2]) break;  // Query-only; no host process-limit mutation.
            rlimit limit{};
            if (getrlimit(static_cast<decltype(RLIMIT_STACK)>(args[1]), &limit)) return -errno;
            const u64 value[2]{limit.rlim_cur, limit.rlim_max};
            return args[3] && !vm.GuestWrite(args[3], value, sizeof(value)) ? -EFAULT : 0;
        }
        case 318:
#if defined(SYS_getrandom)
            if (!vm.PrepareGuestWrite(args[0], args[1])) return -EFAULT;
            return HostCall(SYS_getrandom, args);
#else
            break;
#endif
        default:
            break;
    }
    ++stats.unsupported_syscalls;
    if (options.decode_timing || !reported_unknown) {
        SVM_LOG(Warning,
                Syscall,
                "unsupported guest syscall number={} pc={:#x}; returning ENOSYS",
                current_guest_number,
                vm.PC());
        reported_unknown = true;
    }
    return -ENOSYS;
}

SyscallServiceResult LinuxSyscalls::Service() {
    // The host may write or remap guest memory while servicing a syscall.
    vm.ClearReservation();
    ++vm.stats.syscalls;
    if (vm.ISA() == GuestISA::Arm32) return ServiceArm32();
    if (vm.ISA() == GuestISA::Arm64) {
        auto& state = vm.thread.native.arm_state;
        const u64 arguments[6]{state.gprs[0],
                               state.gprs[1],
                               state.gprs[2],
                               state.gprs[3],
                               state.gprs[4],
                               state.gprs[5]};
        if (state.gprs[8] == 139) {
            if (!vm.signals.ReturnFrame()) {
                GuestExceptionInfo fault;
                fault.isa = GuestISA::Arm64;
                fault.origin = GuestExceptionOrigin::Semantic;
                fault.signal_number = 11;
                fault.instruction_pc = state.pc - 4;
                fault.address = state.gprs[31];
                fault.address_valid = true;
                vm.DeliverException(fault);
            }
            return SyscallServiceResult::ResumeGuestContext;
        }
        const auto number = state.gprs[8];
        resumed_deadline = vm.signals.ConsumeRestartDeadline(number, arguments, restart_deadline);
        current_guest_number = number;
        auto result = Invoke(number, arguments);
        const auto operation = Arm64SyscallOperation(number);
        if (result == CancelledBeforeCall) result = -EINTR;
        state.gprs[0] = static_cast<u64>(result);
        if (result == -EINTR && vm.signals.Pending()) {
            bool with_handler{}, without_handler{};
            RestartPolicy(operation, arguments, with_handler, without_handler);
            vm.signals.InterruptSyscall(number, arguments, with_handler, without_handler);
        }
        return result == -EINTR ? SyscallServiceResult::Interrupted
                                : SyscallServiceResult::Completed;
    }
    auto& state = vm.thread.native.state;
    const u64 arguments[6]{state.gprs[x86::RDI],
                           state.gprs[x86::RSI],
                           state.gprs[x86::RDX],
                           state.gprs[x86::R10],
                           state.gprs[x86::R8],
                           state.gprs[x86::R9]};
    if (state.gprs[x86::RAX] == 15) {
        if (!vm.signals.ReturnFrame()) {
            GuestExceptionInfo fault;
            fault.origin = GuestExceptionOrigin::Semantic;
            fault.signal_number = 11;
            fault.instruction_pc = state.pc - 2;
            fault.address = state.gprs[x86::RSP];
            fault.address_valid = true;
            vm.DeliverException(fault);
        }
        return SyscallServiceResult::ResumeGuestContext;
    }
    const auto number = state.gprs[x86::RAX];
    resumed_deadline = vm.signals.ConsumeRestartDeadline(number, arguments, restart_deadline);
    state.gprs[x86::RCX] = state.pc;
    state.gprs[x86::R11] = x86::flags::ToRflags(state.packed_flags);
    current_guest_number = number;
    auto result = Invoke(number, arguments);
    if (result == CancelledBeforeCall) result = -EINTR;
    state.gprs[x86::RAX] = static_cast<u64>(result);
    if (result == -EINTR && vm.signals.Pending()) {
        bool with_handler{}, without_handler{};
        RestartPolicy(number, arguments, with_handler, without_handler);
        vm.signals.InterruptSyscall(number, arguments, with_handler, without_handler);
    }
    return result == -EINTR ? SyscallServiceResult::Interrupted : SyscallServiceResult::Completed;
}
}  // namespace swift::runtime::platform
