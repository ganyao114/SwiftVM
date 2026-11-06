#include "vector_crypto.h"

#include <array>
#include <bit>

namespace swift::runtime::x86::crypto {
namespace {
using Byte = std::uint8_t;
using Word = std::uint32_t;
using Bytes = std::array<Byte, 16>;
using Words = std::array<Word, 4>;

constexpr Byte Multiply(Byte left, Byte right) {
    Byte result{};
    for (unsigned bit = 0; bit < 8; ++bit) {
        if (right & 1) result ^= left;
        left = static_cast<Byte>((left << 1) ^ ((left & 128) ? 0x1b : 0));
        right >>= 1;
    }
    return result;
}
constexpr Byte Substitute(Byte value) {
    Byte power = 1, base = value;
    for (unsigned exponent = 254; exponent; exponent >>= 1) {
        if (exponent & 1) power = Multiply(power, base);
        base = Multiply(base, base);
    }
    return static_cast<Byte>(power ^ std::rotl(power, 1) ^ std::rotl(power, 2) ^
                             std::rotl(power, 3) ^ std::rotl(power, 4) ^ 0x63);
}
constexpr auto Substitution = [] {
    std::array<Byte, 256> table{};
    for (unsigned i = 0; i < table.size(); ++i) table[i] = Substitute(static_cast<Byte>(i));
    return table;
}();
constexpr auto InverseSubstitution = [] {
    std::array<Byte, 256> table{};
    for (unsigned i = 0; i < table.size(); ++i) table[Substitution[i]] = static_cast<Byte>(i);
    return table;
}();
Bytes Unpack(Block128 value) {
    Bytes result{};
    for (unsigned i = 0; i < 8; ++i) {
        result[i] = static_cast<Byte>(value.low >> (i * 8));
        result[i + 8] = static_cast<Byte>(value.high >> (i * 8));
    }
    return result;
}
Block128 Pack(const Bytes& bytes) {
    Block128 result;
    for (unsigned i = 0; i < 8; ++i) {
        result.low |= std::uint64_t{bytes[i]} << (i * 8);
        result.high |= std::uint64_t{bytes[i + 8]} << (i * 8);
    }
    return result;
}
Words UnpackWords(Block128 value) {
    return {static_cast<Word>(value.low), static_cast<Word>(value.low >> 32),
            static_cast<Word>(value.high), static_cast<Word>(value.high >> 32)};
}
Block128 PackWords(const Words& value) {
    return {std::uint64_t{value[0]} | (std::uint64_t{value[1]} << 32),
            std::uint64_t{value[2]} | (std::uint64_t{value[3]} << 32)};
}
void ShiftRows(Bytes& value, bool inverse) {
    const auto input = value;
    for (unsigned column = 0; column < 4; ++column)
        for (unsigned row = 0; row < 4; ++row)
            value[column * 4 + row] = input[((column + (inverse ? 4 - row : row)) & 3) * 4 + row];
}
void MixColumns(Bytes& value, bool inverse) {
    constexpr Byte forward[4]{2, 3, 1, 1}, backward[4]{14, 11, 13, 9};
    const auto* factors = inverse ? backward : forward;
    const Bytes input = value;
    for (unsigned column = 0; column < 4; ++column) {
        for (unsigned row = 0; row < 4; ++row) {
            Byte result{};
            for (unsigned i = 0; i < 4; ++i)
                result ^= Multiply(input[column * 4 + i], factors[(i + 4 - row) & 3]);
            value[column * 4 + row] = result;
        }
    }
}
Word Small0(Word value) { return std::rotr(value, 7) ^ std::rotr(value, 18) ^ (value >> 3); }
Word Small1(Word value) { return std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10); }
Word Big0(Word value) { return std::rotr(value, 2) ^ std::rotr(value, 13) ^ std::rotr(value, 22); }
Word Big1(Word value) { return std::rotr(value, 6) ^ std::rotr(value, 11) ^ std::rotr(value, 25); }
} // namespace

Block128 Execute(Operation operation, Block128 left, Block128 right, Block128 third,
                 std::uint8_t immediate) noexcept {
    switch (operation) {
        case Operation::AesEncrypt: case Operation::AesEncryptLast:
        case Operation::AesDecrypt: case Operation::AesDecryptLast: {
            const bool inverse = operation == Operation::AesDecrypt || operation == Operation::AesDecryptLast;
            const bool last = operation == Operation::AesEncryptLast || operation == Operation::AesDecryptLast;
            auto data = Unpack(left);
            const auto key = Unpack(right);
            for (auto& byte : data) byte = inverse ? InverseSubstitution[byte] : Substitution[byte];
            ShiftRows(data, inverse);
            if (!last) MixColumns(data, inverse);
            for (unsigned i = 0; i < data.size(); ++i) data[i] ^= key[i];
            return Pack(data);
        }
        case Operation::AesInverseMixColumns: {
            auto data = Unpack(left); MixColumns(data, true); return Pack(data);
        }
        case Operation::AesKeygenAssist: {
            const auto source = Unpack(left);
            Bytes output{};
            for (unsigned word = 0; word < 4; ++word) {
                const auto source_word = word < 2 ? 1u : 3u;
                for (unsigned byte = 0; byte < 4; ++byte)
                    output[word * 4 + byte] = Substitution[source[source_word * 4 + ((byte + (word & 1)) & 3)]];
            }
            output[4] ^= immediate; output[12] ^= immediate;
            return Pack(output);
        }
        case Operation::PolynomialMultiply: {
            const auto a = immediate & 1 ? left.high : left.low;
            const auto b = immediate & 16 ? right.high : right.low;
            unsigned __int128 product{};
            for (unsigned bit = 0; bit < 64; ++bit) if ((b >> bit) & 1)
                product ^= static_cast<unsigned __int128>(a) << bit;
            return {static_cast<std::uint64_t>(product), static_cast<std::uint64_t>(product >> 64)};
        }
        case Operation::Sha256Message1: {
            const auto a = UnpackWords(left), b = UnpackWords(right);
            Words output{};
            for (unsigned i = 0; i < 4; ++i) output[i] = a[i] + Small0(i == 3 ? b[0] : a[i + 1]);
            return PackWords(output);
        }
        case Operation::Sha256Message2: {
            auto output = UnpackWords(left); const auto b = UnpackWords(right);
            output[0] += Small1(b[2]); output[1] += Small1(b[3]);
            output[2] += Small1(output[0]); output[3] += Small1(output[1]);
            return PackWords(output);
        }
        case Operation::Sha256Rounds2: {
            const auto a = UnpackWords(left), b = UnpackWords(right), key = UnpackWords(third);
            std::array<Word, 8> s{b[3], b[2], a[3], a[2], b[1], b[0], a[1], a[0]};
            for (unsigned round = 0; round < 2; ++round) {
                const Word t1 = s[7] + Big1(s[4]) + ((s[4] & s[5]) ^ (~s[4] & s[6])) + key[round];
                const Word t2 = Big0(s[0]) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
                s = {t1 + t2, s[0], s[1], s[2], s[3] + t1, s[4], s[5], s[6]};
            }
            return PackWords({s[5], s[4], s[1], s[0]});
        }
    }
    return {}; // All callers select one of the closed numerical operations.
}
} // namespace swift::runtime::x86::crypto
