#include "hash.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace swift {
void Sha256::Transform(const std::uint8_t* block) {
    static constexpr std::array<std::uint32_t, 64> constants{
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};
    std::array<std::uint32_t, 64> words;
    for (unsigned i = 0; i < 16; ++i)
        words[i] = (std::uint32_t{block[i * 4]} << 24) | (std::uint32_t{block[i * 4 + 1]} << 16) |
                   (std::uint32_t{block[i * 4 + 2]} << 8) | block[i * 4 + 3];
    for (unsigned i = 16; i < words.size(); ++i) {
        const auto a = words[i - 15], b = words[i - 2];
        const auto first = std::rotr(a, 7) ^ std::rotr(a, 18) ^ (a >> 3);
        const auto second = std::rotr(b, 17) ^ std::rotr(b, 19) ^ (b >> 10);
        words[i] = words[i - 16] + first + words[i - 7] + second;
    }
    auto [a, b, c, d, e, f, g, h] = state;
    for (unsigned i = 0; i < words.size(); ++i) {
        const auto sum = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
                         ((e & f) ^ (~e & g)) + constants[i] + words[i];
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto next = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + majority;
        h = g;
        g = f;
        f = e;
        e = d + sum;
        d = c;
        c = b;
        b = a;
        a = sum + next;
    }
    const std::array<std::uint32_t, 8> combined{a, b, c, d, e, f, g, h};
    for (unsigned i = 0; i < state.size(); ++i) state[i] += combined[i];
}

void Sha256::Update(std::span<const std::uint8_t> bytes) {
    length += bytes.size();
    while (!bytes.empty()) {
        const auto count = std::min(bytes.size(), pending.size() - used);
        std::memcpy(pending.data() + used, bytes.data(), count);
        used += static_cast<unsigned>(count);
        bytes = bytes.subspan(count);
        if (used == pending.size()) {
            Transform(pending.data());
            used = 0;
        }
    }
}

Digest Sha256::Finish() {
    const auto bits = length * 8;
    pending[used++] = 0x80;
    if (used > 56) {
        std::fill(pending.begin() + used, pending.end(), 0);
        Transform(pending.data());
        used = 0;
    }
    std::fill(pending.begin() + used, pending.begin() + 56, 0);
    for (unsigned i = 0; i < 8; ++i) pending[63 - i] = static_cast<std::uint8_t>(bits >> (i * 8));
    Transform(pending.data());
    Digest digest;
    for (unsigned i = 0; i < state.size(); ++i)
        for (unsigned byte = 0; byte < 4; ++byte)
            digest[i * 4 + byte] = static_cast<std::uint8_t>(state[i] >> (24 - byte * 8));
    return digest;
}

Digest HashBytes(std::span<const std::uint8_t> bytes) {
    Sha256 hash;
    hash.Update(bytes);
    return hash.Finish();
}

std::string HexDigest(const Digest& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '\0');
    for (unsigned i = 0; i < digest.size(); ++i) {
        result[i * 2] = digits[digest[i] >> 4];
        result[i * 2 + 1] = digits[digest[i] & 15];
    }
    return result;
}
}  // namespace swift
