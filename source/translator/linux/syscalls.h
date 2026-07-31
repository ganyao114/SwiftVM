//
// Linux syscall emulation for the guest process (ARM64 and x86_64).
//
// ARM64 convention (shared with the ARM64 frontend/backend, see
// translator/arm64/cpu.h and translator/arm64/translator.h):
//   - the guest executes `svc #0`;
//   - the frontend stores the whole ThreadContext64 into the uniform buffer,
//     sets pc to the instruction *after* the svc and halts the runtime with
//     HaltReason::CallHost;
//   - Arm64Core::Run() returns ExitReason::Syscall; the handler below reads:
//         x8      = syscall number
//         x0..x5  = arguments
//     and the caller writes the returned s64 back into x0
//     (negative values are -errno, as on Linux), then re-enters Run().
//
// x86_64 convention: the guest executes `syscall`; the frontend surfaces it
// the same way (CallHost, rip = next instruction). The handler reads:
//         rax              = syscall number
//         rdi/rsi/rdx/r10/r8/r9 = arguments
//     and the caller writes the result back into rax.
//
// Syscall *numbers* differ between the two ISAs: the handler normalizes the
// guest number to the asm-generic (AArch64) numbering via the GuestISA it
// was constructed with, so the emulation code itself is ISA-agnostic.
//

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include "base/types.h"
#include "guest_memory.h"
#include "loader.h"

#if defined(__linux__)
// glibc exposes host syscall numbers as SYS_* macros. This header deliberately
// uses the same names for the guest's canonical AArch64 numbering, so remove
// the host spellings after all system headers have been included. Callers of
// this guest-emulation interface must not mix in host SYS_* constants.
#undef SYS_getcwd
#undef SYS_dup
#undef SYS_fcntl
#undef SYS_ioctl
#undef SYS_unlinkat
#undef SYS_ftruncate
#undef SYS_faccessat
#undef SYS_openat
#undef SYS_close
#undef SYS_pipe2
#undef SYS_getdents64
#undef SYS_lseek
#undef SYS_read
#undef SYS_write
#undef SYS_readv
#undef SYS_writev
#undef SYS_pread64
#undef SYS_pwrite64
#undef SYS_readlinkat
#undef SYS_newfstatat
#undef SYS_fstat
#undef SYS_fsync
#undef SYS_fdatasync
#undef SYS_exit
#undef SYS_exit_group
#undef SYS_set_tid_address
#undef SYS_futex
#undef SYS_set_robust_list
#undef SYS_nanosleep
#undef SYS_clock_gettime
#undef SYS_clock_nanosleep
#undef SYS_sched_getaffinity
#undef SYS_tgkill
#undef SYS_rt_sigaction
#undef SYS_rt_sigprocmask
#undef SYS_times
#undef SYS_uname
#undef SYS_umask
#undef SYS_gettimeofday
#undef SYS_getpid
#undef SYS_getuid
#undef SYS_geteuid
#undef SYS_getgid
#undef SYS_getegid
#undef SYS_gettid
#undef SYS_sysinfo
#undef SYS_clone
#undef SYS_brk
#undef SYS_munmap
#undef SYS_mremap
#undef SYS_mmap
#undef SYS_mprotect
#undef SYS_madvise
#undef SYS_prlimit64
#undef SYS_getrandom
#undef SYS_rseq
#undef SYS_faccessat2
#endif

