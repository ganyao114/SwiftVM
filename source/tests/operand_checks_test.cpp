#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/operand_checks.h"

TEST_CASE("register operands exclude shifts and narrowing extensions", "[codegen]") {
    using namespace vixl::aarch64;
    using swift::runtime::backend::arm64::IsUnmodifiedRegister;
    CHECK_FALSE(IsUnmodifiedRegister(Operand{0}));
    for (auto shift : {LSL, LSR, ASR, ROR}) {
        CHECK(IsUnmodifiedRegister(Operand{x1, shift, 0}));
        CHECK_FALSE(IsUnmodifiedRegister(Operand{x1, shift, 1}));
    }
    for (auto extend : {UXTX, SXTX}) {
        CHECK(IsUnmodifiedRegister(Operand{x1, extend, 0}));
        CHECK_FALSE(IsUnmodifiedRegister(Operand{x1, extend, 1}));
    }
    for (auto extend : {UXTB, UXTH, UXTW, SXTB, SXTH, SXTW}) {
        CHECK_FALSE(IsUnmodifiedRegister(Operand{w1, extend, 0}));
    }
}
