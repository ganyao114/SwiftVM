#include "runtime/arch/x86/floating.h"

#include <cstdint>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace swift::runtime::x86;
using u64 = std::uint64_t;
using u32 = std::uint32_t;

FloatingCall Call(FloatingOperation operation,
                  unsigned bits,
                  u64 a = 0,
                  u64 b = 0,
                  u64 c = 0,
                  unsigned control = 0x1f80) {
    FloatingCall call{};
    call.operation = static_cast<u32>(operation);
    call.lane_bits = bits;
    call.flags = static_cast<u32>(FloatingFlag::Scalar);
    call.a[0] = a;
    call.b[0] = b;
    call.c[0] = c;
    call.control = control;
    return call;
}

struct Format {
    unsigned bits;
    u64 one, two, half, infinity, qnan, snan, maximum, normal, below_one;
};

constexpr Format Formats[]{{32,
                            0x3f800000,
                            0x40000000,
                            0x3f000000,
                            0x7f800000,
                            0x7fc00123,
                            0x7f800123,
                            0x7f7fffff,
                            0x00800000,
                            0x3f7fffff},
                           {64,
                            UINT64_C(0x3ff0000000000000),
                            UINT64_C(0x4000000000000000),
                            UINT64_C(0x3fe0000000000000),
                            UINT64_C(0x7ff0000000000000),
                            UINT64_C(0x7ff8000000000123),
                            UINT64_C(0x7ff0000000000123),
                            UINT64_C(0x7fefffffffffffff),
                            UINT64_C(0x0010000000000000),
                            UINT64_C(0x3fefffffffffffff)}};
}  // namespace

TEST_CASE("SSE numerical arithmetic reports six MXCSR exceptions", "[compile][x86-floating]") {
    for (const auto f : Formats) {
        CAPTURE(f.bits);
        auto call = Call(FloatingOperation::Div, f.bits, 0, 0);
        ExecuteFloating(&call);
        CHECK(call.exceptions == 1);
        CHECK(call.result[0] == ((u64{1} << (f.bits - 1)) | f.qnan) - 0x123);
        call = Call(FloatingOperation::Div, f.bits, f.one, 0);
        ExecuteFloating(&call);
        CHECK(call.exceptions == 4);
        CHECK(call.result[0] == f.infinity);
        call = Call(FloatingOperation::Add, f.bits, f.one, 1);
        ExecuteFloating(&call);
        CHECK(call.exceptions == (2 | 32));
        call.control |= 64;
        ExecuteFloating(&call);
        CHECK(call.exceptions == 0);
        call = Call(FloatingOperation::Div, f.bits, 1, 0);
        ExecuteFloating(&call);
        CHECK(call.exceptions == 4);
        call = Call(FloatingOperation::Add, f.bits, f.qnan, 1);
        ExecuteFloating(&call);
        CHECK(call.exceptions == 0);
        CHECK(call.result[0] == f.qnan);
        call = Call(FloatingOperation::Mul, f.bits, f.maximum, f.two);
        ExecuteFloating(&call);
        CHECK(call.exceptions == (8 | 32));
        call = Call(FloatingOperation::Mul, f.bits, f.normal, f.half);
        ExecuteFloating(&call);
        CHECK(call.exceptions == 0);
        CHECK(call.result[0] == f.normal / 2);
        call.control |= 0x8000;
        ExecuteFloating(&call);
        CHECK(call.exceptions == (16 | 32));
        CHECK(call.result[0] == 0);
    }
}

