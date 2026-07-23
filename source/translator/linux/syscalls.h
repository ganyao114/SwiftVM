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

#include <functional>
#include <string>
#include <sys/stat.h>
#include "base/types.h"
#include "guest_memory.h"
#include "loader.h"

namespace swift::linux {

// Canonical syscall numbering (asm-generic unistd == AArch64). x86_64
// numbers are translated to these in Handle().
enum GuestSyscall : u64 {
    SYS_getcwd = 17,
    SYS_dup = 23,
    SYS_fcntl = 25,
    SYS_ioctl = 29,
    SYS_unlinkat = 35,
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
    SYS_readlinkat = 78,
    SYS_newfstatat = 79,
    SYS_fstat = 80,
    SYS_exit = 93,
    SYS_exit_group = 94,
    SYS_set_tid_address = 96,
    SYS_futex = 98,
    SYS_set_robust_list = 99,
    SYS_nanosleep = 101,
    SYS_clock_gettime = 113,
    SYS_tgkill = 131,
    SYS_times = 153,
    SYS_uname = 160,
    SYS_gettimeofday = 169,
    SYS_getpid = 172,
    SYS_getuid = 174,
    SYS_geteuid = 175,
    SYS_getgid = 176,
    SYS_getegid = 177,
    SYS_gettid = 178,
    SYS_sysinfo = 179,
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
    X64_ioctl = 16,
    X64_pread64 = 17,
    X64_readv = 19,
    X64_writev = 20,
    X64_access = 21,
    X64_mremap = 25,
    X64_madvise = 28,
    X64_dup = 32,
    X64_dup2 = 33,
    X64_nanosleep = 35,
    X64_getpid = 39,
    X64_exit = 60,
    X64_uname = 63,
    X64_fcntl = 72,
    X64_getcwd = 79,
    X64_unlink = 87,
    X64_readlink = 89,
    X64_gettimeofday = 96,
    X64_sysinfo = 99,
    X64_times = 100,
    X64_getuid = 102,
    X64_getgid = 104,
    X64_geteuid = 107,
    X64_getegid = 108,
    X64_arch_prctl = 158,
    X64_gettid = 186,
    X64_futex = 202,
    X64_getdents64 = 217,
    X64_set_tid_address = 218,
    X64_clock_gettime = 228,
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
                            std::string exe_path = {})
            : memory(memory), isa(isa), exe_path(std::move(exe_path)), brk_base(brk_base),
              brk_current(brk_base), brk_mapped_end(GuestMemory::RoundHostPage(brk_base)) {}

    struct Result {
        s64 ret{};
        bool exited{false};
        u8 exit_code{0};
    };

    Result Handle(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);

    [[nodiscard]] VAddr GetBrk() const { return brk_current; }

    // x86_64 only: hands the handler the thread context so arch_prctl can
    // write fs_base/gs_base through to the frontend-visible fields
    // (translator/x86/cpu.h ThreadContext64::fs_base/gs_base).
    void SetX86Context(void* x86_ctx) { this->x86_ctx = x86_ctx; }

    // SMC wiring (Phase 4): called with (guest_start, guest_end) whenever the
    // guest mprotects (PROT_WRITE), mmaps (MAP_FIXED), munmaps, or mremaps a
    // range that may hold translated code. The callback must invalidate any
    // stale JIT blocks in that range. nullptr/unset = no-op (tests, interp-only).
    void SetSmcInvalidate(std::function<void(VAddr, VAddr)> fn) { smc_invalidate_ = std::move(fn); }

    // TLS segment bases set via arch_prctl (x86_64 only). Mirrors of the
    // context fields, kept for inspection/tests.
    [[nodiscard]] u64 GetFsBase() const { return fs_base; }
    [[nodiscard]] u64 GetGsBase() const { return gs_base; }

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
    s64 SysNanosleep(u64 req, u64 rem);
    s64 SysTimes(u64 buf);
    s64 SysOpenat(u64 dirfd, u64 path, u64 flags, u64 mode);
    s64 SysClose(u64 fd);
    s64 SysLseek(u64 fd, u64 offset, u64 whence);
    s64 SysPread64(u64 fd, u64 buf, u64 count, u64 offset);
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
    s64 SysPrlimit64(u64 pid, u64 resource, u64 new_rlim, u64 old_rlim);
    s64 SysGetrandom(u64 buf, u64 buflen, u64 flags);
    s64 SysSysinfo(u64 buf);
    // Converts a host stat to the guest-ABI struct stat and writes it out.
    s64 WriteGuestStat(u64 guest_buf, const struct stat& host_st);

    GuestMemory* memory;
    GuestISA isa;
    std::string exe_path;
    VAddr brk_base{};
    VAddr brk_current{};
    VAddr brk_mapped_end{};
    // x86_64 ThreadContext64 (translator/x86/cpu.h), set via SetX86Context;
    // arch_prctl writes fs_base/gs_base through it. Opaque here to keep the
    // x86 header out of this one.
    void* x86_ctx{};
    // Thread-ish state for the single emulated thread.
    u64 fs_base{};
    u64 gs_base{};
    u64 robust_list_head{};
    u64 robust_list_len{};
    s64 tid{1000};
    // SMC callback — see SetSmcInvalidate. nullptr = no SMC tracking active.
    std::function<void(VAddr, VAddr)> smc_invalidate_;
};

}  // namespace swift::linux
