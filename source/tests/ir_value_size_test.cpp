#include <array>
#include <cfenv>
#include <utility>
#include <catch2/catch_test_macros.hpp>
#include "runtime/ir/ir_types.h"

using swift::runtime::ir::ValueType;
namespace {
struct RestoreEnvironment {
    fenv_t environment{};
    RestoreEnvironment() { fegetenv(&environment); }
    ~RestoreEnvironment() { fesetenv(&environment); }
};
}

TEST_CASE("IR byte widths are exact under every host rounding mode without FP side effects") {
    const std::array<std::pair<ValueType, unsigned>, 14> widths{{
        {ValueType::U8, 1}, {ValueType::U16, 2}, {ValueType::U32, 4}, {ValueType::U64, 8},
        {ValueType::S8, 1}, {ValueType::S16, 2}, {ValueType::S32, 4}, {ValueType::S64, 8},
        {ValueType::V8, 1}, {ValueType::V16, 2}, {ValueType::V32, 4}, {ValueType::V64, 8},
        {ValueType::V128, 16}, {ValueType::V256, 32}}};
    RestoreEnvironment restore;
    for (int rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        REQUIRE(fesetround(rounding) == 0);
        REQUIRE(feclearexcept(FE_ALL_EXCEPT) == 0);
        // Seed an observable sticky flag: an integer metadata query must
        // neither clear existing FP exceptions nor introduce new ones.
        REQUIRE(feraiseexcept(FE_INVALID) == 0);
        const int exceptions = fetestexcept(FE_ALL_EXCEPT);
        for (const auto [type, bytes] : widths) {
            INFO("rounding=" << rounding << " type=" << unsigned(type));
            REQUIRE(swift::runtime::ir::GetValueSizeByte(type) == bytes);
            REQUIRE(fegetround() == rounding);
            REQUIRE(fetestexcept(FE_ALL_EXCEPT) == exceptions);
        }
    }
}
