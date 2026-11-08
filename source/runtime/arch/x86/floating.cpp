#include "floating.h"

#define LITTLEENDIAN 1
#define SOFTFLOAT_FAST_INT64 1
#define FEXCORE_PRESERVE_ALL_ATTR
extern "C" {
#include <SoftFloat-3e/softfloat.h>
}
#undef FEXCORE_PRESERVE_ALL_ATTR
#undef SOFTFLOAT_FAST_INT64
#undef LITTLEENDIAN

namespace swift::runtime::x86 {
namespace {
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using Op = FloatingOperation;

constexpr u32 Invalid = 1, Denormal = 2, DivideByZero = 4, Underflow = 16, Precision = 32;
constexpr u32 Daz = 64, OverflowMask = 1u << 10, UnderflowMask = 1u << 11, Ftz = 1u << 15;

struct WideResult {
    float128_t value;
    bool inexact;
};

bool Has(const FloatingCall& call, FloatingFlag flag) noexcept {
    return call.flags & static_cast<u32>(flag);
}

unsigned Rounding(unsigned rc) noexcept {
    constexpr unsigned modes[]{softfloat_round_near_even,
                               softfloat_round_min,
                               softfloat_round_max,
                               softfloat_round_minMag};
    return modes[rc & 3];
}

softfloat_state State(unsigned control) noexcept {
    return {softfloat_tininess_afterRounding,
            static_cast<std::uint8_t>(Rounding(control >> 13)),
            0,
            80,
            0};
}

u32 Exceptions(const softfloat_state& state) noexcept {
    const auto flags = state.exceptionFlags;
    return ((flags & softfloat_flag_invalid) ? Invalid : 0) |
           ((flags & softfloat_flag_infinite) ? DivideByZero : 0) |
           ((flags & softfloat_flag_overflow) ? 8 : 0) |
           ((flags & softfloat_flag_underflow) ? Underflow : 0) |
           ((flags & softfloat_flag_inexact) ? Precision : 0);
}

template <unsigned Bits> struct Format {
    static constexpr u64 Sign = u64{1} << (Bits - 1);
    static constexpr u64 Fraction = Bits == 32 ? 0x007fffffu : UINT64_C(0x000fffffffffffff);
    static constexpr u64 Exponent = Bits == 32 ? 0x7f800000u : UINT64_C(0x7ff0000000000000);
    static constexpr u64 Quiet = (Fraction + 1) >> 1;
    static constexpr u64 One = Bits == 32 ? 0x3f800000u : UINT64_C(0x3ff0000000000000);
    static constexpr u64 Mask = Bits == 32 ? UINT64_C(0xffffffff) : UINT64_MAX;

    static bool IsNaN(u64 value) noexcept {
        return (value & Exponent) == Exponent && (value & Fraction);
    }

    static bool IsSignaling(u64 value) noexcept { return IsNaN(value) && !(value & Quiet); }

    static bool IsDenormal(u64 value) noexcept { return !(value & Exponent) && (value & Fraction); }

    static u64 Input(u64 value, unsigned control) noexcept {
        return (control & Daz) && IsDenormal(value) ? value & Sign : value;
    }

    static u64 Get(const u64* source, unsigned lane) noexcept {
        if constexpr (Bits == 32)
            return static_cast<u32>(source[lane / 2] >> ((lane & 1) * 32));
        else
            return source[lane];
    }

    static void Put(u64* target, unsigned lane, u64 value) noexcept {
        if constexpr (Bits == 32) {
            const auto shift = (lane & 1) * 32;
            target[lane / 2] = (target[lane / 2] & ~(Mask << shift)) | ((value & Mask) << shift);
        } else
            target[lane] = value;
    }

    // IEEE ordering without touching the host floating-point environment.
    static unsigned Relation(u64 a, u64 b) noexcept {
        if (IsNaN(a) || IsNaN(b)) return 8;
        if (a == b || !((a | b) & ~Sign)) return 2;
        if ((a ^ b) & Sign) return a & Sign ? 1 : 4;
        return ((a < b) != bool(a & Sign)) ? 1 : 4;
    }

    static u64 Arithmetic(Op operation, softfloat_state& state, u64 a, u64 b, u64 c) noexcept {
        if constexpr (Bits == 32) {
            const float32_t x{static_cast<u32>(a)}, y{static_cast<u32>(b)}, z{static_cast<u32>(c)};
            switch (operation) {
                case Op::Add:
                    return f32_add(&state, x, y).v;
                case Op::Sub:
                    return f32_sub(&state, x, y).v;
                case Op::Mul:
                    return f32_mul(&state, x, y).v;
                case Op::Div:
                    return f32_div(&state, x, y).v;
                case Op::Sqrt:
                    return f32_sqrt(&state, y).v;
                case Op::RoundIntegral:
                    return f32_roundToInt(&state, y, state.roundingMode, true).v;
                case Op::FusedMultiplyAdd:
                    return f32_mulAdd(&state, x, y, z).v;
                default:
                    return 0;
            }
        } else {
            const float64_t x{a}, y{b}, z{c};
            switch (operation) {
                case Op::Add:
                    return f64_add(&state, x, y).v;
                case Op::Sub:
                    return f64_sub(&state, x, y).v;
                case Op::Mul:
                    return f64_mul(&state, x, y).v;
                case Op::Div:
                    return f64_div(&state, x, y).v;
                case Op::Sqrt:
                    return f64_sqrt(&state, y).v;
                case Op::RoundIntegral:
                    return f64_roundToInt(&state, y, state.roundingMode, true).v;
                case Op::FusedMultiplyAdd:
                    return f64_mulAdd(&state, x, y, z).v;
                default:
                    return 0;
            }
        }
    }

    static float128_t Widen(softfloat_state& state, u64 value) noexcept {
        if constexpr (Bits == 32)
            return f32_to_f128(&state, float32_t{static_cast<u32>(value)});
        else
            return f64_to_f128(&state, float64_t{value});
    }

    static WideResult WideArithmetic(Op op, u64 a, u64 b, u64 c) noexcept {
        // Binary128's exponent range covers every finite F32/F64 intermediate.
        // A product has at most 106 bits, so the FMA product is exact here.
        auto state = State(0x1f80);
        state.roundingMode = softfloat_round_minMag;
        const auto x = Widen(state, a), y = Widen(state, b), z = Widen(state, c);
        float128_t result{};
        switch (op) {
            case Op::Add:
                result = f128_add(&state, x, y);
                break;
            case Op::Sub:
                result = f128_sub(&state, x, y);
                break;
            case Op::Mul:
                result = f128_mul(&state, x, y);
                break;
            case Op::Div:
                result = f128_div(&state, x, y);
                break;
            case Op::Sqrt:
                result = f128_sqrt(&state, y);
                break;
            case Op::FusedMultiplyAdd:
                result = f128_add(&state, f128_mul(&state, x, y), z);
                break;
            default:
                break;
        }
        return {result, bool(state.exceptionFlags & softfloat_flag_inexact)};
    }

    template <class Evaluate> static u32 PrecisePost(const FloatingCall& call,
                                                     u64 result,
                                                     u32 raised,
                                                     Evaluate&& evaluate) noexcept {
        constexpr u64 MinNormal = Fraction + 1;
        const bool check_underflow =
                !(call.control & UnderflowMask) && (result & ~Sign) <= MinNormal;
        const bool check_overflow = !(call.control & OverflowMask) && (raised & 8);
        if (!check_underflow && !check_overflow) return raised;
        const auto wide = evaluate();
        const u64 high = wide.value.v[1];
        const bool negative = high >> 63;
        int exponent = static_cast<int>((high >> 48) & 0x7fff) - 16383;
        if (!((high & UINT64_C(0x7fffffffffffffff)) | wide.value.v[0])) {
            return raised & ~(Precision | Underflow);
        }
        using u128 = unsigned __int128;
        constexpr unsigned precision = Bits == 32 ? 24 : 53, discarded = 113 - precision;
        const auto significand =
                (u128{(high & UINT64_C(0x0000ffffffffffff)) | (u64{1} << 48)} << 64) |
                wide.value.v[0];
        auto retained = static_cast<u64>(significand >> discarded);
        const auto tail = significand & ((u128{1} << discarded) - 1);
        const bool inexact = tail || wide.inexact;
        const auto rounding = (call.control >> 13) & 3;
        bool increment{};
        if (rounding == 0) {
            const auto half = u128{1} << (discarded - 1);
            increment = tail > half || (tail == half && (wide.inexact || (retained & 1)));
        } else if (rounding != 3)
            increment = inexact && (rounding == 1 ? negative : !negative);
        if (increment && ++retained == (u64{1} << precision)) ++exponent;
        raised = (raised & ~Precision) | (inexact ? Precision : 0);
        if (check_underflow) {
            const bool tiny = exponent < (Bits == 32 ? -126 : -1022);
            raised = (raised & ~Underflow) | (tiny ? Underflow : 0);
        }
        return raised;
    }

    static u64 Finish(FloatingCall& call,
                      u32 raised,
                      u64 result,
                      bool denormal_input,
                      bool nan_input,
                      bool output_underflow = true) noexcept {
        // SDM 4.9.2: NaN handling and other pre-computation IE/ZE take
        // precedence over a denormal operand in the same lane.
        if (denormal_input && !nan_input && !(raised & (Invalid | DivideByZero)))
            raised |= Denormal;
        if (output_underflow && IsDenormal(result) && (call.control & UnderflowMask) &&
            (call.control & Ftz)) {
            result &= Sign;
            raised |= Underflow | Precision;
        }
        call.exceptions |= raised;
        return result;
    }

    static u64 Estimate(Op operation, u64 value) noexcept {
        // RCP/RSQRT always treat denormal operands as signed zero, ignore RC,
        // flush tiny results, and do not signal any floating-point exception.
        if (IsNaN(value)) return value | Quiet;
        if (IsDenormal(value)) value &= Sign;
        auto state = State(0x1f80);
        if (operation == Op::ReciprocalSqrt) value = Arithmetic(Op::Sqrt, state, 0, value, 0);
        auto result = Arithmetic(Op::Div, state, One, value, 0);
        return IsDenormal(result) ? result & Sign : result;
    }
};

template <unsigned Bits> void Vector(FloatingCall& call) noexcept {
    using F = Format<Bits>;
    const auto op = static_cast<Op>(call.operation);
    const bool compare = op == Op::CompareMask || op == Op::CompareFlags;
    const bool minmax = op == Op::Min || op == Op::Max;
    const bool unary = op == Op::Sqrt || op == Op::RoundIntegral || op == Op::Reciprocal ||
                       op == Op::ReciprocalSqrt;
    const unsigned count = Has(call, FloatingFlag::Scalar) ? 1 : 128 / Bits;
    for (unsigned lane = 0; lane < count; ++lane) {
        const auto original_a = F::Get(call.a, lane), original_b = F::Get(call.b, lane);
        const auto original_c = op == Op::FusedMultiplyAdd ? F::Get(call.c, lane) : 0;
        auto a = F::Input(original_a, call.control), b = F::Input(original_b, call.control);
        auto c = F::Input(original_c, call.control);
        const bool nan = F::IsNaN(b) || (!unary && F::IsNaN(a)) || F::IsNaN(c);
        const bool denormal = F::IsDenormal(b) || (!unary && F::IsDenormal(a)) || F::IsDenormal(c);
        auto state = State(call.control);
        u64 result{};
        if (compare || minmax) {
            const auto relation = F::Relation(a, b);
            if ((minmax || Has(call, FloatingFlag::Signaling))
                        ? nan
                        : (F::IsSignaling(a) || F::IsSignaling(b)))
                state.exceptionFlags |= softfloat_flag_invalid;
            if (op == Op::CompareFlags) {
                result = relation == 8 ? 7 : relation == 1 ? 1 : relation == 2 ? 4 : 0;
                call.result[0] = F::Finish(call, Exceptions(state), result, denormal, nan, false);
                call.result[1] = 0;
                return;
            }
            if (minmax)
                result = relation == (op == Op::Min ? 1u : 4u) ? a : b;
            else
                result = call.immediate & relation ? F::Mask : 0;
            result = F::Finish(call, Exceptions(state), result, denormal, nan, false);
        } else if (op == Op::Reciprocal || op == Op::ReciprocalSqrt) {
            result = F::Estimate(op, original_b);
        } else {
            if (op == Op::RoundIntegral) state.roundingMode = Rounding(call.immediate);
            if (op == Op::FusedMultiplyAdd) {
                if (call.immediate & 1) a ^= F::Sign;
                const bool negate_add =
                        bool(call.immediate & 2) ^
                        (bool(call.immediate & 8) && ((lane & 1) == ((call.immediate >> 4) & 1)));
                if (negate_add) c ^= F::Sign;
            }
            result = F::Arithmetic(op, state, a, b, c);
            // FMA's sign modifiers do not modify a propagated source NaN.
            // Still evaluate first: a later SNaN or invalid product raises IE.
            if (op == Op::FusedMultiplyAdd && nan)
                result = (F::IsNaN(original_a)   ? original_a
                          : F::IsNaN(original_b) ? original_b
                                                 : original_c) |
                         F::Quiet;
            if (op == Op::RoundIntegral && Has(call, FloatingFlag::SuppressPrecision))
                state.exceptionFlags &= ~softfloat_flag_inexact;
            auto raised = Exceptions(state);
            if (op != Op::RoundIntegral && !(raised & (Invalid | DivideByZero)))
                raised = F::PrecisePost(
                        call, result, raised, [&] { return F::WideArithmetic(op, a, b, c); });
            result = F::Finish(call,
                               raised,
                               result,
                               op != Op::RoundIntegral && denormal,
                               nan,
                               op != Op::RoundIntegral);
        }
        F::Put(call.result, lane, result);
    }
}

u64 FromInteger(softfloat_state& state,
                u64 input,
                unsigned source_bits,
                unsigned output_bits) noexcept {
    if (source_bits <= 32) {
        const auto value = source_bits == 16 ? static_cast<std::int16_t>(input)
                                             : static_cast<std::int32_t>(input);
        return output_bits == 32 ? i32_to_f32(&state, value).v : i32_to_f64(&state, value).v;
    }
    const auto value = static_cast<std::int64_t>(input);
    return output_bits == 32 ? i64_to_f32(&state, value).v : i64_to_f64(&state, value).v;
}

template <unsigned Bits>
u64 ToInteger(FloatingCall& call, u64 input, unsigned output_bits, bool truncate) noexcept {
    using F = Format<Bits>;
    input = F::Input(input, call.control);
    auto state = State(call.control);
    const auto rounding =
            truncate ? static_cast<unsigned>(softfloat_round_minMag) : state.roundingMode;
    u64 result;
    if constexpr (Bits == 32) {
        const float32_t value{static_cast<u32>(input)};
        result = output_bits <= 32 ? static_cast<u32>(f32_to_i32(&state, value, rounding, true))
                                   : static_cast<u64>(f32_to_i64(&state, value, rounding, true));
    } else {
        const float64_t value{input};
        result = output_bits <= 32 ? static_cast<u32>(f64_to_i32(&state, value, rounding, true))
                                   : static_cast<u64>(f64_to_i64(&state, value, rounding, true));
    }
    if (output_bits == 16) {
        const auto value = static_cast<std::int32_t>(result);
        if ((state.exceptionFlags & softfloat_flag_invalid) || value < -32768 || value > 32767) {
            result = 0x8000;
            state.exceptionFlags = softfloat_flag_invalid;
        } else
            result = static_cast<std::uint16_t>(value);
    }
    // Float-to-integer conversion applies DAZ but never raises DE.
    call.exceptions |= Exceptions(state);
    return result;
}

template <unsigned Bits> u64 Convert(FloatingCall& call, u64 input) noexcept {
    using F = Format<Bits>;
    input = F::Input(input, call.control);
    auto state = State(call.control);
    if constexpr (Bits == 32) {
        const auto result = f32_to_f64(&state, float32_t{static_cast<u32>(input)}).v;
        auto raised = Exceptions(state);
        raised = Format<64>::PrecisePost(call, result, raised, [&] {
            auto wide_state = State(call.control);
            return WideResult{F::Widen(wide_state, input), false};
        });
        return Format<64>::Finish(call, raised, result, F::IsDenormal(input), F::IsNaN(input));
    } else {
        const auto result = f64_to_f32(&state, float64_t{input}).v;
        auto raised = Exceptions(state);
        raised = Format<32>::PrecisePost(call, result, raised, [&] {
            auto wide_state = State(call.control);
            return WideResult{F::Widen(wide_state, input), false};
        });
        return Format<32>::Finish(call, raised, result, F::IsDenormal(input), F::IsNaN(input));
    }
}

void Packed(FloatingCall& call) noexcept {
    const auto kind = call.immediate;
    call.result[0] = call.result[1] = 0;
    if (kind <= 1) {
        const unsigned width = kind == 0 ? 32 : 64;
        for (unsigned i = 0; i < 128 / width; ++i) {
            auto state = State(call.control);
            const auto value = FromInteger(state, Format<32>::Get(call.a, i), 32, width);
            call.exceptions |= Exceptions(state);
            if (width == 32)
                Format<32>::Put(call.result, i, value);
            else
                Format<64>::Put(call.result, i, value);
        }
    } else if (kind <= 5) {
        const unsigned width = kind <= 3 ? 32 : 64;
        for (unsigned i = 0; i < 128 / width; ++i) {
            const auto value =
                    width == 32 ? ToInteger<32>(call, Format<32>::Get(call.a, i), 32, kind == 3)
                                : ToInteger<64>(call, Format<64>::Get(call.a, i), 32, kind == 5);
            Format<32>::Put(call.result, i, value);
        }
    } else if (kind == 6) {
        for (unsigned i = 0; i < 2; ++i)
            Format<64>::Put(call.result, i, Convert<32>(call, Format<32>::Get(call.a, i)));
    } else if (kind == 7) {
        for (unsigned i = 0; i < 2; ++i)
            Format<32>::Put(call.result, i, Convert<64>(call, Format<64>::Get(call.a, i)));
    }
}
}  // namespace

void ExecuteFloating(FloatingCall* packet) noexcept {
    auto& call = *packet;
    const auto op = static_cast<Op>(call.operation);
    call.exceptions = 0;
    call.result[0] = call.a[0];
    call.result[1] = call.a[1];
    if (op == Op::ConvertPacked) {
        Packed(call);
    } else if (op == Op::FromSignedInteger) {
        auto state = State(call.control);
        call.result[0] = FromInteger(state, call.a[0], call.source_bits, call.lane_bits);
        call.result[1] = 0;
        call.exceptions = Exceptions(state);
    } else if (op == Op::ToSignedInteger) {
        call.result[0] = call.source_bits == 32 ? ToInteger<32>(call,
                                                                call.a[0],
                                                                call.lane_bits,
                                                                Has(call, FloatingFlag::Truncate))
                                                : ToInteger<64>(call,
                                                                call.a[0],
                                                                call.lane_bits,
                                                                Has(call, FloatingFlag::Truncate));
        call.result[1] = 0;
    } else if (op == Op::ConvertPrecision) {
        call.result[0] = call.source_bits == 32 ? Convert<32>(call, call.a[0])
                                                : Convert<64>(call, call.a[0]);
        call.result[1] = 0;
    } else if (call.lane_bits == 32)
        Vector<32>(call);
    else
        Vector<64>(call);
}
}  // namespace swift::runtime::x86
