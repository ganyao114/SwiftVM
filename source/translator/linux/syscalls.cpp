//
// Linux syscall emulation — see syscalls.h for the calling convention.
//

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>
#include "base/logging.h"
#include "path_utils.h"
#include "syscalls.h"
#include "translator/x86/cpu.h"

namespace swift::linux {

// Guest mmap flags (asm-generic mman).
static constexpr u64 GUEST_MAP_SHARED = 0x01;
static constexpr u64 GUEST_MAP_PRIVATE = 0x02;
static constexpr u64 GUEST_MAP_FIXED = 0x10;
static constexpr u64 GUEST_MAP_ANONYMOUS = 0x20;

// Guest mprotect protections (asm-generic mman).
static constexpr u64 GUEST_PROT_READ = 0x01;
static constexpr u64 GUEST_PROT_WRITE = 0x02;
static constexpr u64 GUEST_PROT_EXEC = 0x04;

// Guest AT_* constants (asm-generic fcntl). dirfd is an `int` in the Linux
// syscall ABI, so only its low 32 bits are significant. x86_64 callers commonly
// materialize AT_FDCWD with a write to edi, producing the zero-extended u64
// value 0x00000000ffffff9c in the raw syscall register.
static constexpr u64 GUEST_AT_FDCWD = 0xffffff9c;
static constexpr u64 GUEST_AT_SYMLINK_NOFOLLOW = 0x100;
static constexpr u64 GUEST_AT_REMOVEDIR = 0x200;
static constexpr u64 GUEST_AT_EMPTY_PATH = 0x1000;

static bool IsGuestAtFdcwd(u64 dirfd) {
    return (dirfd & UINT32_MAX) == GUEST_AT_FDCWD;
}

// arch_prctl codes (x86_64 only).
static constexpr u64 ARCH_SET_GS = 0x1001;
static constexpr u64 ARCH_SET_FS = 0x1002;
static constexpr u64 ARCH_GET_FS = 0x1003;
static constexpr u64 ARCH_GET_GS = 0x1004;

// futex op codes (after masking FUTEX_PRIVATE_FLAG/FUTEX_CLOCK_REALTIME).
static constexpr u64 FUTEX_WAIT = 0;
static constexpr u64 FUTEX_WAKE = 1;
static constexpr u64 FUTEX_WAIT_BITSET = 9;
static constexpr u64 FUTEX_WAKE_BITSET = 10;
static constexpr u64 FUTEX_CMD_MASK = 0x7f;
static constexpr u64 FUTEX_CLOCK_REALTIME = 0x100;

// clone(2) flags used by glibc/musl pthread_create.
static constexpr u64 CLONE_VM = 0x00000100;
static constexpr u64 CLONE_FS = 0x00000200;
static constexpr u64 CLONE_FILES = 0x00000400;
static constexpr u64 CLONE_SIGHAND = 0x00000800;
static constexpr u64 CLONE_THREAD = 0x00010000;
static constexpr u64 CLONE_SYSVSEM = 0x00040000;
static constexpr u64 CLONE_SETTLS = 0x00080000;
static constexpr u64 CLONE_PARENT_SETTID = 0x00100000;
static constexpr u64 CLONE_CHILD_CLEARTID = 0x00200000;
static constexpr u64 CLONE_DETACHED = 0x00400000;  // obsolete, still passed by musl
static constexpr u64 CLONE_CHILD_SETTID = 0x01000000;
static constexpr u64 SUPPORTED_THREAD_CLONE_FLAGS =
        CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
        CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID |
        CLONE_CHILD_CLEARTID | CLONE_DETACHED | CLONE_CHILD_SETTID;

// mremap flags.
static constexpr u64 MREMAP_MAYMOVE = 1;
static constexpr u64 MREMAP_FIXED = 2;

static constexpr u64 GUEST_RLIM_INFINITY = ~0ULL;

// Guest iovec: same layout as the host one on LP64 (u64 base, u64 len).
struct GuestIovec {
    u64 base;
    u64 len;
};
static_assert(sizeof(GuestIovec) == sizeof(struct iovec));

// 64-bit Linux timespec/timeval (both ISAs).
struct GuestTimespec {
    s64 sec;
    s64 nsec;
};
struct GuestTimeval {
    s64 sec;
    s64 usec;
};

// x86_64 Linux struct stat (arch/x86/include/uapi/asm/stat.h), 144 bytes.
struct GuestStatX64 {
    u64 st_dev;
    u64 st_ino;
    u64 st_nlink;
    u32 st_mode;
    u32 st_uid;
    u32 st_gid;
    u32 pad0;
    u64 st_rdev;
    s64 st_size;
    s64 st_blksize;
    s64 st_blocks;
    s64 atime;
    s64 st_atime_nsec;
    s64 mtime;
    s64 st_mtime_nsec;
    s64 ctime;
    s64 st_ctime_nsec;
    s64 unused[3];
};
static_assert(sizeof(GuestStatX64) == 144);

// asm-generic (AArch64) struct stat (include/uapi/asm-generic/stat.h), 128 bytes.
struct GuestStatArm64 {
    u64 st_dev;
    u64 st_ino;
    u32 st_mode;
    u32 st_nlink;
    u32 st_uid;
    u32 st_gid;
    u64 st_rdev;
    u64 pad1;
    s64 st_size;
    s32 st_blksize;
    s32 pad2;
    s64 st_blocks;
    s64 atime;
    s64 st_atime_nsec;
    s64 mtime;
    s64 st_mtime_nsec;
    s64 ctime;
    s64 st_ctime_nsec;
    u32 unused[2];
};
static_assert(sizeof(GuestStatArm64) == 128);

// struct sysinfo: identical layout on x86_64 and aarch64, 112 bytes.
struct GuestSysinfo {
    s64 uptime;
    u64 loads[3];
    u64 totalram;
    u64 freeram;
    u64 sharedram;
    u64 bufferram;
    u64 totalswap;
    u64 freeswap;
    u16 procs;
    u16 pad;
    u64 totalhigh;
    u64 freehigh;
    u32 mem_unit;
};
static_assert(sizeof(GuestSysinfo) == 112);

struct GuestRlimit {
    u64 cur;
    u64 max;
};

struct GuestTms {
    s64 utime;
    s64 stime;
    s64 cutime;
    s64 cstime;
};

// Translates a host (macOS) errno to a guest (asm-generic) -errno.
static s64 HostErrno() {
    const int e = errno;
    if (e == 0) return 0;
    if (e <= 10) return -e;  // EPERM..ECHILD share values on both ABIs.
    switch (e) {
        case EDEADLK: return -EDEADLK_;    // macOS 11 -> Linux 35
        case EAGAIN: return -EAGAIN_;      // macOS 35 -> Linux 11
        case ENAMETOOLONG: return -ENAMETOOLONG_;
        case ELOOP: return -ELOOP_;
        case ENOSYS: return -ENOSYS_;
        default: break;
    }
    if (e >= 12 && e <= 34) return -e;  // ENOMEM..ERANGE share values.
    return -EINVAL_;
}

// Guest (Linux, both ISAs) O_* -> host (macOS) O_* translation.
static int GuestToHostOpenFlags(u64 g) {
    int h = 0;
    switch (g & 3) {  // O_ACCMODE
        case 0: h |= O_RDONLY; break;
        case 1: h |= O_WRONLY; break;
        case 2: h |= O_RDWR; break;
        default: h |= O_RDONLY; break;
    }
    if (g & 0x40) h |= O_CREAT;
    if (g & 0x80) h |= O_EXCL;
    if (g & 0x200) h |= O_TRUNC;
    if (g & 0x400) h |= O_APPEND;
    if (g & 0x800) h |= O_NONBLOCK;
    if (g & 0x10000) h |= O_DIRECTORY;
    if (g & 0x20000) h |= O_NOFOLLOW;
    if (g & 0x80000) h |= O_CLOEXEC;
    // O_NOCTTY/O_LARGEFILE/O_DSYNC/O_SYNC/O_DIRECT/O_NOATIME: no host
    // equivalent we care about; ignored.
    return h;
}

static u64 HostToGuestOpenFlags(int h) {
    u64 g = static_cast<u64>(h) & 3;  // access mode bits match
    if (h & O_CREAT) g |= 0x40;
    if (h & O_EXCL) g |= 0x80;
    if (h & O_TRUNC) g |= 0x200;
    if (h & O_APPEND) g |= 0x400;
    if (h & O_NONBLOCK) g |= 0x800;
    return g;
}

// Maps an x86_64 syscall number onto the canonical (asm-generic) numbering.
// Returns the raw number unchanged when there is no mapping (it will fall
// into the -ENOSYS default below).
static u64 X86ToCanonical(u64 nr) {
    switch (nr) {
        case X64_read: return SYS_read;
        case X64_write: return SYS_write;
        case X64_open: return SYS_x64_open;
        case X64_close: return SYS_close;
        case X64_stat: return SYS_x64_stat;
        case X64_fstat: return SYS_fstat;
        case X64_lstat: return SYS_x64_lstat;
        case X64_lseek: return SYS_lseek;
        case X64_readv: return SYS_readv;
        case X64_writev: return SYS_writev;
        case X64_ioctl: return SYS_ioctl;
        case X64_pread64: return SYS_pread64;
        case X64_pwrite64: return SYS_pwrite64;
        case X64_access: return SYS_x64_access;
        case X64_mremap: return SYS_mremap;
        case X64_madvise: return SYS_madvise;
        case X64_dup: return SYS_dup;
        case X64_dup2: return SYS_x64_dup2;
        case X64_nanosleep: return SYS_nanosleep;
        case X64_exit: return SYS_exit;
        case X64_exit_group: return SYS_exit_group;
        case X64_brk: return SYS_brk;
        case X64_mmap: return SYS_mmap;
        case X64_munmap: return SYS_munmap;
        case X64_mprotect: return SYS_mprotect;
        case X64_rt_sigaction: return SYS_rt_sigaction;
        case X64_rt_sigprocmask: return SYS_rt_sigprocmask;
        case X64_fcntl: return SYS_fcntl;
        case X64_getcwd: return SYS_getcwd;
        case X64_unlink: return SYS_x64_unlink;
        case X64_readlink: return SYS_x64_readlink;
        case X64_gettimeofday: return SYS_gettimeofday;
        case X64_sysinfo: return SYS_sysinfo;
        case X64_times: return SYS_times;
        case X64_uname: return SYS_uname;
        case X64_clock_gettime: return SYS_clock_gettime;
        case X64_clock_nanosleep: return SYS_clock_nanosleep;
        case X64_arch_prctl: return SYS_x64_arch_prctl;
        case X64_time: return SYS_x64_time;
        case X64_futex: return SYS_futex;
        case X64_sched_getaffinity: return SYS_sched_getaffinity;
        case X64_getdents64: return SYS_getdents64;
        case X64_set_tid_address: return SYS_set_tid_address;
        case X64_tgkill: return SYS_tgkill;
        case X64_openat: return SYS_openat;
        case X64_newfstatat: return SYS_newfstatat;
        case X64_unlinkat: return SYS_unlinkat;
        case X64_readlinkat: return SYS_readlinkat;
        case X64_faccessat: return SYS_faccessat;
        case X64_set_robust_list: return SYS_set_robust_list;
        case X64_pipe2: return SYS_pipe2;
        case X64_prlimit64: return SYS_prlimit64;
        case X64_getrandom: return SYS_getrandom;
        case X64_rseq: return SYS_rseq;
        case X64_faccessat2: return SYS_faccessat2;
        case X64_getpid: return SYS_getpid;
        case X64_clone: return SYS_clone;
        case X64_gettid: return SYS_gettid;
        case X64_getuid: return SYS_getuid;
        case X64_geteuid: return SYS_geteuid;
        case X64_getgid: return SYS_getgid;
        case X64_getegid: return SYS_getegid;
        default: return nr;
    }
}

SyscallProcessState::SyscallProcessState(GuestMemory* memory, VAddr brk_base)
        : memory(memory)
        , brk_base(brk_base)
        , brk_current(brk_base)
        , brk_mapped_end(GuestMemory::RoundHostPage(brk_base)) {}

bool SyscallProcessState::StoreGuestU32(u64 uaddr, u32 value) {
    if ((uaddr & (alignof(u32) - 1)) != 0 || !memory->RangeIsMapped(uaddr, sizeof(u32))) {
        return false;
    }
    auto* word = static_cast<u32*>(memory->ToHost(uaddr));
    std::atomic_ref<u32>(*word).store(value, std::memory_order_release);
    return true;
}

GuestSigAction SyscallProcessState::GetSignalAction(u64 signal) {
    std::lock_guard guard(signal_mutex);
    return signal_actions[signal];
}

void SyscallProcessState::SetSignalAction(u64 signal, const GuestSigAction& action) {
    std::lock_guard guard(signal_mutex);
    signal_actions[signal] = action;
}

s64 SyscallProcessState::WakeFutex(u64 uaddr, u32 count) {
    return WakeFutexMasked(uaddr, count, UINT32_MAX);
}

s64 SyscallProcessState::WakeFutexMasked(u64 uaddr, u32 count, u32 bitset) {
    if ((uaddr & (alignof(u32) - 1)) != 0) {
        return -EINVAL_;
    }
    if (!memory->RangeIsMapped(uaddr, sizeof(u32))) {
        return -EFAULT_;
    }
    std::lock_guard guard(futex_mutex);
    auto it = futex_queues.find(uaddr);
    if (it == futex_queues.end() || count == 0) {
        return 0;
    }
    auto& waiters = it->second.waiters;
    size_t wake_count = 0;
    for (auto waiter = waiters.begin(); waiter != waiters.end() && wake_count < count;) {
        if (((*waiter)->bitset & bitset) == 0) {
            ++waiter;
            continue;
        }
        auto selected = *waiter;
        waiter = waiters.erase(waiter);
        selected->woken = true;
        selected->cv.notify_one();
        ++wake_count;
    }
    if (waiters.empty()) {
        futex_queues.erase(it);
    }
    return static_cast<s64>(wake_count);
}

void SyscallProcessState::WakeAllFutexesLocked() {
    for (auto& [address, queue] : futex_queues) {
        for (auto& waiter : queue.waiters) {
            waiter->woken = true;
            waiter->cv.notify_one();
        }
        queue.waiters.clear();
    }
    futex_queues.clear();
}

void SyscallProcessState::RequestExitGroup(u8 code) {
    exit_code.store(code, std::memory_order_release);
    exiting.store(true, std::memory_order_release);
    std::lock_guard guard(futex_mutex);
    WakeAllFutexesLocked();
}

s64 SyscallProcessState::Futex(u64 uaddr,
                               u64 op,
                               u64 val,
                               u64 timeout,
                               u64 uaddr2,
                               u64 val3) {
    (void) uaddr2;
    (void) val3;
    const u64 cmd = op & FUTEX_CMD_MASK;
    if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
        const u32 bitset = cmd == FUTEX_WAKE_BITSET ? static_cast<u32>(val3) : UINT32_MAX;
        if (bitset == 0) {
            return -EINVAL_;
        }
        return WakeFutexMasked(
                uaddr, static_cast<u32>(std::min<u64>(val, UINT32_MAX)), bitset);
    }
    if (cmd != FUTEX_WAIT && cmd != FUTEX_WAIT_BITSET) {
        LOG_WARNING("futex op {} not supported, returning -ENOSYS", cmd);
        return -ENOSYS_;
    }
    if ((uaddr & (alignof(u32) - 1)) != 0) {
        return -EINVAL_;
    }
    if (!memory->RangeIsMapped(uaddr, sizeof(u32))) {
        return -EFAULT_;
    }

