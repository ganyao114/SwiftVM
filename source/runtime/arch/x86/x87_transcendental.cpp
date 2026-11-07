#include "x87_detail.h"
#include "runtime/externals/cephes/include/cephes_128bit.h"
#include <array>

namespace swift::runtime::x86::detail {
namespace {
constexpr float128_t qzero{0, 0}, qone{0, UINT64_C(0x3fff000000000000)};
constexpr float128_t qpi{UINT64_C(0x8469898cc51701b8), UINT64_C(0x4000921fb54442d1)};
constexpr float128_t qhalfpi{UINT64_C(0x8469898cc51701b8), UINT64_C(0x3fff921fb54442d1)};
constexpr float128_t qquarterpi{UINT64_C(0x8469898cc51701b8), UINT64_C(0x3ffe921fb54442d1)};
constexpr float128_t qln2{UINT64_C(0xf35793c7673007e6), UINT64_C(0x3ffe62e42fefa39e)};

float128_t Negate(float128_t value) {
    value.v[1] ^= UINT64_C(0x8000000000000000);
    return value;
}

float128_t WithSign(float128_t value, bool negative) {
    value.v[1] = (value.v[1] & UINT64_C(0x7fffffffffffffff)) | (u64{negative} << 63);
    return value;
}

float128_t Integer(softfloat_state& env, unsigned value) {
    return extF80_to_f128(&env, FromSigned(value, 32));
}

struct Reduced {
    float128_t argument;
    unsigned quadrant;
};

Reduced Reduce(softfloat_state& env, extFloat80_t input) {
    const bool negative = (input.signExp & 0x8000) != 0;
    const auto exponent = static_cast<int>(input.signExp & 0x7fff) - 0x3fff;
    if (exponent < -1) return {extF80_to_f128(&env, input), 0};
    // floor((2/pi)*2^256), sufficient to retain over 190 fractional bits
    // for every accepted x87 input |x|<2^63. Multiplication and reduction use
    // integer limbs, so large arguments never hit Cephes' 2^55 loss return.
    constexpr std::array<u64, 4> reciprocal{UINT64_C(0xfe5163abdebbc561),
                                            UINT64_C(0xdb6295993c439041),
                                            UINT64_C(0xfc2757d1f534ddc0),
                                            UINT64_C(0xa2f9836e4e441529)};
    std::array<u64, 5> product{};
    using Wide = unsigned __int128;
    Wide carry{};
    for (unsigned i = 0; i < 4; ++i) {
        const Wide word = Wide{input.signif} * reciprocal[i] + carry;
        product[i] = static_cast<u64>(word);
        carry = word >> 64;
    }
    product[4] = static_cast<u64>(carry);
    const unsigned shift = 319 - exponent;  // 257..320, before the binary point.
    const auto bit = [&](unsigned at) { return ((product[at / 64] >> (at & 63)) & 1) != 0; };
    const auto whole = shift == 320 ? u64{0} : product[4] >> (shift - 256);
    const bool round_up = bit(shift - 1);
    unsigned quadrant = static_cast<unsigned>((whole + round_up) & 3);
    if (shift != 320) product[4] &= (u64{1} << (shift - 256)) - 1;
    if (round_up) {
        u64 increment = 1;
        for (auto& word : product) {
            const auto next = ~word + increment;
            increment = increment && next == 0;
            word = next;
        }
        if (shift != 320) product[4] &= (u64{1} << (shift - 256)) - 1;
    }
    int highest = 319;
    while (highest >= 0 && !bit(static_cast<unsigned>(highest))) --highest;
    if (highest < 0) return {qzero, negative ? (4 - quadrant) & 3 : quadrant};
    Wide significand{};
    for (int i = 0; i < 113; ++i) {
        significand <<= 1;
        if (highest >= i && bit(static_cast<unsigned>(highest - i))) significand |= 1;
    }
    bool sticky{};
    for (int i = highest - 114; i >= 0; --i) sticky |= bit(static_cast<unsigned>(i));
    if (highest >= 113 && bit(static_cast<unsigned>(highest - 113)) &&
        (sticky || (significand & 1)))
        ++significand;
    int result_exponent = highest - static_cast<int>(shift) + 0x3fff;
    if (significand >> 113) {
        significand >>= 1;
        ++result_exponent;
    }
    float128_t fraction{static_cast<u64>(significand),
                        (static_cast<u64>(significand >> 64) & UINT64_C(0x0000ffffffffffff)) |
                                (u64{static_cast<unsigned>(result_exponent)} << 48)};
    fraction = WithSign(fraction, negative != round_up);
    if (negative) quadrant = (4 - quadrant) & 3;
    return {f128_mul(&env, fraction, qhalfpi), quadrant};
}

float128_t ExpMinusOne(softfloat_state& env, float128_t x) {
    // For |x|<=1 the direct series avoids cancellation even for the smallest
    // ext80 subnormal. exp2(x)-1 would otherwise round first to exact one.
    const auto t = f128_mul(&env, x, qln2);
    auto term = t, sum = t;
    for (unsigned n = 2; n <= 48; ++n) {
        term = f128_div(&env, f128_mul(&env, term, t), Integer(env, n));
        const auto next = f128_add(&env, sum, term);
        if (next.v[0] == sum.v[0] && next.v[1] == sum.v[1]) break;
        sum = next;
    }
    return sum;
}

float128_t LogPlusOne(softfloat_state& env, float128_t x, bool small) {
    if (!small) return cephes_f128_log2l(f128_add(&env, qone, x));
    auto power = x, sum = x;
    for (unsigned n = 2; n <= 80; ++n) {
        power = f128_mul(&env, power, x);
        const auto term = f128_div(&env, power, Integer(env, n));
        const auto next = (n & 1) ? f128_add(&env, sum, term) : f128_sub(&env, sum, term);
        if (next.v[0] == sum.v[0] && next.v[1] == sum.v[1]) break;
        sum = next;
    }
    return f128_div(&env, sum, qln2);
}

float128_t ArcTan(softfloat_state& env, extFloat80_t x, extFloat80_t y) {
    const bool sx = (x.signExp & 0x8000) != 0, sy = (y.signExp & 0x8000) != 0;
    if (IsZero(y)) return WithSign(sx ? qpi : qzero, sy);
    if (IsZero(x)) return WithSign(qhalfpi, sy);
    if (IsInfinity(y)) {
        auto angle = qhalfpi;
        if (IsInfinity(x)) angle = sx ? f128_add(&env, qhalfpi, qquarterpi) : qquarterpi;
        return WithSign(angle, sy);
    }
    if (IsInfinity(x)) return WithSign(sx ? qpi : qzero, sy);
    return cephes_f128_atan2l(extF80_to_f128(&env, y), extF80_to_f128(&env, x));
}
}  // namespace

X87Result ExecuteTranscendental(X87State& s, X87Transcendental operation) {
    const bool binary = operation == X87Transcendental::Atan ||
                        operation == X87Transcendental::YLog2X ||
                        operation == X87Transcendental::YLog2XPlusOne;
    const bool pushing =
            operation == X87Transcendental::SinCos || operation == X87Transcendental::Tan;
    const bool trig =
            operation == X87Transcendental::Sin || operation == X87Transcendental::Cos || pushing;
    s.status &= ~c1;
    if (Missing(s, 0) || (binary && Missing(s, 1))) {
        if (StackFault(s, false)) return X87Result::RaisedException;
        Write(s, binary ? 1 : 0, indefinite);
        if (binary) Pop(s);
        if (pushing) return Push(s, indefinite);
        return X87Result::Ok;
    }
    auto x = CanonicalOperand(Read(s, 0));
    auto y = binary ? CanonicalOperand(Read(s, 1)) : zero;
    // Out-of-range trig is the architectural no-result/no-push path.
    if (trig && !IsNaN(x) && !IsInfinity(x) && !UnsupportedValue(x) &&
        (x.signExp & 0x7fff) >= 0x403e) {
        s.status |= c2;
        return X87Result::Ok;
    }
    const bool push_overflow = pushing && Tag(s, (Top(s) - 1) & 7) != 3;
    if (push_overflow && StackFault(s, true)) return X87Result::RaisedException;
    if ((Denormal(x) || (binary && Denormal(y))) && Raise(s, denormal))
        return X87Result::RaisedException;
    if (trig) s.status &= ~c2;
    auto env = Environment(s);
    env.roundingPrecision = 80;
    auto work = env;
    work.roundingMode = softfloat_round_near_even;
    extFloat80_t result{}, extra = one;
    u16 exceptions{};
    const bool invalid_encoding = UnsupportedValue(x) || (binary && UnsupportedValue(y));
    if (invalid_encoding || (trig && IsInfinity(x))) {
        result = extra = indefinite;
        exceptions |= invalid;
    } else if (IsNaN(x) || (binary && IsNaN(y))) {
        result = extF80_add(&env, x, binary ? y : zero);
        extra = result;
    } else {
        auto qx = extF80_to_f128(&work, x);
        float128_t answer{};
        bool precision{};
        if (trig) {
            const auto reduced = Reduce(work, x);
            const auto q = reduced.quadrant;
            if (operation == X87Transcendental::Tan) {
                answer = cephes_f128_tanl(reduced.argument);
                if (q & 1) answer = Negate(f128_div(&work, qone, answer));
            } else {
                auto sine = cephes_f128_sinl(reduced.argument),
                     cosine = cephes_f128_cosl(reduced.argument);
                const auto actual_sine = q == 0   ? sine
                                         : q == 1 ? cosine
                                         : q == 2 ? Negate(sine)
                                                  : Negate(cosine);
                const auto actual_cosine = q == 0   ? cosine
                                           : q == 1 ? Negate(sine)
                                           : q == 2 ? Negate(cosine)
                                                    : sine;
                answer = operation == X87Transcendental::Cos ? actual_cosine : actual_sine;
                if (operation == X87Transcendental::SinCos)
                    extra = f128_to_extF80(&env, actual_cosine);
            }
            precision = !IsZero(x);
        } else if (operation == X87Transcendental::Atan) {
            answer = ArcTan(work, x, y);
            precision = !(IsZero(y) && !(x.signExp & 0x8000)) &&
                        !(IsInfinity(x) && !(x.signExp & 0x8000) && !IsInfinity(y));
        } else if (operation == X87Transcendental::TwoToXMinusOne) {
            if (IsInfinity(x))
                answer = (x.signExp & 0x8000) ? Negate(qone) : qx;
            else if ((x.signExp & 0x7fff) == 0x3fff && x.signif == integer_bit)
                answer = (x.signExp & 0x8000) ? float128_t{0, UINT64_C(0xbffe000000000000)} : qone;
            else if ((x.signExp & 0x7fff) < 0x3fff)
                answer = ExpMinusOne(work, qx);
            else
                answer = f128_sub(&work, cephes_f128_exp2l(qx), qone);  // Outside specified [-1,1].
            precision = !IsZero(x) && !((x.signExp & 0x7fff) == 0x3fff && x.signif == integer_bit);
        } else {
            const bool plus_one = operation == X87Transcendental::YLog2XPlusOne;
            const auto argument = plus_one ? f128_add(&work, qone, qx) : qx;
            const auto exponent = (argument.v[1] >> 48) & 0x7fff;
            const bool argument_zero =
                    !(argument.v[0] || (argument.v[1] & UINT64_C(0x7fffffffffffffff)));
            const bool argument_negative = (argument.v[1] >> 63) && !argument_zero;
            if (argument_negative || (argument_zero && IsZero(y))) {
                answer = extF80_to_f128(&work, indefinite);
                exceptions |= invalid;
            } else {
                float128_t logarithm{};
                if (argument_zero) {
                    logarithm = {0, UINT64_C(0xffff000000000000)};
                    exceptions |= 4;
                } else if (exponent == 0x7fff)
                    logarithm = argument;
                else if (plus_one)
                    logarithm = LogPlusOne(work, qx, (x.signExp & 0x7fff) < 0x3ffd);
                else
                    logarithm = cephes_f128_log2l(qx);
                answer = f128_mul(&work, extF80_to_f128(&work, y), logarithm);
                const bool power_of_two =
                        exponent ? exponent != 0x7fff && !argument.v[0] &&
                                           !(argument.v[1] & UINT64_C(0x0000ffffffffffff))
                                 : std::popcount(argument.v[0]) +
                                                   std::popcount(argument.v[1] &
                                                                 UINT64_C(0x0000ffffffffffff)) ==
                                           1;
                precision = !argument_zero &&
                            (!power_of_two ||
                             (plus_one && !IsZero(x) && (x.signExp & 0x7fff) < 0x3ffd)) &&
                            !IsZero(y) && !IsInfinity(y);
            }
        }
        result = f128_to_extF80(&env, answer);
        exceptions |= Exceptions(work.exceptionFlags &
                                 (softfloat_flag_invalid | softfloat_flag_infinite));
        if (IsInfinity(result) && (work.exceptionFlags & softfloat_flag_overflow))
            exceptions |= 8 | 32;
        if (IsZero(result) && (work.exceptionFlags & softfloat_flag_underflow))
            exceptions |= 16 | 32;
        if (precision) exceptions |= 32;
    }
    if (env.roundedUp) s.status |= c1;
    if (Raise(s, exceptions | Exceptions(env.exceptionFlags))) return X87Result::RaisedException;
    Write(s, binary ? 1 : 0, result);
    if (binary) Pop(s);
    if (pushing) {
        const auto rounding = s.status & c1;
        const auto pushed = Push(s, extra);
        if (pushed == X87Result::Ok && !push_overflow) s.status = (s.status & ~c1) | rounding;
        return pushed;
    }
    return X87Result::Ok;
}
}  // namespace swift::runtime::x86::detail
