#include "runtime/arch/arm32/numeric.h"
#include "runtime/arch/arm32/state.h"
#include "tests/support/check.h"

namespace {
namespace arch = swift::runtime::arm32;
using arch::Numeric;
using arch::NumericConvert;
using arch::RoundingMode;
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