namespace swift::linux {

// Canonical syscall numbering (asm-generic unistd == AArch64). x86_64
// numbers are translated to these in Handle().
enum GuestSyscall : u64 {
    SYS_getcwd = 17,
    SYS_dup = 23,
    SYS_fcntl = 25,
    SYS_ioctl = 29,
    SYS_unlinkat = 35,
    SYS_ftruncate = 46,
    SYS_faccessat = 48,
    SYS_openat = 56,
    SYS_close = 57,
    SYS_pipe2 = 59,
    SYS_getdents64 = 61,
    SYS_lseek = 62,
    SYS_read = 63,
    SYS_write = 64,
    SYS_readv = 65,
    SYS_writev = 66,
    SYS_pread64 = 67,
    SYS_pwrite64 = 68,
    SYS_readlinkat = 78,
    SYS_newfstatat = 79,
    SYS_fstat = 80,
    SYS_fsync = 82,
    SYS_fdatasync = 83,
    SYS_exit = 93,
    SYS_exit_group = 94,
    SYS_set_tid_address = 96,
    SYS_futex = 98,
    SYS_set_robust_list = 99,
    SYS_nanosleep = 101,
    SYS_clock_gettime = 113,
    SYS_clock_nanosleep = 115,
    SYS_sched_getaffinity = 123,
    SYS_tgkill = 131,
    SYS_rt_sigaction = 134,
    SYS_rt_sigprocmask = 135,
    SYS_times = 153,
    SYS_uname = 160,
    SYS_umask = 166,
    SYS_gettimeofday = 169,
    SYS_getpid = 172,
    SYS_getuid = 174,
    SYS_geteuid = 175,
    SYS_getgid = 176,
    SYS_getegid = 177,
    SYS_gettid = 178,
    SYS_sysinfo = 179,
    SYS_clone = 220,
    SYS_brk = 214,
    SYS_munmap = 215,
    SYS_mremap = 216,
    SYS_mmap = 222,
    SYS_mprotect = 226,
    SYS_madvise = 233,
    SYS_prlimit64 = 261,
    SYS_getrandom = 278,
    SYS_rseq = 293,
    SYS_faccessat2 = 439,

    // Internal pseudo-numbers (outside the real syscall space): x86-64
    // legacy syscalls with no asm-generic equivalent. X86ToCanonical() maps
    // onto these and Handle() dispatches them directly.
    SYS_x64_open = 0x10000,
    SYS_x64_access,
    SYS_x64_stat,
    SYS_x64_lstat,
    SYS_x64_readlink,
    SYS_x64_unlink,
    SYS_x64_dup2,
    SYS_x64_arch_prctl,
    SYS_x64_time,
    SYS_x64_alarm,
    SYS_x64_rt_sigreturn,
};

// x86_64 Linux syscall numbers (arch/x86/entry/syscalls/syscall_64.tbl).
// Only the ones we emulate; X86ToCanonical() maps them onto GuestSyscall.
enum GuestSyscallX64 : u64 {
    X64_read = 0,
    X64_write = 1,
    X64_open = 2,
    X64_close = 3,
    X64_stat = 4,
    X64_fstat = 5,
    X64_lstat = 6,
    X64_lseek = 8,
    X64_mmap = 9,
    X64_mprotect = 10,
    X64_munmap = 11,
    X64_brk = 12,
    X64_rt_sigaction = 13,
    X64_rt_sigprocmask = 14,
    X64_rt_sigreturn = 15,
    X64_ioctl = 16,
    X64_pread64 = 17,
    X64_pwrite64 = 18,
    X64_readv = 19,
    X64_writev = 20,
    X64_access = 21,
    X64_mremap = 25,
    X64_madvise = 28,
    X64_dup = 32,
    X64_dup2 = 33,
    X64_nanosleep = 35,
    X64_alarm = 37,
    X64_getpid = 39,
    X64_clone = 56,
    X64_exit = 60,
    X64_uname = 63,
    X64_fcntl = 72,
    X64_fsync = 74,
    X64_fdatasync = 75,
    X64_ftruncate = 77,
    X64_getcwd = 79,
    X64_unlink = 87,
    X64_readlink = 89,
    X64_umask = 95,
    X64_gettimeofday = 96,
    X64_sysinfo = 99,
    X64_times = 100,
    X64_getuid = 102,
    X64_getgid = 104,
    X64_geteuid = 107,
    X64_getegid = 108,
    X64_arch_prctl = 158,
    X64_gettid = 186,
    X64_time = 201,
    X64_futex = 202,
    X64_sched_getaffinity = 204,
    X64_getdents64 = 217,
    X64_set_tid_address = 218,
    X64_clock_gettime = 228,
    X64_clock_nanosleep = 230,
    X64_exit_group = 231,
    X64_tgkill = 234,
    X64_openat = 257,
    X64_newfstatat = 262,
    X64_unlinkat = 263,
    X64_readlinkat = 267,
    X64_faccessat = 269,
    X64_set_robust_list = 273,
    X64_pipe2 = 293,
    X64_prlimit64 = 302,
    X64_getrandom = 318,
    X64_rseq = 334,
    X64_faccessat2 = 439,
};

// errno values (asm-generic).
enum GuestErrno : s64 {
    EPERM_ = 1,
    ENOENT_ = 2,
    EINTR_ = 4,
    EIO_ = 5,
    EBADF_ = 9,
    EAGAIN_ = 11,
    ENOMEM_ = 12,
    EACCES_ = 13,
    EFAULT_ = 14,
    EBUSY_ = 16,
    EEXIST_ = 17,
    ENODEV_ = 19,
    ENOTDIR_ = 20,
    EISDIR_ = 21,
    EINVAL_ = 22,
    EMFILE_ = 24,
    ENOTTY_ = 25,
    EFBIG_ = 27,
    ENOSPC_ = 28,
    ESPIPE_ = 29,
    EROFS_ = 30,
    EPIPE_ = 32,
    ERANGE_ = 34,
    EDEADLK_ = 35,
    ENAMETOOLONG_ = 36,
    ENOSYS_ = 38,
    ELOOP_ = 40,
    ETIMEDOUT_ = 110,
};

// Linux x86_64/asm-generic kernel sigaction ABI. The signal mask passed to
// rt_sigaction is one 64-bit kernel word even though libc's sigset_t is larger.
struct GuestSigAction {
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
};
static_assert(sizeof(GuestSigAction) == 32);

// Process-wide state shared by every emulated guest thread. Guest address
// mappings, the program break, futex queues, and exit_group are Linux process
// state; keeping them here prevents each per-thread SyscallHandler from
// accidentally creating a private view.
class SyscallProcessState {
public:
    SyscallProcessState(GuestMemory* memory, VAddr brk_base);
    ~SyscallProcessState();

