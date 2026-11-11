#pragma once

#include <cstdint>

namespace swift::runtime::platform {

// Private operation numbers consumed by the existing common implementation.
// Guest register/number decoding stays at the architectural ABI boundary.
constexpr std::uint64_t Arm64SyscallOperation(std::uint64_t number) noexcept {
    switch (number) {
        case 23:
            return 32;  // dup
        case 35:
            return 263;  // unlinkat
        case 36:
            return 266;  // symlinkat
        case 61:
            return 217;  // getdents64
        case 24:
            return 292;  // dup3
        case 25:
            return 72;  // fcntl
        case 29:
            return 16;  // ioctl
        case 46:
            return 77;  // ftruncate
        case 48:
            return 269;  // faccessat
        case 56:
            return 257;  // openat
        case 57:
            return 3;  // close
        case 62:
            return 8;  // lseek
        case 63:
            return 0;  // read
        case 64:
            return 1;  // write
        case 66:
            return 20;  // writev
        case 68:
            return 18;  // pwrite64
        case 78:
            return 267;  // readlinkat
        case 79:
            return 262;  // newfstatat
        case 80:
            return 5;  // fstat
        case 93:
            return 60;  // exit
        case 94:
            return 231;  // exit_group
        case 96:
            return 218;  // set_tid_address
        case 98:
            return 202;  // futex
        case 113:
            return 228;  // clock_gettime
        case 123:
            return 204;  // sched_getaffinity
        case 124:
            return 24;  // sched_yield
        case 121:
            return 143;  // sched_getparam
        case 120:
            return 145;  // sched_getscheduler
        case 129:
            return 62;  // kill
        case 130:
            return 200;  // tkill
        case 131:
            return 234;  // tgkill
        case 132:
            return 131;  // sigaltstack
        case 133:
            return 130;  // rt_sigsuspend
        case 134:
            return 13;  // rt_sigaction
        case 135:
            return 14;  // rt_sigprocmask
        case 136:
            return 127;  // rt_sigpending
        case 137:
            return 128;  // rt_sigtimedwait
        case 138:
            return 129;  // rt_sigqueueinfo
        case 139:
            return 15;  // rt_sigreturn: Service handles the control result.
        case 240:
            return 297;  // rt_tgsigqueueinfo
        case 160:
            return 63;  // uname
        case 163:
            return 97;  // getrlimit
        case 167:
            return 157;  // prctl
        case 169:
            return 96;  // gettimeofday
        case 172:
            return 39;  // getpid
        case 174:
            return 102;  // getuid
        case 175:
            return 107;  // geteuid
        case 176:
            return 104;  // getgid
        case 177:
            return 108;  // getegid
        case 178:
            return 186;  // gettid
        case 214:
            return 12;  // brk
        case 215:
            return 11;  // munmap
        case 222:
            return 9;  // mmap
        case 226:
            return 10;  // mprotect
        case 233:
            return 28;  // madvise
        case 261:
            return 302;  // prlimit64
        case 278:
            return 318;  // getrandom
        default:
            return UINT64_MAX;
    }
}

}  // namespace swift::runtime::platform
