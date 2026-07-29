#include <array>

#include <catch2/catch_test_macros.hpp>

#include "runtime/frontend/x86/distorm_fast.h"

namespace {

void CheckCandidate(std::array<std::uint8_t, 16> bytes, unsigned long long& candidates) {
    _DInst fast{};
    if (!swift::x86::DecodeDistormFast(bytes.data(), bytes.size(), true, fast)) return;
    ++candidates;
    const auto distorm = DisDecode(bytes.data(), bytes.size(), 1);
    CAPTURE(bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);
    REQUIRE(swift::x86::DistormFastEquivalent(fast, distorm, bytes.data()));
}

}  // namespace

TEST_CASE("distorm fast decoder is field-equivalent over its ModRM subset", "[distorm-fast]") {
    constexpr std::array<std::uint8_t, 25> opcodes{
            0x89, 0x8b, 0xc7, 0x01, 0x03, 0x09, 0x0b, 0x21, 0x23, 0x29, 0x2b, 0x31, 0x33,
            0x39, 0x3b, 0x85, 0x81, 0x83, 0xf7, 0x8d, 0xb6, 0xb7, 0xd1, 0xd3, 0xc1};
    unsigned long long candidates = 0;
    for (unsigned rex_value = 0; rex_value < 17; ++rex_value) {
        const bool have_rex = rex_value != 0;
        const std::uint8_t rex = have_rex ? static_cast<std::uint8_t>(0x40 | (rex_value - 1)) : 0;
        for (const auto opcode : opcodes) {
            for (unsigned modrm = 0; modrm < 256; ++modrm) {
                std::array<std::uint8_t, 16> bytes{};
                unsigned offset = 0;
                if (have_rex) bytes[offset++] = rex;
                if (opcode == 0xb6 || opcode == 0xb7) bytes[offset++] = 0x0f;
                bytes[offset++] = opcode;
                bytes[offset++] = static_cast<std::uint8_t>(modrm);
                if ((modrm & 7) == 4 && (modrm >> 6) != 3) bytes[offset++] = 0xac;
                for (unsigned value = 0; offset < bytes.size(); ++value) {
                    bytes[offset++] = static_cast<std::uint8_t>(0x80 + value);
                }
                CheckCandidate(bytes, candidates);
            }
        }
    }
    REQUIRE(candidates > 40000);
}

TEST_CASE("distorm fast decoder matches direct control and stack forms", "[distorm-fast]") {
    unsigned long long candidates = 0;
    for (unsigned rex_value = 0; rex_value < 17; ++rex_value) {
        for (unsigned opcode = 0; opcode < 256; ++opcode) {
            std::array<std::uint8_t, 16> bytes{};
            unsigned offset = 0;
            if (rex_value != 0) {
                bytes[offset++] = static_cast<std::uint8_t>(0x40 | (rex_value - 1));
            }
            bytes[offset++] = static_cast<std::uint8_t>(opcode);
            for (unsigned value = 0; offset < bytes.size(); ++value) {
                bytes[offset++] = static_cast<std::uint8_t>(0x80 + value);
            }
            CheckCandidate(bytes, candidates);
        }
    }
    REQUIRE(candidates > 100);
}

TEST_CASE("distorm fast decoder rejects legacy prefixes and high-8 MOVZX", "[distorm-fast]") {
    for (const auto bytes : {
                 std::array<std::uint8_t, 4>{0x66, 0x89, 0xc0, 0},
                 std::array<std::uint8_t, 4>{0x67, 0x89, 0xc0, 0},
                 std::array<std::uint8_t, 4>{0x64, 0x89, 0xc0, 0},
                 std::array<std::uint8_t, 4>{0xf0, 0x01, 0x00, 0},
                 std::array<std::uint8_t, 4>{0xf3, 0x89, 0xc0, 0},
                 std::array<std::uint8_t, 4>{0x0f, 0xb6, 0xc4, 0},
         }) {
        _DInst fast{};
        REQUIRE_FALSE(swift::x86::DecodeDistormFast(bytes.data(), bytes.size(), true, fast));
    }
}