    [[nodiscard]] s64 AllocateTid() {
        return next_tid.fetch_add(1, std::memory_order_relaxed);
    }

    s64 Futex(u64 uaddr, u64 op, u64 val, u64 timeout, u64 uaddr2, u64 val3);
    s64 WakeFutex(u64 uaddr, u32 count);
    bool StoreGuestU32(u64 uaddr, u32 value);
    GuestSigAction GetSignalAction(u64 signal);
    void SetSignalAction(u64 signal, const GuestSigAction& action);
    u64 ArmAlarm(u32 seconds);
    void SetAlarmInterrupt(std::function<void()> fn);
    void ShutdownAlarm();
    [[nodiscard]] u64 ConsumePendingSignal(u64 blocked_mask);

    void RequestExitGroup(u8 code);
    [[nodiscard]] bool IsExiting() const {
        return exiting.load(std::memory_order_acquire);
    }
    [[nodiscard]] u8 GetExitCode() const {
        return exit_code.load(std::memory_order_acquire);
    }

    GuestMemory* memory;
    std::mutex memory_mutex;
    VAddr brk_base{};
    VAddr brk_current{};
    VAddr brk_mapped_end{};

private:
    struct FutexWaiter {
        std::condition_variable cv;
        u32 bitset{~u32{0}};
        bool woken{};
    };
    struct FutexQueue {
        std::deque<std::shared_ptr<FutexWaiter>> waiters;
    };

    void WakeAllFutexesLocked();
    s64 WakeFutexMasked(u64 uaddr, u32 count, u32 bitset);
    void AlarmLoop();

