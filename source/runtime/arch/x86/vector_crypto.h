#pragma once

#include <cstdint>

namespace swift::runtime::x86::crypto {

// Little-endian x86 vector bits. These numerical operations own no CPU state,
// host feature selection, memory access or floating-point environment.
struct Block128 { std::uint64_t low{}, high{}; };
static_assert(sizeof(Block128) == 16);
enum class Operation : std::uint8_t {
    AesEncrypt, AesEncryptLast, AesDecrypt, AesDecryptLast,
    AesInverseMixColumns, AesKeygenAssist, PolynomialMultiply,
    Sha256Message1, Sha256Message2, Sha256Rounds2,
};
Block128 Execute(Operation operation, Block128 left, Block128 right = {},
                 Block128 third = {}, std::uint8_t immediate = 0) noexcept;

} // namespace swift::runtime::x86::crypto
