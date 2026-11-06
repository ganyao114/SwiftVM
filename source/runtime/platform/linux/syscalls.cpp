#include "syscalls.h"
#include "runtime/execution/vm.h"
#include "runtime/arch/x86/flags.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <linux/futex.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

namespace swift::runtime::platform {
namespace {
using u64 = std::uint64_t; using s64 = std::int64_t;
using namespace ::swift::runtime::x86;
s64 Result(s64 result) { return result < 0 ? -errno : result; }
struct GuestStat {
    u64 device, inode, links;
    std::uint32_t mode, uid, gid, padding;
    u64 rdevice;
    s64 size, block_size, blocks, atime; u64 atime_nsec;
    s64 mtime; u64 mtime_nsec;
    s64 ctime; u64 ctime_nsec;
    s64 unused[3];
};
static_assert(sizeof(GuestStat) == 144);

}
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
        const GuestStat guest{static_cast<u64>(status.st_dev), static_cast<u64>(status.st_ino), status.st_nlink,
                status.st_mode, status.st_uid, status.st_gid, 0, static_cast<u64>(status.st_rdev), status.st_size,
                status.st_blksize, status.st_blocks, status.st_atim.tv_sec, static_cast<u64>(status.st_atim.tv_nsec),
                status.st_mtim.tv_sec, static_cast<u64>(status.st_mtim.tv_nsec), status.st_ctim.tv_sec,
                static_cast<u64>(status.st_ctim.tv_nsec), {0, 0, 0}};
        return vm.GuestWrite(output, &guest, sizeof(guest)) ? 0 : -EFAULT;
    }