    std::mutex futex_mutex;
    std::unordered_map<VAddr, FutexQueue> futex_queues;
    std::mutex signal_mutex;
    std::array<GuestSigAction, 65> signal_actions{};
    std::atomic<u64> pending_signals{};
    std::mutex alarm_mutex;
    std::condition_variable alarm_changed;
    std::thread alarm_thread;
    std::chrono::steady_clock::time_point alarm_deadline{};
    std::function<void()> alarm_interrupt;
    u64 alarm_generation{};
    bool alarm_armed{};
    bool alarm_shutdown{};
    std::atomic<s64> next_tid{1001};
    std::atomic_bool exiting{false};
    std::atomic<u8> exit_code{0};
};

class SyscallHandler {
public:
    // brk_base: initial program break (end of the loaded image).
    // isa: selects the guest syscall numbering (x86_64 numbers are
    // normalized to the asm-generic GuestSyscall enum in Handle()).
    // exe_path: guest ELF path, used for /proc/self/exe emulation.
    explicit SyscallHandler(GuestMemory* memory,
                            VAddr brk_base,
                            GuestISA isa = GuestISA::kArm64,
                            std::string exe_path = {},
                            std::shared_ptr<SyscallProcessState> process = {},
                            s64 tid = 1000,
                            u64 signal_mask = 0)
            : memory(memory), isa(isa), exe_path(std::move(exe_path)),
              process(process ? std::move(process)
                              : std::make_shared<SyscallProcessState>(memory, brk_base)),
              tid(tid), signal_mask(signal_mask) {}

    struct Result {
        s64 ret{};
        bool exited{false};
        bool exit_group{false};
        bool context_restored{false};
        u8 exit_code{0};
    };

    struct SignalDelivery {
        bool delivered{false};
        bool terminated{false};
        u8 exit_code{0};
    };

    struct CloneRequest {
        u64 flags{};
        VAddr child_stack{};
        VAddr parent_tid{};
        VAddr child_tid{};
        u64 tls{};
        u64 signal_mask{};
    };
    using CloneCallback = std::function<s64(const CloneRequest&)>;

    Result Handle(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);

    [[nodiscard]] VAddr GetBrk() const { return process->brk_current; }

    // x86_64 only: hands the handler the thread context so arch_prctl can
    // write fs_base/gs_base through to the frontend-visible fields
    // (translator/x86/cpu.h ThreadContext64::fs_base/gs_base).
    void SetX86Context(void* x86_ctx) { this->x86_ctx = x86_ctx; }

    // SMC wiring (Phase 4): called with (guest_start, guest_end) whenever the
    // guest mprotects (PROT_WRITE), mmaps (MAP_FIXED), munmaps, or mremaps a
    // range that may hold translated code. The callback must invalidate any
    // stale JIT blocks in that range. nullptr/unset = no-op (tests, interp-only).
    void SetSmcInvalidate(std::function<void(VAddr, VAddr)> fn) { smc_invalidate_ = std::move(fn); }
    void SetCloneCallback(CloneCallback fn) { clone_callback_ = std::move(fn); }
    void SetAlarmInterrupt(std::function<void()> fn) {
        process->SetAlarmInterrupt(std::move(fn));
    }
    void ShutdownAlarm() { process->ShutdownAlarm(); }
    SignalDelivery DeliverPendingSignal();

