#include "x87_detail.h"

namespace swift::runtime::x86::detail {
namespace {
struct Magnitude {
    u64 significand;
    int exponent;
};

Magnitude Align(extFloat80_t value) {
    auto exponent = static_cast<int>(value.signExp & 0x7fff);
    if (!exponent) exponent = 1;
    const auto shift = static_cast<int>(std::countl_zero(value.signif));
    return {value.signif << shift, exponent - shift};
}

struct Quotient {
    u64 low;
    bool rounded_up;
};

Quotient Divide(extFloat80_t a, extFloat80_t b, bool nearest) {
    const auto left = Align(a), right = Align(b);
    const auto difference = left.exponent - right.exponent;
    if (difference < -1) return {};
    using Wide = unsigned __int128;
    Wide remainder = left.significand;
    Wide denominator = right.significand;
    u64 quotient{};
    if (difference < 0)
        denominator <<= 1;
    else
        for (int bit = difference; bit >= 0; --bit) {
            if (remainder >= denominator) {
                remainder -= denominator;
                quotient |= u64{1} << bit;
            }
            if (bit) remainder <<= 1;
        }
    // Only shifts/subtracts are used: the helper has no hidden __udivti3
    // call with a different register-preservation contract.
    const auto twice = remainder << 1;
    const bool up = nearest && (twice > denominator || (twice == denominator && (quotient & 1)));
    return {quotient + up, up};
}

extFloat80_t ScaleEncoding(extFloat80_t value, int amount) {
    const auto magnitude = Align(value);
    const auto exponent = magnitude.exponent + amount;
    if (exponent > 0)
        return {magnitude.significand, static_cast<u16>((value.signExp & 0x8000) | exponent)};
    return {magnitude.significand >> (1 - exponent), static_cast<u16>(value.signExp & 0x8000)};
}

X87Result RequirePair(X87State& s) {
    s.status &= ~c1;
    if (Missing(s, 0) || Missing(s, 1)) {
        if (StackFault(s, false)) return X87Result::RaisedException;
        Write(s, 0, indefinite);
        return X87Result::Unsupported;
    }
    if (UnsupportedValue(Read(s, 0)) || UnsupportedValue(Read(s, 1))) {
        if (Raise(s, invalid)) return X87Result::RaisedException;
        Write(s, 0, indefinite);
        return X87Result::Unsupported;
    }
    if ((Denormal(Read(s, 0)) || Denormal(Read(s, 1))) && Raise(s, denormal))
        return X87Result::RaisedException;
    return X87Result::Ok;
}
}  // namespace

X87Result ExecuteRemainder(X87State& s, bool nearest) {
    const auto valid = RequirePair(s);
    if (valid != X87Result::Ok) return valid == X87Result::Unsupported ? X87Result::Ok : valid;
    const auto dividend = CanonicalOperand(Read(s, 0)), divisor = CanonicalOperand(Read(s, 1));
    const bool ordinary = !IsNaN(dividend) && !IsNaN(divisor) && !IsInfinity(dividend) &&
                          !IsInfinity(divisor) && !IsZero(dividend) && !IsZero(divisor);
    const auto difference = ordinary ? Align(dividend).exponent - Align(divisor).exponent : 0;
    const bool partial = ordinary && difference >= 64;
    const auto actual_divisor =
            partial ? ScaleEncoding(divisor, difference - (32 + (difference & 31))) : divisor;
    auto env = Environment(s);
    env.roundingPrecision = 80;
    auto result = extF80_rem(&env, dividend, actual_divisor);
    u64 quotient{};
    if (ordinary) {
        const auto selected = Divide(dividend, actual_divisor, nearest);
        quotient = selected.low;
        if (!nearest && Divide(dividend, actual_divisor, true).rounded_up) {
            auto adjustment = actual_divisor;
            adjustment.signExp = (adjustment.signExp & 0x7fff) | (dividend.signExp & 0x8000);
            result = extF80_add(&env, result, adjustment);
        }
    }
    if (Raise(s, Exceptions(env.exceptionFlags))) return X87Result::RaisedException;
    Write(s, 0, result);
    if (partial)
        s.status |= c2;
    else
        s.status = (s.status & ~conditions) | ((quotient & 4) ? c0 : 0) |
                   ((quotient & 2) ? c3 : 0) | ((quotient & 1) ? c1 : 0);
    return X87Result::Ok;
}

X87Result ExecuteScale(X87State& s) {
    const auto valid = RequirePair(s);
    if (valid != X87Result::Ok) return valid == X87Result::Unsupported ? X87Result::Ok : valid;
    auto value = CanonicalOperand(Read(s, 0));
    const auto scale = CanonicalOperand(Read(s, 1));
    auto env = Environment(s);
    env.roundingPrecision = 80;
    if (IsNaN(value) || IsNaN(scale))
        value = extF80_mul(&env, value, scale);
    else if (IsInfinity(scale)) {
        const extFloat80_t factor =
                (scale.signExp & 0x8000) ? zero : extFloat80_t{integer_bit, 0x7fff};
        value = extF80_mul(&env, value, factor);
    } else {
        auto conversion = env;
        auto amount = extF80_to_i64(&conversion, scale, softfloat_round_minMag, false);
        if (conversion.exceptionFlags & softfloat_flag_invalid)
            amount = (scale.signExp & 0x8000) ? -32768 : 32768;
        if (amount < -32768) amount = -32768;
        if (amount > 32768) amount = 32768;
        while (amount && !IsZero(value) && !IsInfinity(value) && !IsNaN(value)) {
            const auto chunk = static_cast<int>(amount < -16000  ? -16000
                                                : amount > 16000 ? 16000
                                                                 : amount);
            value = extF80_mul(
                    &env, value, extFloat80_t{integer_bit, static_cast<u16>(0x3fff + chunk)});
            amount -= chunk;
        }
    }
    if (env.roundedUp) s.status |= c1;
    if (Raise(s, Exceptions(env.exceptionFlags))) return X87Result::RaisedException;
    Write(s, 0, value);
    return X87Result::Ok;
}

X87Result ExecuteExtract(X87State& s) {
    s.status &= ~c1;
    const bool missing = Missing(s, 0);
    if (missing && StackFault(s, false)) return X87Result::RaisedException;
    const bool overflow = Tag(s, (Top(s) - 1) & 7) != 3;
    if (overflow && StackFault(s, true)) return X87Result::RaisedException;
    const auto value = missing ? indefinite : CanonicalOperand(Read(s, 0));
    auto significand = value, exponent = value;
    auto env = Environment(s);
    if (missing || UnsupportedValue(value)) {
        if (!missing && Raise(s, invalid)) return X87Result::RaisedException;
        significand = exponent = indefinite;
    } else if (IsNaN(value)) {
        significand = exponent = extF80_add(&env, value, zero);
    } else if (IsInfinity(value))
        exponent = {integer_bit, 0x7fff};
    else if (IsZero(value)) {
        if (Raise(s, 4)) return X87Result::RaisedException;
        exponent = {integer_bit, 0xffff};
    } else {
        if (Denormal(value) && Raise(s, denormal)) return X87Result::RaisedException;
        const auto adjusted = Align(value);
        significand = {adjusted.significand, static_cast<u16>((value.signExp & 0x8000) | 0x3fff)};
        exponent = FromSigned(static_cast<u64>(adjusted.exponent - 0x3fff), 64);
    }
    if (Raise(s, Exceptions(env.exceptionFlags))) return X87Result::RaisedException;
    Write(s, 0, exponent);
    return Push(s, significand);
}
}  // namespace swift::runtime::x86::detail