s64 LinuxSyscalls::Invoke(u64 number, const u64* args) {
        auto& memory = vm.memory; auto& stats = vm.stats; auto& options = vm.options;
        auto& state = vm.thread.state; auto& clear_child_tid = vm.thread.clear_child_tid;
        auto& exited = vm.thread.exited; auto& exit_code = vm.thread.exit_code;
        switch (number) {
            case 0:
                if (!vm.PrepareGuestWrite(args[1], args[2])) return -EFAULT;
                return Result(read(static_cast<int>(args[0]), reinterpret_cast<void*>(args[1]), args[2]));
            case 1: {
                if (!memory.Contains(args[1], args[2], PROT_READ)) return -EFAULT;
                const auto count = write(static_cast<int>(args[0]), reinterpret_cast<const void*>(args[1]), args[2]);
                if (count > 0) vm.FileChanged(static_cast<int>(args[0]));
                return Result(count);
            }
            case 3: return Result(close(static_cast<int>(args[0])));
            case 5: return Stat(static_cast<int>(args[0]), args[1]);
            case 8: return Result(lseek(static_cast<int>(args[0]), static_cast<off_t>(args[1]), static_cast<int>(args[2])));
            case 9: {
                constexpr u64 mapping_flags = MAP_SHARED | MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS |
                        MAP_DENYWRITE | MAP_EXECUTABLE | MAP_NORESERVE;
                if ((args[2] & ~u64{7}) || (args[3] & ~mapping_flags)) return -EINVAL;
                const bool invalidate = (args[3] & MAP_FIXED) && memory.WriteAffectsCode(args[0], args[1]);
                const auto address = memory.Map(args[0], args[1], static_cast<int>(args[2]), static_cast<int>(args[3]),
                        static_cast<int>(args[4]), args[5]);
                if (address == UINT64_MAX) return -errno;
                if (invalidate || (args[2] & PROT_EXEC) ||
                    ((args[2] & PROT_WRITE) && memory.WriteAffectsCode(address, args[1]))) vm.Invalidate();
                return static_cast<s64>(address);
            }
            case 10: {
                if (args[2] & ~u64{7}) return -EINVAL;
                const bool invalidate = memory.WriteAffectsCode(args[0], args[1]) || (args[2] & PROT_EXEC);
                const auto result = memory.Protect(args[0], args[1], static_cast<int>(args[2]));
                if (!result && invalidate) vm.Invalidate(); return Result(result);
            }
            case 11: {
                const bool invalidate = memory.WriteAffectsCode(args[0], args[1]);
                const auto result = memory.Unmap(args[0], args[1]); if (!result && invalidate) vm.Invalidate(); return Result(result);
            }
            case 12: {
                const auto previous = memory.Brk(0);
                if (args[0] && args[0] != previous &&
                    memory.WriteAffectsCode(std::min(previous, args[0]), std::max(previous, args[0]) - std::min(previous, args[0])))
                    vm.Invalidate();
                return static_cast<s64>(memory.Brk(args[0]));
            }
            case 16: {
                if (args[1] == 0x5413) {
                    struct winsize size{}; if (ioctl(static_cast<int>(args[0]), TIOCGWINSZ, &size)) return -errno;
                    return vm.GuestWrite(args[2], &size, sizeof(size)) ? 0 : -EFAULT;
                }
                if (args[1] == 0x541b) {
                    int bytes{}; if (ioctl(static_cast<int>(args[0]), FIONREAD, &bytes)) return -errno;
                    return vm.GuestWrite(args[2], &bytes, sizeof(bytes)) ? 0 : -EFAULT;
                }
                break;
            }
            case 18: {
                if (!memory.Contains(args[1], args[2], PROT_READ)) return -EFAULT;
                if (args[3] > static_cast<u64>(std::numeric_limits<off_t>::max())) return -EINVAL;
                const auto count = pwrite(static_cast<int>(args[0]), reinterpret_cast<const void*>(args[1]), args[2],
                        static_cast<off_t>(args[3]));
                if (count > 0) vm.FileChanged(static_cast<int>(args[0]));
                return Result(count);
            }
            case 20: {
                if (args[2] > 1024 || !memory.Contains(args[1], static_cast<std::size_t>(args[2]) * sizeof(iovec), PROT_READ)) return -EINVAL;
                const auto count = writev(static_cast<int>(args[0]), reinterpret_cast<const iovec*>(args[1]), static_cast<int>(args[2]));
                if (count > 0) vm.FileChanged(static_cast<int>(args[0]));
                return Result(count);
            }
            case 24: return Result(sched_yield());
            case 28:
                if (!memory.Contains(args[0], args[1], 0)) return -ENOMEM;
                if (const auto result = madvise(reinterpret_cast<void*>(args[0]), args[1], static_cast<int>(args[2])); result) return -errno;
                if (memory.WriteAffectsCode(args[0], args[1])) vm.Invalidate(); return 0;
            case 39: return getpid();
            case 60: case 231: {
                if (clear_child_tid) { const std::uint32_t zero{}; vm.GuestWrite(clear_child_tid, &zero, sizeof(zero)); }
                exited = true; exit_code = static_cast<int>(args[0] & 255); return 0;
            }
            case 63: {
                struct utsname name{}; if (uname(&name)) return -errno;
                std::strcpy(name.machine, "x86_64");
                return vm.GuestWrite(args[0], &name, sizeof(name)) ? 0 : -EFAULT;
            }
            case 72:
                if (args[1] >= F_GETFD && args[1] <= F_SETFL)
                    return Result(fcntl(static_cast<int>(args[0]), static_cast<int>(args[1]), args[2]));
                break;
            case 77: {
                if (args[1] > static_cast<u64>(std::numeric_limits<off_t>::max())) return -EINVAL;
                const auto result = ftruncate(static_cast<int>(args[0]), static_cast<off_t>(args[1]));
                if (!result) vm.FileChanged(static_cast<int>(args[0]));
                return Result(result);
            }
            case 96: {
                timeval now{}; if (gettimeofday(&now, nullptr)) return -errno;
                const s64 value[2]{now.tv_sec, now.tv_usec}; const int timezone[2]{};
                if (args[0] && !vm.GuestWrite(args[0], value, sizeof(value))) return -EFAULT;
                if (args[1] && !vm.GuestWrite(args[1], timezone, sizeof(timezone))) return -EFAULT;
                return 0;
            }
            case 102: return getuid(); case 104: return getgid(); case 107: return geteuid(); case 108: return getegid();
            case 143: case 145: {
                const auto pid = static_cast<pid_t>(args[0]);
                if (number == 143 && !args[1]) return -EINVAL;
                // This guest has one execution thread. Its existing getpid /
                // gettid identities both refer to that thread, never another
                // task which merely shares the host's PID namespace.
                const auto own_thread = vm.thread.faults.thread_id
                        ? vm.thread.faults.thread_id : static_cast<pid_t>(syscall(SYS_gettid));
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
                return Result(prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, args[2], args[3],
                        args[4] ? name.c_str() : nullptr));
            }
            case 158:
                if (args[0] == 0x1001 || args[0] == 0x1002) {
                    if (args[1] >= (u64{1} << 48)) return -EPERM;
                    (args[0] == 0x1002 ? state.fs_base : state.gs_base) = args[1]; return 0;
                }
                if (args[0] == 0x1003 || args[0] == 0x1004) {
                    const auto value = args[0] == 0x1003 ? state.fs_base : state.gs_base;
                    return vm.GuestWrite(args[1], &value, sizeof(value)) ? 0 : -EFAULT;
                }
                return -EINVAL;
            case 186: return syscall(SYS_gettid);
            case 201: { const auto value = static_cast<s64>(time(nullptr)); if (args[0] && !vm.GuestWrite(args[0], &value, sizeof(value))) return -EFAULT; return value; }
            case 202: {
                const auto operation = static_cast<int>(args[1]);
                const auto command = operation & FUTEX_CMD_MASK;
                const bool timed = command == FUTEX_WAIT || command == FUTEX_WAIT_BITSET;
                const bool requeue = command == FUTEX_REQUEUE || command == FUTEX_CMP_REQUEUE;
                if (!timed && !requeue && command != FUTEX_WAKE && command != FUTEX_WAKE_BITSET &&
                    command != FUTEX_WAKE_OP) break;
                if (!memory.Contains(args[0], sizeof(std::uint32_t), PROT_READ)) return -EFAULT;
                if (command == FUTEX_WAKE_OP) {
                    if (!vm.PrepareGuestWrite(args[4], sizeof(std::uint32_t))) return -EFAULT;
                } else if (requeue && !memory.Contains(args[4], sizeof(std::uint32_t), PROT_READ)) return -EFAULT;
                // Both kernel ABIs use a 32-bit word and two signed 64-bit
                // timespec fields. Requeue's fourth argument is an integer,
                // whereas WAIT/WAIT_BITSET consume an optional time pointer.
                if (timed && args[3] && !memory.Contains(args[3], sizeof(timespec), PROT_READ)) return -EFAULT;
                return Result(syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(args[0]), operation, args[2],
                        args[3], reinterpret_cast<std::uint32_t*>(args[4]), args[5]));
            }
            case 204:
