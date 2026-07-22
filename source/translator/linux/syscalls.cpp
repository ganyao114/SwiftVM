//
// Linux syscall emulation — see syscalls.h for the calling convention.
//

#include <cerrno>
#include <cstring>
#include <ctime>
#include <sys/uio.h>
#include <unistd.h>
#include "base/logging.h"
#include "syscalls.h"

namespace swift::linux {

// Guest mmap flags (asm-generic mman).
static constexpr u64 GUEST_MAP_SHARED = 0x01;
static constexpr u64 GUEST_MAP_PRIVATE = 0x02;
static constexpr u64 GUEST_MAP_FIXED = 0x10;
static constexpr u64 GUEST_MAP_ANONYMOUS = 0x20;

// Guest iovec: same layout as the host one on LP64 (u64 base, u64 len), and
// guest memory is identity-mapped, so host readv/writev can consume it.
struct GuestIovec {
    u64 base;
    u64 len;
};
static_assert(sizeof(GuestIovec) == sizeof(struct iovec));

// Maps an x86_64 syscall number onto the canonical (asm-generic) numbering.
// Returns the raw number unchanged when there is no mapping (it will fall
// into the -ENOSYS default below).
static u64 X86ToCanonical(u64 nr) {
    switch (nr) {
        case X64_read: return SYS_read;
        case X64_write: return SYS_write;
        case X64_readv: return SYS_readv;
        case X64_writev: return SYS_writev;
        case X64_exit: return SYS_exit;
        case X64_exit_group: return SYS_exit_group;
        case X64_brk: return SYS_brk;
        case X64_mmap: return SYS_mmap;
        case X64_munmap: return SYS_munmap;
        case X64_mprotect: return SYS_mprotect;
        case X64_uname: return SYS_uname;
        case X64_clock_gettime: return SYS_clock_gettime;
        case X64_getpid: return SYS_getpid;
        case X64_gettid: return SYS_gettid;
        case X64_getuid: return SYS_getuid;
        case X64_geteuid: return SYS_geteuid;
        case X64_getgid: return SYS_getgid;
        case X64_getegid: return SYS_getegid;
        default: return nr;
    }
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
        case SYS_writev:
            result.ret = SysWritev(a0, a1, a2);
            break;
        case SYS_exit:
        case SYS_exit_group:
            result.ret = 0;
            result.exited = true;
            result.exit_code = static_cast<u8>(a0);
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
        case SYS_uname:
            result.ret = SysUname(a0);
            break;
        case SYS_clock_gettime:
            result.ret = SysClockGettime(a0, a1);
            break;
        case SYS_getpid:
            result.ret = 1000;
            break;
        case SYS_gettid:
            result.ret = 1000;
            break;
        case SYS_getuid:
        case SYS_geteuid:
        case SYS_getgid:
        case SYS_getegid:
            result.ret = 1000;
            break;
        default:
            LOG_WARNING("Unimplemented guest syscall nr {} (args {:#x} {:#x} {:#x}), returning -ENOSYS",
                        raw_nr,
                        a0,
                        a1,
                        a2);
            result.ret = -ENOSYS_;
            break;
    }
    return result;
}

s64 SyscallHandler::SysRead(u64 fd, u64 buf, u64 count) {
    auto ret = ::read(static_cast<int>(fd), reinterpret_cast<void*>(buf), count);
    return ret < 0 ? -errno : ret;
}

s64 SyscallHandler::SysWrite(u64 fd, u64 buf, u64 count) {
    auto ret = ::write(static_cast<int>(fd), reinterpret_cast<const void*>(buf), count);
    return ret < 0 ? -errno : ret;
}

s64 SyscallHandler::SysWritev(u64 fd, u64 iov, u64 iovcnt) {
    if (iovcnt > 1024) return -EINVAL_;
    auto ret = ::writev(static_cast<int>(fd),
                        reinterpret_cast<const struct iovec*>(iov),
                        static_cast<int>(iovcnt));
    return ret < 0 ? -errno : ret;
}

s64 SyscallHandler::SysBrk(u64 addr) {
    if (addr == 0 || addr < brk_base) {
        return static_cast<s64>(brk_current);
    }
    const VAddr new_mapped_end = GuestMemory::RoundHostPage(addr);
    if (new_mapped_end > brk_mapped_end) {
        if (!memory->MapFixed(brk_mapped_end, new_mapped_end - brk_mapped_end)) {
            return -ENOMEM_;
        }
        brk_mapped_end = new_mapped_end;
    }
    brk_current = addr;
    return static_cast<s64>(brk_current);
}

s64 SyscallHandler::SysMmap(u64 addr, u64 length, u64 prot, u64 flags, s64 fd, u64 offset) {
    if (length == 0) return -EINVAL_;
    if (!(flags & GUEST_MAP_ANONYMOUS) || fd != -1) {
        LOG_WARNING("guest mmap: file-backed mappings not supported (fd {}, offset {:#x})", fd, offset);
        return -ENOSYS_;
    }
    const u64 map_length = GuestMemory::RoundHostPage(length);

    VAddr guest_addr = 0;
    if (flags & GUEST_MAP_FIXED) {
        if (addr % GuestMemory::kHostPageSize != 0) return -EINVAL_;
        if (!memory->MapFixed(addr, map_length)) return -ENOMEM_;
        guest_addr = addr;
    } else if (addr != 0 && addr % GuestMemory::kHostPageSize == 0) {
        // Address hint: honor it if the range is free, else fall back.
        if (memory->MapFixed(addr, map_length)) {
            guest_addr = addr;
        }
    }
    if (!guest_addr) {
        guest_addr = memory->MapFixed(mmap_next, map_length) ? mmap_next : 0;
        if (guest_addr) {
            mmap_next = GuestMemory::RoundHostPage(guest_addr + map_length);
        } else {
            guest_addr = memory->MapAnywhere(map_length);
        }
    }
    if (!guest_addr) return -ENOMEM_;
    return static_cast<s64>(guest_addr);
}

s64 SyscallHandler::SysMunmap(u64 addr, u64 length) {
    if (addr % GuestMemory::kGuestPageSize != 0) return -EINVAL_;
    if (length == 0) return -EINVAL_;
    memory->Unmap(addr, length);
    return 0;
}

s64 SyscallHandler::SysMprotect(u64 addr, u64 len, u64 prot) {
    // Guest protections are advisory for us: the host never executes guest
    // code and the JIT reads/writes guest memory as data. Pretend success.
    return 0;
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
    memory->WriteBytes(buf, {uts, sizeof(uts)});
    return 0;
}

s64 SyscallHandler::SysClockGettime(u64 clock_id, u64 ts) {
    struct timespec host_ts {};
    // Guest clock ids match the host for REALTIME(0) / MONOTONIC(1).
    if (::clock_gettime(static_cast<clockid_t>(clock_id), &host_ts) != 0) {
        return -EINVAL_;
    }
    memory->Write<s64>(ts, host_ts.tv_sec);
    memory->Write<s64>(ts + 8, host_ts.tv_nsec);
    return 0;
}

}  // namespace swift::linux