    GuestTimespec guest_timeout{};
    if (timeout) {
        if (!memory->TryRead(timeout, guest_timeout)) {
            return -EFAULT_;
        }
        if (guest_timeout.sec < 0 || guest_timeout.nsec < 0 ||
            guest_timeout.nsec >= 1'000'000'000) {
            return -EINVAL_;
        }
    }

    std::unique_lock lock(futex_mutex);
    // The value check and waiter insertion are serialized against WAKE by the
    // same lock. Guest writes do not take this lock, but the aligned atomic
    // load gives the Linux val32 check semantics and avoids a C++ data race.
    auto* word = static_cast<u32*>(memory->ToHost(uaddr));
    const u32 current = std::atomic_ref<u32>(*word).load(std::memory_order_acquire);
    if (current != static_cast<u32>(val)) {
        return -EAGAIN_;
    }
    if (IsExiting()) {
        return 0;
    }

    const u32 bitset = cmd == FUTEX_WAIT_BITSET ? static_cast<u32>(val3) : UINT32_MAX;
    if (bitset == 0) {
        return -EINVAL_;
    }
    auto waiter = std::make_shared<FutexWaiter>();
    waiter->bitset = bitset;
    futex_queues[uaddr].waiters.push_back(waiter);
    bool woke = true;
    if (!timeout) {
        waiter->cv.wait(lock, [&] { return waiter->woken || IsExiting(); });
    } else {
        const auto duration = std::chrono::seconds(guest_timeout.sec) +
                              std::chrono::nanoseconds(guest_timeout.nsec);
        if (cmd == FUTEX_WAIT_BITSET) {
            // WAIT_BITSET uses an absolute deadline. CLOCK_REALTIME selects
            // system_clock; otherwise Linux uses CLOCK_MONOTONIC.
            if (op & FUTEX_CLOCK_REALTIME) {
                const auto deadline = std::chrono::system_clock::time_point(
                        std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                duration));
                woke = waiter->cv.wait_until(
                        lock, deadline, [&] { return waiter->woken || IsExiting(); });
            } else {
                const auto deadline = std::chrono::steady_clock::time_point(
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                duration));
                woke = waiter->cv.wait_until(
                        lock, deadline, [&] { return waiter->woken || IsExiting(); });
            }
        } else {
            woke = waiter->cv.wait_for(
                    lock, duration, [&] { return waiter->woken || IsExiting(); });
        }
    }
    if (!waiter->woken) {
        auto it = futex_queues.find(uaddr);
        if (it != futex_queues.end()) {
            std::erase(it->second.waiters, waiter);
            if (it->second.waiters.empty()) {
                futex_queues.erase(it);
            }
        }
    }
    return woke || IsExiting() ? 0 : -ETIMEDOUT_;
}