#if defined(SYS_sched_getaffinity)
                if (!vm.PrepareGuestWrite(args[2], args[1])) return -EFAULT;
                return Result(syscall(SYS_sched_getaffinity, static_cast<pid_t>(args[0]), args[1],
                        reinterpret_cast<void*>(args[2])));
#else
                break;
#endif
            case 218: clear_child_tid = args[0]; return syscall(SYS_gettid);
            case 228: {
                timespec now{}; if (clock_gettime(static_cast<clockid_t>(args[0]), &now)) return -errno;
                const s64 value[2]{now.tv_sec, now.tv_nsec}; return vm.GuestWrite(args[1], value, sizeof(value)) ? 0 : -EFAULT;
            }
            case 2: case 21: case 89: case 257: case 262: case 269: {
                const u64 pointer = number == 257 || number == 262 || number == 269 ? args[1] : args[0];
                std::string path; if (!ReadString(pointer, path)) return -EFAULT;
                if (number == 2) {
                    const auto fd = open(path.c_str(), static_cast<int>(args[1]), static_cast<mode_t>(args[2]));
                    if (fd >= 0 && (args[1] & O_TRUNC)) vm.FileChanged(fd);
                    return Result(fd);
                }
                if (number == 21) return Result(access(path.c_str(), static_cast<int>(args[1])));
                if (number == 269)
                    return Result(faccessat(static_cast<int>(args[0]), path.c_str(), static_cast<int>(args[2]), 0));
                if (number == 89) {
                    if (!vm.PrepareGuestWrite(args[1], args[2])) return -EFAULT;
                    return Result(readlink(path.c_str(), reinterpret_cast<char*>(args[1]), args[2]));
                }
                if (number == 257) {
                    const auto fd = openat(static_cast<int>(args[0]), path.c_str(), static_cast<int>(args[2]), static_cast<mode_t>(args[3]));
                    if (fd >= 0 && (args[2] & O_TRUNC)) vm.FileChanged(fd);
                    return Result(fd);
                }
                return Stat(static_cast<int>(args[0]), args[2], path.c_str(), static_cast<int>(args[3]));
            }
            case 302: {
                if (args[0] || args[2]) break; // Query-only; no host process-limit mutation.
                rlimit limit{}; if (getrlimit(static_cast<decltype(RLIMIT_STACK)>(args[1]), &limit)) return -errno;
                const u64 value[2]{limit.rlim_cur, limit.rlim_max}; return args[3] && !vm.GuestWrite(args[3], value, sizeof(value)) ? -EFAULT : 0;
            }
            case 318:
#if defined(SYS_getrandom)
                if (!vm.PrepareGuestWrite(args[0], args[1])) return -EFAULT;
                return Result(syscall(SYS_getrandom, reinterpret_cast<void*>(args[0]), args[1], args[2]));
#else
                break;
#endif
            default: break;
        }
        ++stats.unsupported_syscalls;
        if (options.decode_timing || !reported_unknown) {
            std::fprintf(stderr, "unsupported x86-64 syscall %llu at pc=0x%llx; returning ENOSYS\n",
                    static_cast<unsigned long long>(number), static_cast<unsigned long long>(state.pc)); reported_unknown = true;
        }
        return -ENOSYS;
    }
void LinuxSyscalls::Service() {
    auto& state = vm.thread.state;
    ++vm.stats.syscalls;
    const u64 arguments[6]{state.gprs[x86::RDI], state.gprs[x86::RSI], state.gprs[x86::RDX],
                          state.gprs[x86::R10], state.gprs[x86::R8], state.gprs[x86::R9]};
    state.gprs[x86::RCX] = state.pc;
    state.gprs[x86::R11] = x86::flags::ToRflags(state.packed_flags);
    state.gprs[x86::RAX] = static_cast<u64>(Invoke(state.gprs[x86::RAX], arguments));
}
} // namespace swift::runtime::platform
