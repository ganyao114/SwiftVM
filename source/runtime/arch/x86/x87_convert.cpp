#include "x87_detail.h"

namespace swift::runtime::x86::detail {
bool IsNaN(extFloat80_t v) noexcept {
    return (v.signExp & 0x7fff) == 0x7fff && (v.signif & ~integer_bit);
}

bool IsSignaling(extFloat80_t v) noexcept { return IsNaN(v) && !(v.signif & (integer_bit >> 1)); }

bool IsInfinity(extFloat80_t v) noexcept {
    return (v.signExp & 0x7fff) == 0x7fff && v.signif == integer_bit;
}

bool IsZero(extFloat80_t v) noexcept { return !(v.signExp & 0x7fff) && !v.signif; }

extFloat80_t CanonicalOperand(extFloat80_t v) noexcept {
    // Pseudo-denormals denote the same value as exponent one. Keeping this
    // normalization local preserves the actual 80-bit register payload.
    if (!(v.signExp & 0x7fff) && (v.signif & integer_bit)) v.signExp |= 1;
    return v;
}

bool MagnitudeGreater(extFloat80_t after, extFloat80_t before) noexcept {
    if (IsNaN(after) || IsNaN(before)) return false;
    after = CanonicalOperand(after);
    before = CanonicalOperand(before);
    const auto a = after.signExp & 0x7fff, b = before.signExp & 0x7fff;
    return a > b || (a == b && after.signif > before.signif);
}

extFloat80_t FromSigned(u64 raw, unsigned bits) noexcept {
    if (bits < 64) {
        const auto mask = (u64{1} << bits) - 1;
        raw &= mask;
        if (raw & (u64{1} << (bits - 1))) raw |= ~mask;
    }
    const bool negative = (raw >> 63) != 0;
    const auto magnitude = negative ? u64{0} - raw : raw;
    if (!magnitude) return zero;
    const auto shift = static_cast<unsigned>(std::countl_zero(magnitude));
    return {magnitude << shift, static_cast<u16>((negative ? 0x8000 : 0) | (0x403e - shift))};
}

extFloat80_t FromMemory(softfloat_state& env, X87Format format, u64 raw, u16& extra) noexcept {
    switch (format) {
        case X87Format::Float32:
            if (!(raw & UINT64_C(0x7f800000)) && (raw & UINT64_C(0x007fffff))) extra |= denormal;
            return f32_to_extF80(&env, float32_t{static_cast<u32>(raw)});
        case X87Format::Float64:
            if (!(raw & UINT64_C(0x7ff0000000000000)) && (raw & UINT64_C(0x000fffffffffffff)))
                extra |= denormal;
            return f64_to_extF80(&env, float64_t{raw});
        case X87Format::Int16:
            return FromSigned(raw, 16);
        case X87Format::Int32:
            return FromSigned(raw, 32);
        case X87Format::Int64:
            return FromSigned(raw, 64);
        default:
            return indefinite;
    }
}

X87Result PrepareStore(X87State& s, X87Command command, u64 truncate) {
    u16 exceptions{};
    auto value = Missing(s, 0) ? indefinite : Read(s, 0);
    if (Missing(s, 0)) exceptions = invalid | stack_fault;
    if (command == X87Command::PrepareExt80) {
        const auto status = static_cast<u16>((s.status & ~c1) | exceptions);
        if (status & ~s.control & 0x3f) {
            s.status = status;
            Pending(s);
            return X87Result::RaisedException;
        }
        s.result = value.signif;
        s.reserved = value.signExp;
        return X87Result::Ok;
    }

    const bool integer = command == X87Command::PrepareI16 || command == X87Command::PrepareI32 ||
                         command == X87Command::PrepareI64;
    if (UnsupportedValue(value) || (integer && IsNaN(value))) {
        exceptions |= invalid;
        value = indefinite;
    }
    auto env = Environment(s);
    bool rounded_up{};
    u64 result{};
    if (command == X87Command::PrepareF32) {
        const auto converted = extF80_to_f32(&env, CanonicalOperand(value));
        result = converted.v;
        softfloat_state exact{};
        rounded_up = MagnitudeGreater(f32_to_extF80(&exact, converted), value);
    } else if (command == X87Command::PrepareF64) {
        result = extF80_to_f64(&env, CanonicalOperand(value)).v;
        rounded_up = env.roundedUp;
    } else {
        const auto bits = command == X87Command::PrepareI16   ? 16u
                          : command == X87Command::PrepareI32 ? 32u
                                                              : 64u;
        const auto rounding = truncate ? softfloat_round_minMag : env.roundingMode;
        if (exceptions & invalid)
            result = u64{1} << (bits - 1);
        else {
            const auto converted =
                    bits == 64 ? extF80_to_i64(&env, CanonicalOperand(value), rounding, true)
                               : extF80_to_i32(&env, CanonicalOperand(value), rounding, true);
            const bool outside = bits == 16 && (converted < -32768 || converted > 32767);
            if ((env.exceptionFlags & softfloat_flag_invalid) || outside) {
                env.exceptionFlags = softfloat_flag_invalid;
                result = u64{1} << (bits - 1);
            } else {
                result = static_cast<u64>(converted);
                rounded_up = MagnitudeGreater(FromSigned(result, bits), value);
                if (bits < 64) result &= (u64{1} << bits) - 1;
            }
        }
    }
    exceptions |= Exceptions(env.exceptionFlags);
    const auto status = static_cast<u16>((s.status & ~c1) | exceptions | (rounded_up ? c1 : 0));
    if (status & ~s.control & 0x3f) {
        s.status = status;
        Pending(s);
        return X87Result::RaisedException;
    }
    // These two words are scratch until the complete guest memory write has
    // succeeded. An address fault therefore retains architectural FSW/TOP.
    s.result = result;
    s.reserved = status & ~summary;
    return X87Result::Ok;
}

X87Result PrepareBcd(X87State& s) {
    const bool empty = Missing(s, 0);
    const auto value = empty ? indefinite : Read(s, 0);
    u16 exceptions = empty ? invalid | stack_fault : 0;
    u64 magnitude{};
    bool rounded_up{};
    if (UnsupportedValue(value) || IsNaN(value) || IsInfinity(value)) exceptions |= invalid;
    if (!(exceptions & invalid)) {
        auto env = Environment(s);
        const auto integer = extF80_to_i64(&env, CanonicalOperand(value), env.roundingMode, true);
        const auto bits = static_cast<u64>(integer);
        magnitude = integer < 0 ? u64{0} - bits : bits;
        if ((env.exceptionFlags & softfloat_flag_invalid) ||
            magnitude > UINT64_C(999999999999999999))
            exceptions |= invalid;
        else {
            exceptions |= Exceptions(env.exceptionFlags);
            rounded_up = MagnitudeGreater(FromSigned(bits, 64), value);
        }
    }
    const auto status = static_cast<u16>((s.status & ~c1) | exceptions | (rounded_up ? c1 : 0));
    if (status & ~s.control & 0x3f) {
        s.status = status;
        Pending(s);
        return X87Result::RaisedException;
    }
    u64 low{};
    u16 high{};
    if (exceptions & invalid) {
        // Intel SDM Vol.1 Table4-4: FFFFC000000000000000H.
        low = UINT64_C(0xc000000000000000);
        high = 0x80ff;
    } else {
        for (unsigned digit = 0; digit < 16; ++digit) {
            low |= (magnitude % 10) << (digit * 4);
            magnitude /= 10;
        }
        high = static_cast<u16>((magnitude % 10) | ((magnitude / 10) << 4) |
                                (value.signExp & 0x8000));
    }
    s.result = low;
    s.reserved = high | ((exceptions & (invalid | denormal)) << 8) |
                 ((exceptions & (32 | stack_fault)) << 5) | (rounded_up ? 0x1000 : 0);
    return X87Result::Ok;
}

void CommitBcd(X87State& s) {
    const auto scratch = s.reserved;
    const u16 exceptions =
            ((scratch >> 8) & (invalid | denormal)) | ((scratch >> 5) & (32 | stack_fault));
    s.status = (s.status & ~c1) | exceptions | ((scratch & 0x1000) ? c1 : 0);
    Pending(s);
    Pop(s);
}
}  // namespace swift::runtime::x86::detail
