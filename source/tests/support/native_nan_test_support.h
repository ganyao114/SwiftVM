#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <catch2/catch_test_macros.hpp>

namespace swift::tests {

// Native arithmetic chooses quiet-NaN payload/sign only in the explicitly
// written result prefix. Every other byte is compared to the original oracle.
template<std::size_t N>
inline bool MatchesNativeNaNResult(const std::array<std::uint8_t, N>& got,
                                   const std::array<std::uint8_t, N>& want,
                                   std::uint32_t lane_bits, std::uint32_t written_bytes) {
    if ((lane_bits != 32 && lane_bits != 64) || written_bytes > got.size() ||
        written_bytes % (lane_bits / 8) != 0) return false;
    const std::uint32_t step = lane_bits / 8;
    const std::uint64_t exponent = lane_bits == 32 ? 0x7F800000ull : 0x7FF0000000000000ull;
    const std::uint64_t fraction = lane_bits == 32 ? 0x007FFFFFull : 0x000FFFFFFFFFFFFFull;
    const std::uint64_t quiet = lane_bits == 32 ? 0x00400000ull : 0x0008000000000000ull;
    auto normalized = got;
    for (std::uint32_t off = 0; off < written_bytes; off += step) {
        std::uint64_t actual{}, expected{};
        std::memcpy(&actual, got.data() + off, step);
        std::memcpy(&expected, want.data() + off, step);
        const bool actual_nan = (actual & exponent) == exponent && (actual & fraction);
        const bool expected_nan = (expected & exponent) == exponent && (expected & fraction);
        if (!actual_nan && !expected_nan) continue;
        if (!actual_nan || !expected_nan || !(actual & quiet) || !(expected & quiet))
            return false;
        std::copy_n(want.begin() + off, step, normalized.begin() + off);
    }
    return normalized == want;
}

inline void CheckNativeNaNMatcher() {
    for (const std::uint32_t bits : {32u, 64u}) {
        CAPTURE(bits);
        const std::uint32_t step = bits / 8;
        const std::uint64_t exponent = bits == 32 ? 0x7F800000ull : 0x7FF0000000000000ull;
        const std::uint64_t quiet = bits == 32 ? 0x00400000ull : 0x0008000000000000ull;
        const std::uint64_t sign = bits == 32 ? 0x80000000ull : 0x8000000000000000ull;
        const auto put = [step](std::array<std::uint8_t, 32>& value, std::uint32_t off, std::uint64_t word) {
            std::memcpy(value.data() + off, &word, step);
        };
        std::array<std::uint8_t, 32> want{}, got{};
        put(want, 0, exponent | quiet | 1);
        put(got, 0, sign | exponent | quiet | 2);
        CHECK(MatchesNativeNaNResult(got, want, bits, step));
        CHECK_FALSE(MatchesNativeNaNResult(got, want, bits, 0));
        auto signalling = got;
        put(signalling, 0, exponent | 1);
        CHECK_FALSE(MatchesNativeNaNResult(signalling, want, bits, step));
        CHECK_FALSE(MatchesNativeNaNResult(signalling, signalling, bits, step));
        std::array<std::uint8_t, 32> finite{}, changed{};
        changed[0] = 1;
        CHECK_FALSE(MatchesNativeNaNResult(changed, finite, bits, step));
        put(changed, 0, sign);
        CHECK_FALSE(MatchesNativeNaNResult(changed, finite, bits, step));
        put(finite, 0, exponent);
        CHECK_FALSE(MatchesNativeNaNResult(got, finite, bits, step));
        put(want, step, exponent | quiet | 1);
        put(got, step, exponent | quiet | 2);
        CHECK_FALSE(MatchesNativeNaNResult(got, want, bits, step));
        put(got, step, exponent | quiet | 1);
        put(want, 16, exponent | quiet | 1);
        put(got, 16, exponent | quiet | 2);
        CHECK_FALSE(MatchesNativeNaNResult(got, want, bits, 16));
        CHECK(MatchesNativeNaNResult(got, want, bits, 32));
    }
}

} // namespace swift::tests