SyscallHandler::Result SyscallHandler::Handle(u64 nr,
                                              u64 a0,
                                              u64 a1,
                                              u64 a2,
                                              u64 a3,
                                              u64 a4,
                                              u64 a5) {
    const u64 raw_nr = nr;
    if (isa == GuestISA::kX86_64) {
        nr = X86ToCanonical(nr);
    }
    Result result{};
    switch (nr) {
        case SYS_read:
            result.ret = SysRead(a0, a1, a2);
            break;
        case SYS_write:
            result.ret = SysWrite(a0, a1, a2);
            break;
        case SYS_readv:
            result.ret = SysReadv(a0, a1, a2);
            break;
        case SYS_writev:
            result.ret = SysWritev(a0, a1, a2);
            break;
        case SYS_exit:
            result.ret = 0;
            result.exited = true;
            result.exit_code = static_cast<u8>(a0);
            break;
        case SYS_exit_group:
            result.ret = 0;
            result.exited = true;
            result.exit_group = true;
            result.exit_code = static_cast<u8>(a0);
            process->RequestExitGroup(result.exit_code);
            break;
        case SYS_clone:
            result.ret = SysClone(a0, a1, a2, a3, a4);
            break;
        case SYS_brk:
            result.ret = SysBrk(a0);
            break;
        case SYS_mmap:
            result.ret = SysMmap(a0, a1, a2, a3, static_cast<s64>(a4), a5);
            break;
        case SYS_munmap:
            result.ret = SysMunmap(a0, a1);
            break;
        case SYS_mprotect:
            result.ret = SysMprotect(a0, a1, a2);
            break;
        case SYS_mremap:
            result.ret = SysMremap(a0, a1, a2, a3, a4);
            break;
        case SYS_madvise:
            // Advisory only; always succeeds.
            result.ret = 0;
            break;
        case SYS_uname:
            result.ret = SysUname(a0);
            break;
        case SYS_clock_gettime:
            result.ret = SysClockGettime(a0, a1);
            break;
        case SYS_gettimeofday:
            result.ret = SysGettimeofday(a0, a1);
            break;
        case SYS_x64_time:
            result.ret = SysTime(a0);
            break;
        case SYS_nanosleep:
            result.ret = SysNanosleep(a0, a1);
            break;
        case SYS_clock_nanosleep:
            result.ret = SysClockNanosleep(a0, a1, a2, a3);
            break;
        case SYS_times:
            result.ret = SysTimes(a0);
            break;
        case SYS_sched_getaffinity:
            result.ret = SysSchedGetaffinity(a0, a1, a2);
            break;
        case SYS_rt_sigaction:
            result.ret = SysRtSigaction(a0, a1, a2, a3);
            break;
        case SYS_rt_sigprocmask:
            result.ret = SysRtSigprocmask(a0, a1, a2, a3);
            break;
        case SYS_sysinfo:
            result.ret = SysSysinfo(a0);
            break;
        case SYS_openat:
            result.ret = SysOpenat(a0, a1, a2, a3);
            break;
        case SYS_x64_open:
            result.ret = SysOpenat(GUEST_AT_FDCWD, a0, a1, a2);
            break;
        case SYS_close:
            result.ret = SysClose(a0);
            break;
        case SYS_lseek:
            result.ret = SysLseek(a0, a1, a2);
            break;
        case SYS_pread64:
            result.ret = SysPread64(a0, a1, a2, a3);
            break;
        case SYS_pwrite64:
            result.ret = SysPwrite64(a0, a1, a2, a3);
            break;
        case SYS_fstat:
            result.ret = SysFstat(a0, a1);
            break;
        case SYS_newfstatat:
            result.ret = SysFstatat(a0, a1, a2, a3);
            break;
        case SYS_x64_stat:
            result.ret = SysFstatat(GUEST_AT_FDCWD, a0, a1, 0);
            break;
        case SYS_x64_lstat:
            result.ret = SysFstatat(GUEST_AT_FDCWD, a0, a1, GUEST_AT_SYMLINK_NOFOLLOW);
            break;
        case SYS_faccessat:
            result.ret = SysFaccessat(a0, a1, a2, 0);
            break;
        case SYS_faccessat2:
            result.ret = SysFaccessat(a0, a1, a2, a3);
            break;
        case SYS_x64_access:
            result.ret = SysFaccessat(GUEST_AT_FDCWD, a0, a1, 0);
            break;
        case SYS_readlinkat:
            result.ret = SysReadlinkat(a0, a1, a2, a3);
            break;
        case SYS_x64_readlink:
            result.ret = SysReadlinkat(GUEST_AT_FDCWD, a0, a1, a2);
            break;
        case SYS_unlinkat:
            result.ret = SysUnlinkat(a0, a1, a2);
            break;
        case SYS_x64_unlink:
            result.ret = SysUnlinkat(GUEST_AT_FDCWD, a0, 0);
            break;
        case SYS_getcwd:
            result.ret = SysGetcwd(a0, a1);
            break;
        case SYS_fcntl:
            result.ret = SysFcntl(a0, a1, a2);
            break;
        case SYS_dup:
            result.ret = SysDup(a0);
            break;
        case SYS_x64_dup2:
            result.ret = SysDup2(a0, a1);
            break;
        case SYS_ioctl:
            result.ret = SysIoctl(a0, a1, a2);
            break;
        case SYS_futex:
            result.ret = SysFutex(a0, a1, a2, a3, a4, a5);
            break;
        case SYS_x64_arch_prctl:
            result.ret = SysArchPrctl(a0, a1);
            break;
        case SYS_set_tid_address:
            clear_child_tid = a0;
            result.ret = tid;
            break;
        case SYS_set_robust_list:
            // Single-threaded: just remember the list head.
            robust_list_head = a0;
            robust_list_len = a1;
            result.ret = 0;
            break;
        case SYS_rseq:
            // No rseq support: glibc/musl both handle -ENOSYS gracefully.
            result.ret = -ENOSYS_;
            break;
        case SYS_prlimit64:
            result.ret = SysPrlimit64(a0, a1, a2, a3);
            break;
        case SYS_getrandom:
            result.ret = SysGetrandom(a0, a1, a2);
            break;
        case SYS_tgkill: {
            // No guest signal delivery. For signals whose default action is
            // a fatal terminate (abort() path goes through tgkill), end the
            // guest with the conventional 128+sig status; otherwise the
            // signal is dropped and the call succeeds.
            const u64 sig = a2;
            switch (sig) {
                case 4:   // SIGILL
                case 6:   // SIGABRT
                case 7:   // SIGBUS
                case 8:   // SIGFPE
                case 9:   // SIGKILL
                case 11:  // SIGSEGV
                    result.ret = 0;
                    result.exited = true;
                    result.exit_code = static_cast<u8>(128 + sig);
                    break;
                default:
                    result.ret = 0;
                    break;
            }
            break;
        }
        case SYS_pipe2:
            LOG_WARNING("pipe2() not supported, returning -ENOSYS");
            result.ret = -ENOSYS_;
            break;
        case SYS_getdents64:
            LOG_WARNING("getdents64() not supported, returning -ENOSYS");
            result.ret = -ENOSYS_;
            break;
        case SYS_getpid:
            result.ret = 1000;
            break;
        case SYS_gettid:
            result.ret = tid;
            break;
        case SYS_getuid:
        case SYS_geteuid:
        case SYS_getgid:
        case SYS_getegid:
            result.ret = 1000;
            break;
        default:
            LOG_WARNING("Unimplemented guest syscall nr {} (args {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}), "
                        "returning -ENOSYS",
                        raw_nr,
                        a0,
                        a1,
                        a2,
                        a3,
                        a4,
                        a5);
            result.ret = -ENOSYS_;
            break;
    }
    return result;
}

