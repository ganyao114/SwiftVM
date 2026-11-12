#include "runtime/arch/arm32/numeric.h"
#include "runtime/arch/arm32/state.h"
#include "tests/support/check.h"

namespace {
namespace arch = swift::runtime::arm32;
using arch::Numeric;
using arch::NumericConvert;
using arch::RoundingMode;
using U32 = std::uint32_t;
using U64 = std::uint64_t;

U64 Evaluate(arch::GuestState& state,
             Numeric operation,
             unsigned bits,
             U64 a,
             U64 b,
             bool standard = false) {
    return arch::EvaluateNumeric(
            reinterpret_cast<U64>(&state.fpscr), operation, bits, standard, a, b);
}
}  // namespace

TEST_CASE("ARM32 numerical leaves preserve scalar and NEON FPSCR contracts",
          "[compile][arm32-numeric]") {
    arch::GuestState state;
    CHECK(Evaluate(state, Numeric::Add, 32, 0x3f800000, 0x40000000) == 0x40400000);
    CHECK(Evaluate(state, Numeric::Div, 32, 0x3f800000, 0) == 0x7f800000);
    CHECK(state.fpscr == 2);
    state.fpscr = 1u << 24;
    CHECK(Evaluate(state, Numeric::Add, 32, 1, 0) == 0);
    CHECK(state.fpscr == ((1u << 24) | 128));
    state.fpscr = 0;
    CHECK(Evaluate(state, Numeric::Add, 32, 1, 0, true) == 0);
    CHECK(state.fpscr == 128);
    state.fpscr = 1u << 25;
    CHECK(Evaluate(state, Numeric::Add, 32, 0x7fc12345, 0x3f800000) == 0x7fc00000);
    state.fpscr = 0;
    CHECK(Evaluate(state,
                   Numeric::Round,
                   32,
                   0x40200000,
                   NumericConvert(32, RoundingMode::ToNearest_TieEven)) == 0x40000000);
    CHECK(Evaluate(state,
                   Numeric::Round,
                   32,
                   0x40200000,
                   NumericConvert(32, RoundingMode::TowardsPlusInfinity)) == 0x40400000);
    CHECK(Evaluate(state,
                   Numeric::Round,
                   32,
                   0x40600000,
                   NumericConvert(32, RoundingMode::ToNearest_TieEven, 0, true)) == 0x40800000);
    CHECK(state.fpscr == 16);
}

TEST_CASE("ARM32 half conversion distinguishes alternative half exponent 31",
          "[compile][arm32-numeric]") {
    arch::GuestState state;
    CHECK(Evaluate(state,
                   Numeric::Precision,
                   16,
                   0x3c00,
                   NumericConvert(32, RoundingMode::ToNearest_TieEven)) == 0x3f800000);
    state.fpscr = 1u << 26;
    CHECK(Evaluate(state,
                   Numeric::Precision,
                   16,
                   0x7c00,
                   NumericConvert(32, RoundingMode::ToNearest_TieEven)) == 0x47800000);
    CHECK(Evaluate(state,
                   Numeric::Precision,
                   32,
                   0x47ffe000,
                   NumericConvert(16, RoundingMode::ToNearest_TieEven)) == 0x7fff);
    CHECK(Evaluate(state,
                   Numeric::Precision,
                   32,
                   0x48000000,
                   NumericConvert(16, RoundingMode::ToNearest_TieEven)) == 0x7fff);
    CHECK(state.fpscr == ((1u << 26) | 1));
}