    // TLS segment bases set via arch_prctl (x86_64 only). Mirrors of the
    // context fields, kept for inspection/tests.
    [[nodiscard]] u64 GetFsBase() const { return fs_base; }
    [[nodiscard]] u64 GetGsBase() const { return gs_base; }
    [[nodiscard]] s64 GetTid() const { return tid; }
    [[nodiscard]] VAddr GetClearChildTid() const { return clear_child_tid; }
    void SetClearChildTid(VAddr addr) { clear_child_tid = addr; }
    [[nodiscard]] std::shared_ptr<SyscallProcessState> GetProcessState() const { return process; }

private:
    s64 SysRead(u64 fd, u64 buf, u64 count);
    s64 SysWrite(u64 fd, u64 buf, u64 count);
    s64 SysReadv(u64 fd, u64 iov, u64 iovcnt);
    s64 SysWritev(u64 fd, u64 iov, u64 iovcnt);
    s64 SysBrk(u64 addr);
    s64 SysMmap(u64 addr, u64 length, u64 prot, u64 flags, s64 fd, u64 offset);
    s64 SysMunmap(u64 addr, u64 length);
    s64 SysMprotect(u64 addr, u64 len, u64 prot);
    s64 SysMremap(u64 addr, u64 old_size, u64 new_size, u64 flags, u64 new_addr);
    s64 SysUname(u64 buf);
    s64 SysClockGettime(u64 clock_id, u64 ts);
    s64 SysGettimeofday(u64 tv, u64 tz);
    s64 SysTime(u64 tloc);
    s64 SysNanosleep(u64 req, u64 rem);
    s64 SysClockNanosleep(u64 clock_id, u64 flags, u64 req, u64 rem);
    s64 SysTimes(u64 buf);
    s64 SysSchedGetaffinity(u64 pid, u64 cpusetsize, u64 mask);
    s64 SysRtSigaction(u64 signal, u64 act, u64 oldact, u64 sigset_size);
    s64 SysRtSigprocmask(u64 how, u64 set, u64 oldset, u64 sigset_size);
    s64 SysRtSigreturn();
    s64 SysAlarm(u64 seconds);
    s64 SysUmask(u64 mask);
    s64 SysOpenat(u64 dirfd, u64 path, u64 flags, u64 mode);
    s64 SysClose(u64 fd);
    s64 SysLseek(u64 fd, u64 offset, u64 whence);
    s64 SysPread64(u64 fd, u64 buf, u64 count, u64 offset);
    s64 SysPwrite64(u64 fd, u64 buf, u64 count, u64 offset);
    s64 SysFsync(u64 fd, bool data_only);
    s64 SysFtruncate(u64 fd, u64 length);
    s64 SysFstat(u64 fd, u64 statbuf);
    s64 SysFstatat(u64 dirfd, u64 path, u64 statbuf, u64 flags);
    s64 SysFaccessat(u64 dirfd, u64 path, u64 mode, u64 flags);
    s64 SysReadlinkat(u64 dirfd, u64 path, u64 buf, u64 bufsize);
    s64 SysUnlinkat(u64 dirfd, u64 path, u64 flags);
    s64 SysGetcwd(u64 buf, u64 size);
    s64 SysFcntl(u64 fd, u64 cmd, u64 arg);
    s64 SysDup(u64 fd);
    s64 SysDup2(u64 oldfd, u64 newfd);
    s64 SysIoctl(u64 fd, u64 request, u64 arg);
    s64 SysFutex(u64 uaddr, u64 op, u64 val, u64 timeout, u64 uaddr2, u64 val3);
    s64 SysArchPrctl(u64 code, u64 addr);
    s64 SysClone(u64 flags, u64 child_stack, u64 parent_tid, u64 child_tid, u64 tls);
    s64 SysPrlimit64(u64 pid, u64 resource, u64 new_rlim, u64 old_rlim);
    s64 SysGetrandom(u64 buf, u64 buflen, u64 flags);
    s64 SysSysinfo(u64 buf);
    // Converts a host stat to the guest-ABI struct stat and writes it out.
    s64 WriteGuestStat(u64 guest_buf, const struct stat& host_st);

    GuestMemory* memory;
    GuestISA isa;
    std::string exe_path;
    std::shared_ptr<SyscallProcessState> process;
    // x86_64 ThreadContext64 (translator/x86/cpu.h), set via SetX86Context;
    // arch_prctl writes fs_base/gs_base through it. Opaque here to keep the
    // x86 header out of this one.
    void* x86_ctx{};
    // Thread-ish state for the single emulated thread.
    u64 fs_base{};
    u64 gs_base{};
    u64 robust_list_head{};
    u64 robust_list_len{};
    s64 tid{};
    u64 signal_mask{};
    VAddr clear_child_tid{};
    // SMC callback — see SetSmcInvalidate. nullptr = no SMC tracking active.
    std::function<void(VAddr, VAddr)> smc_invalidate_;
    CloneCallback clone_callback_;
};

}  // namespace swift::linux
