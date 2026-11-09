#include "abi_arm32.h"

#include <cstring>
#include <sys/stat.h>
#include "runtime/execution/memory.h"

namespace swift::runtime::platform::abi32 {
GuestSignalAction Decode(const Action& a) noexcept {
    return {a.handler, a.flags, a.restorer, a.mask[0] | (std::uint64_t(a.mask[1]) << 32)};
}

Action Encode(const GuestSignalAction& a) noexcept {
    return {std::uint32_t(a.handler),
            std::uint32_t(a.flags),
            std::uint32_t(a.restorer),
            {std::uint32_t(a.mask), std::uint32_t(a.mask >> 32)}};
}

GuestSignalStack Decode(const Stack& s) noexcept { return {s.address, s.flags, 0, s.size}; }

Stack Encode(const GuestSignalStack& s) noexcept {
    return {std::uint32_t(s.address), s.flags, std::uint32_t(s.size)};
}

GuestSignalInfo Decode(const SignalInfo& s) noexcept {
    GuestSignalInfo out{};
    out.number = s.number;
    out.error = s.error;
    out.code = s.code;
    std::memcpy(out.payload.data(), s.payload.data(), out.payload.size());
    return out;
}

SignalInfo Encode(const GuestSignalInfo& s) noexcept {
    SignalInfo out{};
    out.number = s.number;
    out.error = s.error;
    out.code = s.code;
    std::memcpy(out.payload.data(), s.payload.data(), s.payload.size());
    // The owner records addresses and sigval in 64-bit fields. Their low word
    // occupies the corresponding AArch32 union member; the high word is padding.
    if (s.code > 0 &&
        (s.number == 4 || s.number == 5 || s.number == 7 || s.number == 8 || s.number == 11))
        std::memset(out.payload.data() + 4, 0, 4);
    return out;
}

Stat64 Encode(const struct stat& s) noexcept {
    Stat64 out;
    out.device = s.st_dev;
    out.old_inode = s.st_ino;
    out.mode = s.st_mode;
    out.links = s.st_nlink;
    out.uid = s.st_uid;
    out.gid = s.st_gid;
    out.rdevice = s.st_rdev;
    out.size = s.st_size;
    out.block_size = s.st_blksize;
    out.blocks = s.st_blocks;
    out.atime = s.st_atim.tv_sec;
    out.atime_nsec = s.st_atim.tv_nsec;
    out.mtime = s.st_mtim.tv_sec;
    out.mtime_nsec = s.st_mtim.tv_nsec;
    out.ctime = s.st_ctim.tv_sec;
    out.ctime_nsec = s.st_ctim.tv_nsec;
    out.inode = s.st_ino;
    return out;
}

bool ReadTimespec(const execution::GuestMemory& memory,
                  std::uint64_t address,
                  timespec& value,
                  bool time64) {
    if (time64) {
        std::int64_t words[2];
        if (!memory.CopyIn(words, address, sizeof(words))) return false;
        value = {words[0], words[1]};
    } else {
        Timespec words;
        if (!memory.CopyIn(&words, address, sizeof(words))) return false;
        value = {words.seconds, words.nanos};
    }
    return true;
}

std::uint32_t HardwareCapabilities(const arm32::Profile& p) noexcept {
    // Linux ARM HWCAP: SWP, halfword access, Thumb, fast multiply, DSP, TLS.
    std::uint32_t value = 1u | 2u | 4u | 16u | 128u | 32768u;
    if (p.vfp) value |= (1u << 6) | (1u << 13) | (1u << 19);  // VFPv3 D32
    if (p.vfp && p.neon) value |= 1u << 12;
    if (p.vfp && p.vfpv4) value |= 1u << 16;
    if (p.divide) value |= (1u << 17) | (1u << 18);
    return value;
}

std::uint64_t SyscallOperation(std::uint64_t n) noexcept {
    switch (n) {
        case 1:
            return 60;
        case 3:
            return 0;
        case 4:
            return 1;
        case 6:
            return 3;
        case 19:
            return 8;
        case 20:
            return 39;
        case 24:
        case 199:
            return 102;
        case 33:
            return 21;
        case 85:
            return 89;
        case 37:
            return 62;
        case 45:
            return 12;
        case 47:
        case 200:
            return 104;
        case 49:
        case 201:
            return 107;
        case 50:
        case 202:
            return 108;
        case 54:
            return 16;
        case 55:
        case 221:
            return 72;
        case 78:
            return 96;
        case 91:
            return 11;
        case 93:
        case 194:
            return 77;
        case 122:
            return 63;
        case 125:
            return 10;
        case 146:
            return 20;
        case 155:
            return 143;
        case 157:
            return 145;
        case 158:
            return 24;
        case 172:
            return 157;
        case 173:
            return 15;
        case 174:
            return 13;
        case 175:
            return 14;
        case 176:
            return 127;
        case 177:
        case 421:
            return 128;
        case 178:
            return 129;
        case 179:
            return 130;
        case 181:
            return 18;
        case 186:
            return 131;
        case 191:
            return 97;
        case 192:
            return 9;
        case 197:
            return 5;
        case 220:
            return 28;
        case 224:
            return 186;
        case 238:
            return 200;
        case 240:
        case 422:
            return 202;
        case 242:
            return 204;
        case 248:
            return 231;
        case 256:
            return 218;
        case 263:
        case 403:
            return 228;
        case 268:
            return 234;
        case 322:
            return 257;
        case 327:
            return 262;
        case 332:
            return 267;
        case 334:
            return 269;
        case 363:
            return 297;
        case 369:
            return 302;
        case 384:
            return 318;
        default:
            return UINT64_MAX;
    }
}
}  // namespace swift::runtime::platform::abi32