s64 SyscallHandler::SysRead(u64 fd, u64 buf, u64 count) {
    if (count == 0) return 0;
    if (!memory->RangeIsMapped(buf, count)) return -EFAULT_;
    auto ret = ::read(static_cast<int>(fd), memory->ToHost(buf), count);
    return ret < 0 ? HostErrno() : ret;
}

s64 SyscallHandler::SysWrite(u64 fd, u64 buf, u64 count) {
    if (count == 0) return 0;
    if (!memory->RangeIsMapped(buf, count)) return -EFAULT_;
    auto ret = ::write(static_cast<int>(fd), memory->ToHostConst(buf), count);
    return ret < 0 ? HostErrno() : ret;
}

s64 SyscallHandler::SysReadv(u64 fd, u64 iov, u64 iovcnt) {
    if (iovcnt == 0) return 0;
    if (iovcnt > 1024) return -EINVAL_;
    std::vector<GuestIovec> giov(iovcnt);
    if (!memory->TryReadBytes(iov, {reinterpret_cast<u8*>(giov.data()), iovcnt * sizeof(GuestIovec)})) {
        return -EFAULT_;
    }
    std::vector<struct iovec> hiov(iovcnt);
    for (size_t i = 0; i < iovcnt; ++i) {
        if (!memory->RangeIsMapped(giov[i].base, giov[i].len)) return -EFAULT_;
        hiov[i].iov_base = memory->ToHost(giov[i].base);
        hiov[i].iov_len = giov[i].len;
    }
    auto ret = ::readv(static_cast<int>(fd), hiov.data(), static_cast<int>(iovcnt));
    return ret < 0 ? HostErrno() : ret;
}

s64 SyscallHandler::SysWritev(u64 fd, u64 iov, u64 iovcnt) {
    if (iovcnt == 0) return 0;
    if (iovcnt > 1024) return -EINVAL_;
    std::vector<GuestIovec> giov(iovcnt);
    if (!memory->TryReadBytes(iov, {reinterpret_cast<u8*>(giov.data()), iovcnt * sizeof(GuestIovec)})) {
        return -EFAULT_;
    }
    std::vector<struct iovec> hiov(iovcnt);
    for (size_t i = 0; i < iovcnt; ++i) {
        if (!memory->RangeIsMapped(giov[i].base, giov[i].len)) return -EFAULT_;
        hiov[i].iov_base = memory->ToHost(giov[i].base);
        hiov[i].iov_len = giov[i].len;
    }
    auto ret = ::writev(static_cast<int>(fd), hiov.data(), static_cast<int>(iovcnt));
    return ret < 0 ? HostErrno() : ret;
}

s64 SyscallHandler::SysBrk(u64 addr) {
    std::lock_guard guard(process->memory_mutex);
    if (addr == 0 || addr < process->brk_base) {
        return static_cast<s64>(process->brk_current);
    }
    // Refuse to grow unreasonably far past the image (1 GiB heap ceiling);
    // brk_base is a guest address (the image's linked end), so an absolute
    // ceiling would be wrong.
    if (addr >= process->brk_base + (1ull << 30)) {
        return static_cast<s64>(process->brk_current);
    }
    const VAddr new_mapped_end = GuestMemory::RoundHostPage(addr);
    if (new_mapped_end > process->brk_mapped_end) {
        if (!memory->MapFixed(process->brk_mapped_end,
                              new_mapped_end - process->brk_mapped_end)) {
            return static_cast<s64>(process->brk_current);
        }
        process->brk_mapped_end = new_mapped_end;
    }
    process->brk_current = addr;
    return static_cast<s64>(process->brk_current);
}

