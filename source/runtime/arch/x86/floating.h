#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace swift::runtime::x86 {

enum class FloatingOperation : std::uint32_t {
    Add,
    Sub,
    Mul,
    Div,
    Min,
    Max,
    Sqrt,
    Reciprocal,
    ReciprocalSqrt,
    CompareFlags,
    CompareMask,
    FromSignedInteger,
    ToSignedInteger,
    ConvertPrecision,
    ConvertPacked,
    RoundIntegral,
    FusedMultiplyAdd
};

enum class FloatingFlag : std::uint32_t {
    Scalar = 1,
    Truncate = 2,
    Signaling = 4,
    SuppressPrecision = 8
};

// One pure numerical call. Unary/round operations read b and merge into a;
// binary operations read a/b and FMA additionally reads c. Scalar conversion
// operations read a[0], with source_bits describing that input and lane_bits
// describing the result. ConvertPacked immediate uses the existing 0..7 kinds.
// exceptions contains only this operation's newly raised MXCSR bits 0..5.
struct FloatingCall {
    std::uint64_t a[2]{}, b[2]{}, c[2]{}, result[2]{};
    std::uint32_t operation{}, lane_bits{}, source_bits{}, control{0x1f80};
    std::uint32_t flags{}, immediate{}, exceptions{}, reserved{};
};

void ExecuteFloating(FloatingCall*) noexcept;

static_assert(std::is_standard_layout_v<FloatingCall> &&
              std::is_trivially_copyable_v<FloatingCall> && sizeof(FloatingCall) == 96);
static_assert(offsetof(FloatingCall, a) == 0 && offsetof(FloatingCall, b) == 16 &&
              offsetof(FloatingCall, c) == 32 && offsetof(FloatingCall, result) == 48);
static_assert(offsetof(FloatingCall, operation) == 64 && offsetof(FloatingCall, lane_bits) == 68 &&
              offsetof(FloatingCall, source_bits) == 72 && offsetof(FloatingCall, control) == 76 &&
              offsetof(FloatingCall, flags) == 80 && offsetof(FloatingCall, immediate) == 84 &&
              offsetof(FloatingCall, exceptions) == 88 && offsetof(FloatingCall, reserved) == 92);
}  // namespace swift::runtime::x86
