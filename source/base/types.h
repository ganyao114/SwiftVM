#pragma once

#include <cstdint>
#include <string>

namespace swift {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;

#ifdef __APPLE__
typedef uint64_t u64;     ///<  64-bit unsigned integer.
#else
typedef uint64_t u64;     ///<  64-bit unsigned integer.
#endif

using u128 = std::array<u64, 2>;

using u256 = std::array<u64, 4>;

using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

#if __LP64__
using VAddr = u64;
#else
using VAddr = u32;
#endif

constexpr inline u64 operator ""_KB(unsigned long long n) {
    return static_cast<u64>(n) * UINT64_C(1024);
}

constexpr inline u64 operator ""_MB(unsigned long long n) {
    return operator ""_KB(n) * UINT64_C(1024);
}

constexpr inline u64 operator ""_GB(unsigned long long n) {
    return operator ""_MB(n) * UINT64_C(1024);
}

#define UNREACHABLE abort

}