s64 SyscallHandler::SysMmap(u64 addr, u64 length, u64 prot, u64 flags, s64 fd, u64 offset) {
    std::lock_guard guard(process->memory_mutex);
    if (length == 0) return -EINVAL_;
    if (offset % GuestMemory::kGuestPageSize != 0) return -EINVAL_;
    const bool anonymous = (flags & GUEST_MAP_ANONYMOUS) != 0;
    if (!anonymous) {
        // File-backed mappings use anonymous guest storage populated with
        // pread. MAP_PRIVATE has snapshot semantics naturally. A read-only
        // MAP_SHARED mapping is also safe as a snapshot because the guest
        // cannot dirty it; writable shared mappings still need real writeback
        // and coherence support.
        const bool private_mapping = (flags & GUEST_MAP_PRIVATE) != 0;
        const bool readonly_shared =
                (flags & GUEST_MAP_SHARED) != 0 &&
                (prot & GUEST_PROT_WRITE) == 0;
        const char* shared_enabled = std::getenv("SVM_SYSCALL_MMAP_SHARED_READ");
        const bool shared_disabled =
                shared_enabled && std::strcmp(shared_enabled, "0") == 0;
        if (!private_mapping && (!readonly_shared || shared_disabled)) {
            LOG_WARNING(
                    "guest mmap: writable/disabled shared file mapping not supported "
                    "(fd {}, prot {:#x}, flags {:#x})",
                    fd,
                    prot,
                    flags);
            return -ENOSYS_;
        }
        if (fd < 0) return -EBADF_;
    }
    const u64 map_length = GuestMemory::RoundHostPage(length);

    VAddr guest_addr = 0;
    if (flags & GUEST_MAP_FIXED) {
        if (addr % GuestMemory::kGuestPageSize != 0 || addr + length < addr) {
            return -EINVAL_;
        }
        // Linux x86_64 has 4 KiB guest pages while macOS/arm64 can only map
        // 16 KiB host pages. A fixed ELF segment commonly begins in the
        // middle of an already-backed host page (libc's second PT_LOAD begins
        // at +0x26000). Preserve that page and replace only the requested
        // guest bytes; mapping the whole host page again would destroy the
        // tail of the preceding segment.
        const VAddr host_start = GuestMemory::RoundDownHostPage(addr);
        const VAddr host_end = GuestMemory::RoundHostPage(addr + length);
        if (smc_invalidate_) smc_invalidate_(addr, addr + length);
        for (VAddr page = host_start; page < host_end;
             page += GuestMemory::kHostPageSize) {
            if (!memory->RangeIsMapped(page, GuestMemory::kHostPageSize) &&
                !memory->MapFixed(page, GuestMemory::kHostPageSize)) {
                return -ENOMEM_;
            }
        }
        guest_addr = addr;
    } else if (addr != 0 && addr % GuestMemory::kHostPageSize == 0) {
        // Address hint: honor it if the range is free, else fall back.
        if (!memory->RangeIsMapped(addr, map_length) && memory->MapFixed(addr, map_length)) {
            guest_addr = addr;
        }
    }
    if (!guest_addr) {
        // No (usable) hint: let the host pick a free range. Deliberately no
        // fixed bump allocator — a fixed guest address can collide with
        // host allocations (JIT code cache, ...) and clobbering them is
        // fatal.
        guest_addr = memory->MapAnywhere(map_length);
    }
    if (!guest_addr) return -ENOMEM_;

    // Anonymous MAP_FIXED must clear an already-backed subpage. File mappings
    // also need zero-fill after EOF. New MapAnywhere/MapFixed pages are
    // already zeroed, so doing this uniformly is harmless and makes the
    // replacement semantics explicit.
    std::memset(memory->ToHost(guest_addr), 0, static_cast<size_t>(length));
    if (!anonymous) {
        u64 done = 0;
        while (done < length) {
            auto r = ::pread(static_cast<int>(fd),
                             memory->ToHost(guest_addr + done),
                             length - done,
                             static_cast<off_t>(offset + done));
            if (r < 0) {
                const s64 err = HostErrno();
                memory->Unmap(guest_addr, map_length);
                return err;
            }
            if (r == 0) break;  // EOF: rest stays zero-filled.
            done += static_cast<u64>(r);
        }
    }
    return static_cast<s64>(guest_addr);
}

s64 SyscallHandler::SysMunmap(u64 addr, u64 length) {
    std::lock_guard guard(process->memory_mutex);
    if (addr % GuestMemory::kGuestPageSize != 0) return -EINVAL_;
    if (length == 0) return -EINVAL_;
    // SMC: the unmapped range may hold translated code — invalidate it.
    if (smc_invalidate_) smc_invalidate_(addr, addr + length);
    memory->Unmap(addr, length);
    return 0;
}

s64 SyscallHandler::SysMprotect(u64 addr, u64 len, u64 prot) {
    std::lock_guard guard(process->memory_mutex);
    if (len == 0 || addr % GuestMemory::kGuestPageSize != 0) return -EINVAL_;
    if ((prot & ~(GUEST_PROT_READ | GUEST_PROT_WRITE | GUEST_PROT_EXEC)) != 0) {
        return -EINVAL_;
    }
    const u64 guest_len = GuestMemory::RoundGuestPage(len);
    if (addr + guest_len < addr) return -EINVAL_;
    if (!memory->RangeIsMapped(addr, guest_len)) return -ENOMEM_;
    const bool host_page_exact =
        addr % GuestMemory::kHostPageSize == 0 &&
        (addr + guest_len) % GuestMemory::kHostPageSize == 0;
    if (host_page_exact) {
        if (!memory->Protect(addr,
                             guest_len,
                             (prot & GUEST_PROT_READ) != 0,
                             (prot & GUEST_PROT_WRITE) != 0,
                             (prot & GUEST_PROT_EXEC) != 0)) {
            return HostErrno();
        }
    } else {
        // Linux protects 4 KiB guest pages, but macOS/arm64 can only protect
        // complete 16 KiB host pages. Expanding a RELRO range would also make
        // writable neighboring guest subpages read-only, so leave the range
        // unenforced and report success. Those RELRO neighbors consequently
        // remain writable; this is guest-visible only to code that would fault
        // on real hardware. A future software permission table can enforce
        // guest-page permissions without changing adjacent host subpages.
    }
    // SMC: PROT_WRITE on a range that holds translated code means the guest
    // is about to patch it — invalidate any stale blocks now, before the
    // write happens (the write-protect trap may not fire on an already-writable
    // host page).
    if ((prot & GUEST_PROT_WRITE) && smc_invalidate_) {
        smc_invalidate_(addr, addr + len);
    }
    return 0;
}

s64 SyscallHandler::SysMremap(u64 addr, u64 old_size, u64 new_size, u64 flags, u64 new_addr) {
    std::lock_guard guard(process->memory_mutex);
    if (new_size == 0) return -EINVAL_;
    if (old_size == 0) return -EINVAL_;
    if (!memory->RangeIsMapped(addr, old_size)) return -EFAULT_;
    if (new_size <= old_size) {
        // Shrink (or same size) in place: keep the tail mapped, it is
        // harmless for the guest.
        return static_cast<s64>(addr);
    }
    if (!(flags & MREMAP_MAYMOVE)) return -ENOMEM_;
    const u64 new_len = GuestMemory::RoundHostPage(new_size);
    VAddr out = 0;
    if (flags & MREMAP_FIXED) {
        if (new_addr % GuestMemory::kHostPageSize != 0) return -EINVAL_;
        // SMC: the fixed target range may hold translated code — invalidate it.
        if (smc_invalidate_) smc_invalidate_(new_addr, new_addr + new_len);
        memory->Unmap(new_addr, new_len);  // MREMAP_FIXED replaces the target range
        if (!memory->MapFixed(new_addr, new_len)) return -ENOMEM_;
        out = new_addr;
    } else {
        out = memory->MapAnywhere(new_len);
        if (!out) return -ENOMEM_;
    }
    std::memcpy(memory->ToHost(out), memory->ToHostConst(addr), old_size);
    // SMC: the old range is about to be unmapped — invalidate any translated
    // code there before dropping the mapping.
    if (smc_invalidate_) smc_invalidate_(addr, addr + GuestMemory::RoundHostPage(old_size));
    memory->Unmap(addr, GuestMemory::RoundHostPage(old_size));
    return static_cast<s64>(out);
}

s64 SyscallHandler::SysUname(u64 buf) {
    // struct utsname: 6 x 65-byte fields.
    static constexpr char fields[][65] = {
            "Linux",           // sysname
            "swiftvm",         // nodename
            "6.6.0-swiftvm",   // release
            "#1 SwiftVM",      // version
            "",                // machine (filled per-ISA below)
            "(none)",          // domainname
    };
    u8 uts[6 * 65]{};
    for (size_t i = 0; i < 6; ++i) {
        std::memcpy(uts + i * 65, fields[i], std::strlen(fields[i]) + 1);
    }
    const char* machine = isa == GuestISA::kX86_64 ? "x86_64" : "aarch64";
    std::memcpy(uts + 4 * 65, machine, std::strlen(machine) + 1);
    if (!memory->TryWriteBytes(buf, {uts, sizeof(uts)})) return -EFAULT_;
    return 0;
}

