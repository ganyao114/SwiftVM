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

#include "base/types.h"
#include "guest_memory.h"
#include "loader.h"

namespace swift::linux {

// Canonical syscall numbering (asm-generic unistd == AArch64). x86_64
// numbers are translated to these in Handle().
enum GuestSyscall : u64 {
    SYS_read = 63,
    SYS_write = 64,
    SYS_readv = 65,
    SYS_writev = 66,
    SYS_exit = 93,
    SYS_exit_group = 94,
    SYS_clock_gettime = 113,
    SYS_uname = 160,
    SYS_getpid = 172,
    SYS_gettid = 178,
    SYS_getuid = 174,
    SYS_geteuid = 175,
    SYS_getgid = 176,
    SYS_getegid = 177,
    SYS_brk = 214,
    SYS_munmap = 215,
    SYS_mmap = 222,
    SYS_mprotect = 226,
};

// x86_64 Linux syscall numbers (arch/x86/entry/syscalls/syscall_64.tbl).
// Only the ones we emulate; X86ToCanonical() maps them onto GuestSyscall.
enum GuestSyscallX64 : u64 {
    X64_read = 0,
    X64_write = 1,
    X64_mmap = 9,
    X64_mprotect = 10,
    X64_munmap = 11,
    X64_brk = 12,
    X64_readv = 19,
    X64_writev = 20,
    X64_getpid = 39,
    X64_exit = 60,
    X64_uname = 63,
    X64_getuid = 102,
    X64_getgid = 104,
    X64_geteuid = 107,
    X64_getegid = 108,
    X64_gettid = 186,
    X64_clock_gettime = 228,
    X64_exit_group = 231,
};

// errno values (asm-generic).
enum GuestErrno : s64 {
    EPERM_ = 1,
    EBADF_ = 9,
    ENOMEM_ = 12,
    EFAULT_ = 14,
    EINVAL_ = 22,
    ENOSYS_ = 38,
};

class SyscallHandler {
public:
    // brk_base: initial program break (end of the loaded image).
    // isa: selects the guest syscall numbering (x86_64 numbers are
    // normalized to the asm-generic GuestSyscall enum in Handle()).
    explicit SyscallHandler(GuestMemory* memory, VAddr brk_base, GuestISA isa = GuestISA::kArm64)
            : memory(memory), isa(isa), brk_base(brk_base), brk_current(brk_base),
              brk_mapped_end(GuestMemory::RoundHostPage(brk_base)) {}

    struct Result {
        s64 ret{};
        bool exited{false};
        u8 exit_code{0};
    };

    Result Handle(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);

    [[nodiscard]] VAddr GetBrk() const { return brk_current; }

private:
    s64 SysRead(u64 fd, u64 buf, u64 count);
    s64 SysWrite(u64 fd, u64 buf, u64 count);
    s64 SysWritev(u64 fd, u64 iov, u64 iovcnt);
    s64 SysBrk(u64 addr);
    s64 SysMmap(u64 addr, u64 length, u64 prot, u64 flags, s64 fd, u64 offset);
    s64 SysMunmap(u64 addr, u64 length);
    s64 SysMprotect(u64 addr, u64 len, u64 prot);
    s64 SysUname(u64 buf);
    s64 SysClockGettime(u64 clock_id, u64 ts);

    GuestMemory* memory;
    GuestISA isa;
    VAddr brk_base{};
    VAddr brk_current{};
    VAddr brk_mapped_end{};
    // Bump allocator for guest anonymous mmaps without an address hint.
    VAddr mmap_next{0x60000000};
};

}  // namespace swift::linux
