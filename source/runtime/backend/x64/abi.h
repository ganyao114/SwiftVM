#pragma once
#include <cstdint>

namespace swift::runtime::backend::x64::abi {
enum Feature : std::uint64_t {
    Ssse3 = 1,
    Sse41 = 2,
    Sse42 = 4,
    Fma = 8,
    Aes = 16,
    Pclmul = 32,
    Sha = 64,
    Cx16 = 128,
    Lahf = 256,
    Avx = 512
};

inline constexpr unsigned StateRegister = 12, FlagsRegister = 3;
}  // namespace swift::runtime::backend::x64::abi