s64 SyscallHandler::SysClockGettime(u64 clock_id, u64 ts) {
    // Guest (Linux) clock ids differ from macOS: translate the common ones.
    clockid_t host_id;
    switch (clock_id) {
        case 0: host_id = CLOCK_REALTIME; break;
        case 1: host_id = CLOCK_MONOTONIC; break;
        case 2: host_id = CLOCK_PROCESS_CPUTIME_ID; break;
        case 3: host_id = CLOCK_THREAD_CPUTIME_ID; break;
        case 4: host_id = CLOCK_MONOTONIC_RAW_APPROX; break;  // CLOCK_MONOTONIC_RAW
        case 6: host_id = CLOCK_UPTIME_RAW_APPROX; break;     // CLOCK_BOOTTIME
        default: return -EINVAL_;
    }
    struct timespec host_ts {};
    if (::clock_gettime(host_id, &host_ts) != 0) {
        return HostErrno();
    }
    GuestTimespec gts{host_ts.tv_sec, host_ts.tv_nsec};
    if (!memory->TryWrite(ts, gts)) return -EFAULT_;
    return 0;
}

s64 SyscallHandler::SysGettimeofday(u64 tv, u64 tz) {
    if (tv) {
        struct timeval htv {};
        if (::gettimeofday(&htv, nullptr) != 0) return HostErrno();
        GuestTimeval gtv{htv.tv_sec, htv.tv_usec};
        if (!memory->TryWrite(tv, gtv)) return -EFAULT_;
    }
    if (tz) {
        // struct timezone { int minuteswest, dsttime }: report UTC.
        s32 zero[2] = {0, 0};
        if (!memory->TryWriteBytes(tz, {reinterpret_cast<const u8*>(zero), sizeof(zero)})) {
            return -EFAULT_;
        }
    }
    return 0;
}

s64 SyscallHandler::SysTime(u64 tloc) {
    struct timespec host_ts {};
    if (::clock_gettime(CLOCK_REALTIME, &host_ts) != 0) {
        return HostErrno();
    }
    const s64 seconds = host_ts.tv_sec;
    if (tloc && !memory->TryWrite(tloc, seconds)) {
        return -EFAULT_;
    }
    return seconds;
}

s64 SyscallHandler::SysNanosleep(u64 req_addr, u64 rem_addr) {
    GuestTimespec req{};
    if (!memory->TryRead(req_addr, req)) return -EFAULT_;
    struct timespec hts{req.sec, req.nsec};
    ::nanosleep(&hts, nullptr);  // EINTR/residual rem ignored: no signals.
    return 0;
}

s64 SyscallHandler::SysClockNanosleep(u64 clock_id,
                                      u64 flags,
                                      u64 req_addr,
                                      u64 rem_addr) {
    (void) rem_addr;  // No delivered guest signals, so no EINTR remainder.
    if ((flags & ~u64{1}) != 0) return -EINVAL_;  // TIMER_ABSTIME

    clockid_t host_clock;
    switch (clock_id) {
        case 0: host_clock = CLOCK_REALTIME; break;
        case 1: host_clock = CLOCK_MONOTONIC; break;
        default: return -EINVAL_;
    }

    GuestTimespec req{};
    if (!memory->TryRead(req_addr, req)) return -EFAULT_;
    if (req.sec < 0 || req.nsec < 0 || req.nsec >= 1'000'000'000) {
        return -EINVAL_;
    }

    struct timespec delay{req.sec, req.nsec};
    if (flags & 1) {
        struct timespec now {};
        if (::clock_gettime(host_clock, &now) != 0) return HostErrno();
        if (req.sec < now.tv_sec ||
            (req.sec == now.tv_sec && req.nsec <= now.tv_nsec)) {
            return 0;
        }
        delay.tv_sec = req.sec - now.tv_sec;
        delay.tv_nsec = req.nsec - now.tv_nsec;
        if (delay.tv_nsec < 0) {
            --delay.tv_sec;
            delay.tv_nsec += 1'000'000'000;
        }
    }

    while (::nanosleep(&delay, &delay) != 0) {
        if (errno != EINTR) return HostErrno();
    }
    return 0;
}

s64 SyscallHandler::SysTimes(u64 buf) {
    GuestTms tms{};  // no per-thread CPU accounting: report zeros
    if (!memory->TryWrite(buf, tms)) return -EFAULT_;
    return 0;
}

s64 SyscallHandler::SysSchedGetaffinity(u64 pid, u64 cpusetsize, u64 mask) {
    (void) pid;  // SwiftVM exposes one virtual process containing all guest tids.
    const long host_count = ::sysconf(_SC_NPROCESSORS_ONLN);
    const u64 cpu_count = host_count > 0 ? static_cast<u64>(host_count) : 1;
    const u64 result_size = std::max<u64>(sizeof(u64), ((cpu_count + 63) / 64) * sizeof(u64));
    if (cpusetsize < result_size) return -EINVAL_;
    if (!memory->RangeIsMapped(mask, result_size)) return -EFAULT_;

    auto* guest_mask = static_cast<u8*>(memory->ToHost(mask));
    std::memset(guest_mask, 0, static_cast<size_t>(result_size));
    for (u64 cpu = 0; cpu < cpu_count; ++cpu) {
        guest_mask[cpu / 8] |= static_cast<u8>(1u << (cpu % 8));
    }
    return static_cast<s64>(result_size);
}

s64 SyscallHandler::SysRtSigaction(u64 signal,
                                   u64 act_addr,
                                   u64 oldact_addr,
                                   u64 sigset_size) {
    if (const char* enabled = std::getenv("SVM_SYSCALL_RT_SIGACTION");
        enabled && std::strcmp(enabled, "0") == 0) {
        return -ENOSYS_;
    }
    if (signal == 0 || signal > 64 || sigset_size != sizeof(u64)) {
        return -EINVAL_;
    }
    if (act_addr && (signal == 9 || signal == 19)) {  // SIGKILL / SIGSTOP
        return -EINVAL_;
    }

    GuestSigAction action{};
    if (act_addr && !memory->TryRead(act_addr, action)) {
        return -EFAULT_;
    }
    if (oldact_addr) {
        const GuestSigAction old_action = process->GetSignalAction(signal);
        if (!memory->TryWrite(oldact_addr, old_action)) {
            return -EFAULT_;
        }
    }
    if (act_addr) {
        // Signal delivery is not wired into the translated execution loop.
        // Keep the Linux-visible disposition so registration/query semantics
        // work; single-threaded benchmarks do not require asynchronous delivery.
        process->SetSignalAction(signal, action);
    }
    return 0;
}

s64 SyscallHandler::SysRtSigprocmask(u64 how,
                                     u64 set_addr,
                                     u64 oldset_addr,
                                     u64 sigset_size) {
    if (const char* enabled = std::getenv("SVM_SYSCALL_RT_SIGPROCMASK");
        enabled && std::strcmp(enabled, "0") == 0) {
        return -ENOSYS_;
    }
    if (sigset_size != sizeof(u64)) return -EINVAL_;

    u64 requested{};
    if (set_addr && !memory->TryRead(set_addr, requested)) {
        return -EFAULT_;
    }

    u64 next = signal_mask;
    if (set_addr) {
        constexpr u64 unmaskable =
                (u64{1} << (9 - 1)) |   // SIGKILL
                (u64{1} << (19 - 1));   // SIGSTOP
        requested &= ~unmaskable;
        switch (how) {
            case 0: next |= requested; break;   // SIG_BLOCK
            case 1: next &= ~requested; break;  // SIG_UNBLOCK
            case 2: next = requested; break;    // SIG_SETMASK
            default: return -EINVAL_;
        }
    }

    if (oldset_addr && !memory->TryWrite(oldset_addr, signal_mask)) {
        return -EFAULT_;
    }
    if (set_addr) {
        // SwiftVM currently has no asynchronous guest-signal delivery. Keep
        // the per-thread Linux-visible mask for libc and clone inheritance.
        signal_mask = next;
    }
    return 0;
}