TEST_CASE("unmasked range faults use precision before exponent bounding",
          "[compile][x86-floating]") {
    for (const auto f : Formats) {
        CAPTURE(f.bits);
        auto call = Call(FloatingOperation::Mul, f.bits, f.maximum, f.two, 0, 0x1b80);
        ExecuteFloating(&call);
        CHECK(call.exceptions == 8);
        call = Call(FloatingOperation::Mul, f.bits, f.normal, f.half, 0, 0x9780);
        ExecuteFloating(&call);
        CHECK(call.exceptions == 16);
        CHECK(call.result[0] == f.normal / 2);  // FTZ is inactive while UM=0.
        call = Call(FloatingOperation::Mul, f.bits, f.normal, f.below_one, 0, 0x1780);
        ExecuteFloating(&call);
        CHECK(call.result[0] == f.normal);
        CHECK(call.exceptions == 16);  // Exact at unbounded destination precision.
        call = Call(FloatingOperation::Div,
                    f.bits,
                    f.normal,
                    f.bits == 32 ? 0x40400000 : UINT64_C(0x4008000000000000),
                    0,
                    0x1780);
        ExecuteFloating(&call);
        CHECK(call.exceptions == (16 | 32));
    }
}

TEST_CASE("SSE compare selection preserves signaling and raw operands", "[compile][x86-floating]") {
    for (const auto f : Formats) {
        CAPTURE(f.bits);
        auto call = Call(FloatingOperation::CompareFlags, f.bits, f.one, f.qnan);
        ExecuteFloating(&call);
        CHECK(call.result[0] == 7);
        CHECK(call.exceptions == 0);
        call.flags |= static_cast<u32>(FloatingFlag::Signaling);
        ExecuteFloating(&call);
        CHECK(call.exceptions == 1);
        call.b[0] = f.snan;
        call.flags = static_cast<u32>(FloatingFlag::Scalar);
        ExecuteFloating(&call);
        CHECK(call.exceptions == 1);
        call = Call(FloatingOperation::Min, f.bits, f.one, f.snan);
        ExecuteFloating(&call);
        CHECK(call.result[0] == f.snan);  // MIN forwards source2, without quieting it.
        CHECK(call.exceptions == 1);
        call = Call(FloatingOperation::Max, f.bits, 0, u64{1} << (f.bits - 1));
        ExecuteFloating(&call);
        CHECK(call.result[0] == call.b[0]);
        call = Call(FloatingOperation::CompareMask, f.bits, f.one, f.qnan);
        call.immediate = 0;  // Even a constant-false predicate signals an SNaN.
        ExecuteFloating(&call);
        CHECK(call.result[0] == 0);
        CHECK(call.exceptions == 0);
        call.b[0] = f.snan;
        ExecuteFloating(&call);
        CHECK(call.exceptions == 1);
        call.b[0] = f.qnan;
        call.immediate = 15;
        ExecuteFloating(&call);
        CHECK(call.result[0] == (f.bits == 32 ? UINT32_MAX : UINT64_MAX));
        CHECK(call.exceptions == 0);
    }
}

TEST_CASE("SSE round and estimates have distinct exception rules", "[compile][x86-floating]") {
    for (const auto f : Formats) {
        CAPTURE(f.bits);
        auto call = Call(FloatingOperation::RoundIntegral, f.bits, 0, 1);
        call.a[1] = 0x123456789;
        ExecuteFloating(&call);
        CHECK(call.result[0] == 0);
        CHECK(call.result[1] == call.a[1]);
        CHECK(call.exceptions == 32);  // No DE for ROUND.
        call.flags |= static_cast<u32>(FloatingFlag::SuppressPrecision);
        ExecuteFloating(&call);
        CHECK(call.exceptions == 0);
        call.b[0] = f.snan;
        ExecuteFloating(&call);
        CHECK(call.exceptions == 1);
        call = Call(FloatingOperation::Sqrt, f.bits, 0, (u64{1} << (f.bits - 1)) | 1);
        ExecuteFloating(&call);
        CHECK(call.exceptions == 1);  // Invalid negative root takes priority over DE.
        for (auto operation : {FloatingOperation::Reciprocal, FloatingOperation::ReciprocalSqrt}) {
            call = Call(operation, f.bits, 0, (u64{1} << (f.bits - 1)) | 1);
            ExecuteFloating(&call);
            CHECK(call.result[0] == ((u64{1} << (f.bits - 1)) | f.infinity));
            CHECK(call.exceptions == 0);
        }
    }
}