TEST_CASE("ARM binary arithmetic keeps tininess before rounding and exact FZ exceptions",
          "[compile][arm32-numeric]") {
    for (unsigned bits : {32u, 64u}) {
        const U64 one = bits == 32 ? 0x3f800000 : UINT64_C(0x3ff0000000000000);
        const U64 half = bits == 32 ? 0x3f000000 : UINT64_C(0x3fe0000000000000);
        const U64 minimum = bits == 32 ? 0x00800000 : UINT64_C(0x0010000000000000);
        const U64 infinity = bits == 32 ? 0x7f800000 : UINT64_C(0x7ff0000000000000);
        const U64 quiet = bits == 32 ? 0x00400000 : UINT64_C(0x0008000000000000);
        const U64 sign = U64{1} << (bits - 1);
        for (unsigned rounding = 0; rounding < 4; ++rounding) {
            for (bool flush : {false, true}) {
                for (bool default_nan : {false, true}) {
                    const U32 control = (rounding << 22) | (flush ? 1u << 24 : 0) |
                                        (default_nan ? 1u << 25 : 0);
                    const auto check = [&](Numeric operation, U64 a, U64 b, U64 value, U32 flags) {
                        arch::GuestState state;
                        state.fpscr = control | (1u << 27);  // QC survives scalar arithmetic.
                        CHECK(Evaluate(state, operation, bits, a, b) == value);
                        CHECK(state.fpscr == (control | (1u << 27) | flags));
                    };
                    check(Numeric::Add, sign, 0, rounding == 2 ? sign : 0, 0);
                    check(Numeric::Sub, one, one, rounding == 2 ? sign : 0, 0);
                    check(Numeric::Mul, minimum, half, flush ? 0 : minimum / 2, flush ? 8 : 0);
                    // Exact product lies halfway below MinNorm. FZ must flush
                    // even when the selected rounding would produce MinNorm.
                    const U64 rounded = rounding < 2 ? minimum : minimum - 1;
                    check(Numeric::Mul, minimum, one - 1, flush ? 0 : rounded, flush ? 8 : 24);
                    check(Numeric::Mul,
                          infinity - 1,
                          one + (one - half),
                          rounding < 2 ? infinity : infinity - 1,
                          20);
                    check(Numeric::Mul, infinity, 0, infinity | quiet, 1);
                    // Tiny finite corrections must survive until directed
                    // rounding, even with an exponent gap beyond binary128.
                    check(Numeric::Add, one, 1, one + (!flush && rounding == 1), flush ? 128 : 16);
                    check(Numeric::Sub,
                          one | sign,
                          1,
                          sign | (one + (!flush && rounding == 2)),
                          flush ? 128 : 16);
                    check(Numeric::Add,
                          infinity | 0x42,
                          1,
                          infinity | quiet | (default_nan ? 0 : 0x42),
                          flush ? 129 : 1);
                }
            }
        }
    }
}

TEST_CASE("ARM fused arithmetic retains tiny addends during directed rounding",
          "[compile][arm32-numeric]") {
    for (unsigned bits : {32u, 64u}) {
        const U64 one = bits == 32 ? 0x3f800000 : UINT64_C(0x3ff0000000000000);
        const U64 sign = U64{1} << (bits - 1);
        for (unsigned rounding = 0; rounding < 4; ++rounding) {
            arch::GuestState state;
            state.fpscr = rounding << 22;
            state.numeric_addend = 1;
            CHECK(Evaluate(state, Numeric::Fused, bits, one, one) == one + (rounding == 1));
            CHECK(state.fpscr == ((rounding << 22) | 16));
            state.fpscr = rounding << 22;
            state.numeric_addend = sign | 1;
            CHECK(Evaluate(state, Numeric::Fused, bits, sign | one, one) ==
                  (sign | (one + (rounding == 2))));
            CHECK(state.fpscr == ((rounding << 22) | 16));
        }
    }
}

TEST_CASE("ARM32 VMLA and VFMA retain their distinct rounding", "[compile][arm32-numeric]") {
    arch::GuestState state;
    state.numeric_addend = 0xbf800000;
    CHECK(Evaluate(state, Numeric::Fused, 32, 0x3f800001, 0x3f7ffffe) == 0xa8800000);
    const auto product = Evaluate(state, Numeric::Mul, 32, 0x3f800001, 0x3f7ffffe);
    CHECK(Evaluate(state, Numeric::Add, 32, product, 0xbf800000) == 0);
}

TEST_CASE("ARM32 numerical saturation and NaN comparisons accumulate independent flags",
          "[compile][arm32-numeric]") {
    arch::GuestState state;
    CHECK(Evaluate(state, Numeric::SaturatingAddUnsigned, 8, 255, 1) == 255);
    CHECK(state.fpscr == (1u << 27));
    state.fpscr = 0;
    CHECK(Evaluate(state, Numeric::SaturatingNeg, 8, 128, 0) == 127);
    CHECK(state.fpscr == (1u << 27));
    state.fpscr = 0;
    CHECK(Evaluate(state, Numeric::CompareFlags, 32, 0x7fc12345, 0) == 0x30000000);
    CHECK(state.fpscr == 0);
    CHECK(Evaluate(state, Numeric::CompareFlagsSignal, 32, 0x7fc12345, 0) == 0x30000000);
    CHECK(state.fpscr == 1);
}