s64 SyscallHandler::SysSysinfo(u64 buf) {
    GuestSysinfo si{};
    struct timespec ts {};
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        si.uptime = ts.tv_sec;
    }
    si.totalram = 16ull << 30;
    si.freeram = 8ull << 30;
    si.mem_unit = 1;
    si.procs = 100;
    if (!memory->TryWrite(buf, si)) return -EFAULT_;
    return 0;
}

s64 SyscallHandler::SysOpenat(u64 dirfd, u64 path, u64 flags, u64 mode) {
    std::string p;
    if (!memory->TryReadCString(path, p)) return -EFAULT_;
    const bool absolute = !p.empty() && p.front() == '/';
    p = ResolveGuestPath(p);
    const int hflags = GuestToHostOpenFlags(flags);
    int ret;
    if (IsGuestAtFdcwd(dirfd) || absolute) {
        ret = ::open(p.c_str(), hflags, static_cast<mode_t>(mode & 07777));
    } else {
        ret = ::openat(static_cast<int>(dirfd), p.c_str(), hflags, static_cast<mode_t>(mode & 07777));
    }
    return ret < 0 ? HostErrno() : ret;
}

s64 SyscallHandler::SysClose(u64 fd) {
    auto ret = ::close(static_cast<int>(fd));
    return ret < 0 ? HostErrno() : ret;
}

s64 SyscallHandler::SysLseek(u64 fd, u64 offset, u64 whence) {
    auto ret = ::lseek(static_cast<int>(fd), static_cast<off_t>(offset), static_cast<int>(whence));
    return ret < 0 ? HostErrno() : ret;
}

s64 SyscallHandler::SysPread64(u64 fd, u64 buf, u64 count, u64 offset) {
    if (count == 0) return 0;
    if (!memory->RangeIsMapped(buf, count)) return -EFAULT_;
    auto ret = ::pread(static_cast<int>(fd),
                       memory->ToHost(buf),
                       count,
                       static_cast<off_t>(offset));
    return ret < 0 ? HostErrno() : ret;
}

s64 SyscallHandler::SysPwrite64(u64 fd, u64 buf, u64 count, u64 offset) {
    if (offset > static_cast<u64>(INT64_MAX)) return -EINVAL_;
    if (count == 0) return 0;
    if (!memory->RangeIsMapped(buf, count)) return -EFAULT_;
    auto ret = ::pwrite(static_cast<int>(fd),
                        memory->ToHostConst(buf),
                        count,
                        static_cast<off_t>(offset));
    return ret < 0 ? HostErrno() : ret;
}

s64 SyscallHandler::SysFstat(u64 fd, u64 statbuf) {
    struct stat hst {};
    if (::fstat(static_cast<int>(fd), &hst) != 0) return HostErrno();
    return WriteGuestStat(statbuf, hst);
}