TEST_CASE("SSE conversions apply DAZ and aggregate active packed lanes",
          "[compile][x86-floating]") {
    for (const auto f : Formats) {
        auto call = Call(FloatingOperation::ToSignedInteger, 32, 1);
        call.source_bits = f.bits;
        call.flags = static_cast<u32>(FloatingFlag::Truncate);
        ExecuteFloating(&call);
        CHECK(call.result[0] == 0);
        CHECK(call.exceptions == 32);  // No DE even for CVTT.
        call.control |= 64;
        ExecuteFloating(&call);
        CHECK(call.exceptions == 0);
        call.a[0] = f.qnan;
        ExecuteFloating(&call);
        CHECK(call.result[0] == 0x80000000);
        CHECK(call.exceptions == 1);
        call = Call(FloatingOperation::ToSignedInteger,
                    16,
                    f.bits == 32 ? 0x4788b800 : UINT64_C(0x40f1170000000000));
        call.source_bits = f.bits;
        ExecuteFloating(&call);
        CHECK(call.result[0] == 0x8000);
        CHECK(call.exceptions == 1);
        call.a[0] = f.one | (u64{1} << (f.bits - 1));
        ExecuteFloating(&call);
        CHECK(call.result[0] == 0xffff);
        CHECK(call.exceptions == 0);
    }
    auto call = Call(FloatingOperation::FromSignedInteger, 32, 0xffff);
    call.source_bits = 16;
    ExecuteFloating(&call);
    CHECK(call.result[0] == 0xbf800000);
    CHECK(call.exceptions == 0);
    call.lane_bits = 64;
    ExecuteFloating(&call);
    CHECK(call.result[0] == UINT64_C(0xbff0000000000000));
    CHECK(call.exceptions == 0);
    call = Call(FloatingOperation::FromSignedInteger, 32, 16777217);
    call.source_bits = 32;
    call.control = 0x5f80;
    ExecuteFloating(&call);
    CHECK(call.result[0] == 0x4b800001);
    CHECK(call.exceptions == 32);
    call = Call(FloatingOperation::ConvertPrecision, 64, 1);
    call.source_bits = 32;
    ExecuteFloating(&call);
    CHECK(call.result[0] == UINT64_C(0x36a0000000000000));
    CHECK(call.exceptions == 2);
    call = Call(FloatingOperation::ConvertPacked, 0, UINT64_C(0x400000003f800000));
    call.a[1] = UINT64_C(0x4080000040400000);
    call.immediate = 2;
    ExecuteFloating(&call);
    CHECK(call.result[0] == UINT64_C(0x0000000200000001));
    CHECK(call.result[1] == UINT64_C(0x0000000400000003));
    CHECK(call.exceptions == 0);
    call.immediate = 6;
    ExecuteFloating(&call);
    CHECK(call.result[0] == UINT64_C(0x3ff0000000000000));
    CHECK(call.result[1] == UINT64_C(0x4000000000000000));
    call.immediate = 7;
    call.a[0] = UINT64_C(0x3ff0000000000000);
    call.a[1] = UINT64_C(0x4000000000000000);
    ExecuteFloating(&call);
    CHECK(call.result[0] == UINT64_C(0x400000003f800000));
    CHECK(call.result[1] == 0);
}

TEST_CASE("SSE fused operations retain one rounding and source NaN signs",
          "[compile][x86-floating]") {
    for (const auto f : Formats) {
        CAPTURE(f.bits);
        auto call = Call(FloatingOperation::FusedMultiplyAdd,
                         f.bits,
                         f.one,
                         f.one,
                         (u64{1} << (f.bits - 1)) | f.one,
                         0x3f80);
        ExecuteFloating(&call);
        CHECK(call.result[0] == (u64{1} << (f.bits - 1)));
        CHECK(call.exceptions == 0);
        call.a[0] = f.qnan;
        call.b[0] = f.snan;
        call.immediate = 3;
        ExecuteFloating(&call);
        CHECK(call.result[0] == f.qnan);
        CHECK(call.exceptions == 1);
    }
}
