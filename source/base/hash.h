#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace swift {
using Digest = std::array<std::uint8_t, 32>;

class Sha256 {
public:
    void Update(std::span<const std::uint8_t>);
    Digest Finish();

private:
    void Transform(const std::uint8_t*);
    std::array<std::uint32_t, 8> state{0x6a09e667,
                                       0xbb67ae85,
                                       0x3c6ef372,
                                       0xa54ff53a,
                                       0x510e527f,
                                       0x9b05688c,
                                       0x1f83d9ab,
                                       0x5be0cd19};
    std::array<std::uint8_t, 64> pending{};
    std::uint64_t length{};
    unsigned used{};
};

Digest HashBytes(std::span<const std::uint8_t>);
std::string HexDigest(const Digest&);
}  // namespace swift