s64 SyscallHandler::SysFstatat(u64 dirfd, u64 path, u64 statbuf, u64 flags) {
    struct stat hst {};
    if (flags & GUEST_AT_EMPTY_PATH) {
        // Empty path: operate on dirfd itself.
        if (::fstat(static_cast<int>(dirfd), &hst) != 0) return HostErrno();
        return WriteGuestStat(statbuf, hst);
    }
    std::string p;
    if (!memory->TryReadCString(path, p)) return -EFAULT_;
    const bool absolute = !p.empty() && p.front() == '/';
    p = ResolveGuestPath(p);
    int ret;
    if (IsGuestAtFdcwd(dirfd) || absolute) {
        ret = (flags & GUEST_AT_SYMLINK_NOFOLLOW) ? ::lstat(p.c_str(), &hst) : ::stat(p.c_str(), &hst);
    } else {
        ret = ::fstatat(static_cast<int>(dirfd),
                        p.c_str(),
                        &hst,
                        (flags & GUEST_AT_SYMLINK_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW : 0);
    }
    if (ret != 0) return HostErrno();
    return WriteGuestStat(statbuf, hst);
}

s64 SyscallHandler::SysFaccessat(u64 dirfd, u64 path, u64 mode, u64 flags) {
    std::string p;
    if (!memory->TryReadCString(path, p)) return -EFAULT_;
    const bool absolute = !p.empty() && p.front() == '/';
    p = ResolveGuestPath(p);
    if (flags) {
        // AT_EACCESS / AT_SYMLINK_NOFOLLOW semantics are ignored; the
        // emulated process has euid == uid anyway.
        LOG_WARNING("faccessat: flags {:#x} ignored", flags);
    }
    int ret;
    if (IsGuestAtFdcwd(dirfd) || absolute) {
        ret = ::access(p.c_str(), static_cast<int>(mode));
    } else {
        ret = ::faccessat(static_cast<int>(dirfd), p.c_str(), static_cast<int>(mode), 0);
    }
    return ret < 0 ? HostErrno() : ret;
}

s64 SyscallHandler::SysReadlinkat(u64 dirfd, u64 path, u64 buf, u64 bufsize) {
    std::string p;
    if (!memory->TryReadCString(path, p)) return -EFAULT_;
    std::string target;
    if (p == "/proc/self/exe") {
        // Emulated /proc: the running guest binary.
        target = exe_path.empty() ? "/swiftvm/guest" : exe_path;
    } else {
        const bool absolute = !p.empty() && p.front() == '/';
        p = ResolveGuestPath(p);
        if (!IsGuestAtFdcwd(dirfd) && !absolute) {
            LOG_WARNING("readlinkat with dirfd {} not supported (path {})", static_cast<s64>(dirfd), p);
            return -ENOSYS_;
        }
        std::vector<char> tmp(std::min<u64>(bufsize, 4096));
        auto n = ::readlink(p.c_str(), tmp.data(), tmp.size());
        if (n < 0) return HostErrno();
        target.assign(tmp.data(), static_cast<size_t>(n));
    }
    const size_t n = std::min(target.size(), static_cast<size_t>(bufsize));
    if (n && !memory->TryWriteBytes(buf, {reinterpret_cast<const u8*>(target.data()), n})) {
        return -EFAULT_;
    }
    return static_cast<s64>(n);
}

s64 SyscallHandler::SysUnlinkat(u64 dirfd, u64 path, u64 flags) {
    std::string p;
    if (!memory->TryReadCString(path, p)) return -EFAULT_;
    const bool absolute = !p.empty() && p.front() == '/';
    p = ResolveGuestPath(p);
    int ret;
    if (IsGuestAtFdcwd(dirfd) || absolute) {
        ret = (flags & GUEST_AT_REMOVEDIR) ? ::rmdir(p.c_str()) : ::unlink(p.c_str());
    } else {
        ret = ::unlinkat(static_cast<int>(dirfd),
                         p.c_str(),
                         (flags & GUEST_AT_REMOVEDIR) ? AT_REMOVEDIR : 0);
    }
    return ret < 0 ? HostErrno() : ret;
}

s64 SyscallHandler::SysGetcwd(u64 buf, u64 size) {
    // The emulated process always sits at the filesystem root.
    static constexpr char kCwd[] = "/";
    if (size < sizeof(kCwd)) return -ERANGE_;
    if (!memory->TryWriteBytes(buf, {reinterpret_cast<const u8*>(kCwd), sizeof(kCwd)})) {
        return -EFAULT_;
    }
    return sizeof(kCwd);
}

s64 SyscallHandler::SysFcntl(u64 fd, u64 cmd, u64 arg) {
    const int ifd = static_cast<int>(fd);
    switch (cmd) {
        case 0: {  // F_DUPFD
            auto ret = ::fcntl(ifd, F_DUPFD, static_cast<int>(arg));
            return ret < 0 ? HostErrno() : ret;
        }
        case 1: {  // F_GETFD
            auto ret = ::fcntl(ifd, F_GETFD);
            return ret < 0 ? HostErrno() : ret;
        }
        case 2: {  // F_SETFD (flag values match the host)
            auto ret = ::fcntl(ifd, F_SETFD, static_cast<int>(arg));
            return ret < 0 ? HostErrno() : ret;
        }
        case 3: {  // F_GETFL
            auto ret = ::fcntl(ifd, F_GETFL);
            return ret < 0 ? HostErrno() : static_cast<s64>(HostToGuestOpenFlags(static_cast<int>(ret)));
        }
        case 4: {  // F_SETFL (only the settable status flags)
            auto ret = ::fcntl(ifd, F_SETFL, GuestToHostOpenFlags(arg) & (O_APPEND | O_NONBLOCK));
            return ret < 0 ? HostErrno() : ret;
        }
        case 5:   // F_GETLK
        case 6:   // F_SETLK
        case 7:   // F_SETLKW
            // Single-threaded process: no lock contention is possible.
            return 0;
        default:
            LOG_WARNING("fcntl cmd {} not supported, returning -ENOSYS", cmd);
            return -ENOSYS_;
    }
}

s64 SyscallHandler::SysDup(u64 fd) {
    auto ret = ::dup(static_cast<int>(fd));
    return ret < 0 ? HostErrno() : ret;
}

s64 SyscallHandler::SysDup2(u64 oldfd, u64 newfd) {
    if (oldfd == newfd) {
        // dup2 with equal fds validates and returns the fd without closing.
        if (::fcntl(static_cast<int>(oldfd), F_GETFD) < 0) return -EBADF_;
        return static_cast<s64>(newfd);
    }
    auto ret = ::dup2(static_cast<int>(oldfd), static_cast<int>(newfd));
    return ret < 0 ? HostErrno() : ret;
}

s64 SyscallHandler::SysIoctl(u64 fd, u64 request, u64 arg) {
    // We emulate no terminal (and no other ioctl-aware device): ENOTTY is
    // the answer glibc/musl expect for isatty()-style probing of a pipe or
    // regular file.
    LOG_INFO("guest ioctl(fd {}, request {:#x}) -> -ENOTTY", fd, request);
    return -ENOTTY_;
}

s64 SyscallHandler::SysFutex(u64 uaddr, u64 op, u64 val, u64 timeout, u64 uaddr2, u64 val3) {
    return process->Futex(uaddr, op, val, timeout, uaddr2, val3);
}

s64 SyscallHandler::SysClone(u64 flags,
                             u64 child_stack,
                             u64 parent_tid,
                             u64 child_tid,
                             u64 tls) {
    const u64 required = CLONE_VM | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD;
    if ((flags & required) != required || (flags & ~SUPPORTED_THREAD_CLONE_FLAGS) != 0) {
        LOG_WARNING("clone flags {:#x} are not a supported pthread thread clone", flags);
        return -EINVAL_;
    }
    if (!child_stack || !clone_callback_) {
        return -EINVAL_;
    }
    if ((flags & CLONE_PARENT_SETTID) &&
        !memory->RangeIsMapped(parent_tid, sizeof(u32))) {
        return -EFAULT_;
    }
    if ((flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) &&
        !memory->RangeIsMapped(child_tid, sizeof(u32))) {
        return -EFAULT_;
    }
    return clone_callback_(
            CloneRequest{flags, child_stack, parent_tid, child_tid, tls, signal_mask});
}

s64 SyscallHandler::SysArchPrctl(u64 code, u64 addr) {
    // Write through to the frontend context so fs:/gs: segment overrides
    // resolve against these bases (ThreadContext64::fs_base/gs_base).
    auto* ctx = static_cast<swift::x86::ThreadContext64*>(x86_ctx);
    switch (code) {
        case ARCH_SET_FS:
            fs_base = addr;
            if (ctx) ctx->fs_base = addr;
            return 0;
        case ARCH_SET_GS:
            gs_base = addr;
            if (ctx) ctx->gs_base = addr;
            return 0;
        case ARCH_GET_FS:
            return memory->TryWrite(addr, fs_base) ? 0 : -EFAULT_;
        case ARCH_GET_GS:
            return memory->TryWrite(addr, gs_base) ? 0 : -EFAULT_;
        default:
            // Unknown codes (e.g. ARCH_SET_CPUID = 0x3001) get EINVAL,
            // which glibc handles gracefully.
            return -EINVAL_;
    }
}

s64 SyscallHandler::SysPrlimit64(u64 pid, u64 resource, u64 new_rlim, u64 old_rlim) {
    if (old_rlim) {
        GuestRlimit lim{GUEST_RLIM_INFINITY, GUEST_RLIM_INFINITY};
        switch (resource) {
            case 3:  // RLIMIT_STACK
                lim.cur = 8ull << 20;
                break;
            case 7:  // RLIMIT_NOFILE
                lim.cur = 1024;
                lim.max = 1024;
                break;
            default:
                break;
        }
        if (!memory->TryWrite(old_rlim, lim)) return -EFAULT_;
    }
    // new_rlim: accept and ignore (no limit enforcement).
    return 0;
}

s64 SyscallHandler::SysGetrandom(u64 buf, u64 buflen, u64 flags) {
    if (buflen > (1u << 20)) return -EINVAL_;
    std::vector<u8> tmp(buflen);
    arc4random_buf(tmp.data(), tmp.size());
    if (!tmp.empty() && !memory->TryWriteBytes(buf, tmp)) return -EFAULT_;
    return static_cast<s64>(buflen);
}

s64 SyscallHandler::WriteGuestStat(u64 guest_buf, const struct stat& h) {
    if (isa == GuestISA::kX86_64) {
        GuestStatX64 s{};
        s.st_dev = h.st_dev;
        s.st_ino = h.st_ino;
        s.st_nlink = h.st_nlink;
        s.st_mode = h.st_mode;
        s.st_uid = h.st_uid;
        s.st_gid = h.st_gid;
        s.st_rdev = h.st_rdev;
        s.st_size = h.st_size;
        s.st_blksize = h.st_blksize;
        s.st_blocks = h.st_blocks;
        s.atime = h.st_atimespec.tv_sec;
        s.st_atime_nsec = h.st_atimespec.tv_nsec;
        s.mtime = h.st_mtimespec.tv_sec;
        s.st_mtime_nsec = h.st_mtimespec.tv_nsec;
        s.ctime = h.st_ctimespec.tv_sec;
        s.st_ctime_nsec = h.st_ctimespec.tv_nsec;
        return memory->TryWrite(guest_buf, s) ? 0 : -EFAULT_;
    }
    GuestStatArm64 s{};
    s.st_dev = h.st_dev;
    s.st_ino = h.st_ino;
    s.st_mode = h.st_mode;
    s.st_nlink = h.st_nlink;
    s.st_uid = h.st_uid;
    s.st_gid = h.st_gid;
    s.st_rdev = h.st_rdev;
    s.st_size = h.st_size;
    s.st_blksize = h.st_blksize;
    s.st_blocks = h.st_blocks;
    s.atime = h.st_atimespec.tv_sec;
    s.st_atime_nsec = h.st_atimespec.tv_nsec;
    s.mtime = h.st_mtimespec.tv_sec;
    s.st_mtime_nsec = h.st_mtimespec.tv_nsec;
    s.ctime = h.st_ctimespec.tv_sec;
    s.st_ctime_nsec = h.st_ctimespec.tv_nsec;
    return memory->TryWrite(guest_buf, s) ? 0 : -EFAULT_;
}

}  // namespace swift::linux
