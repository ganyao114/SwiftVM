#include "runtime/frontend/x86/x87.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

#include "runtime/frontend/x86/decoder.h"
#include "translator/x86/cpu.h"

extern "C" {
#include "softfloat.h"
}

namespace swift::x86 {

namespace {

constexpr u16 kSwIE = 1u << 0;
constexpr u16 kSwDE = 1u << 1;
constexpr u16 kSwZE = 1u << 2;
constexpr u16 kSwOE = 1u << 3;
constexpr u16 kSwUE = 1u << 4;
constexpr u16 kSwPE = 1u << 5;
constexpr u16 kSwSF = 1u << 6;
constexpr u16 kSwES = 1u << 7;
constexpr u16 kSwC0 = 1u << 8;
constexpr u16 kSwC1 = 1u << 9;
constexpr u16 kSwC2 = 1u << 10;
constexpr u16 kSwTopMask = 7u << 11;
constexpr u16 kSwC3 = 1u << 14;
constexpr u16 kSwB = 1u << 15;
constexpr u16 kSwConditionMask = kSwC0 | kSwC1 | kSwC2 | kSwC3;

constexpr u16 kTagValid = 0;
constexpr u16 kTagZero = 1;
constexpr u16 kTagSpecial = 2;
constexpr u16 kTagEmpty = 3;

constexpr extFloat80_t kIndefinite{
        .signif = UINT64_C(0xC000000000000000),
        .signExp = 0xFFFF,
};
constexpr extFloat80_t kZero{.signif = 0, .signExp = 0};
constexpr extFloat80_t kOne{
        .signif = UINT64_C(0x8000000000000000),
        .signExp = 0x3FFF,
};

struct DecodedCommand {
    X87Action action;
    X87Format format;
    u8 index;
    u8 operation;
    u32 flags;
};

DecodedCommand DecodeCommand(u64 command) {
    return {
            .action = static_cast<X87Action>(command & 0xFF),
            .format = static_cast<X87Format>((command >> 8) & 0xFF),
            .index = static_cast<u8>((command >> 16) & 7),
            .operation = static_cast<u8>((command >> 24) & 0xFF),
            .flags = static_cast<u32>(command >> 32),
    };
}

u8 Top(const ThreadContext64& ctx) {
    return static_cast<u8>((ctx.x87_fsw >> 11) & 7);
}

void SetTop(ThreadContext64& ctx, u8 top) {
    ctx.x87_fsw = static_cast<u16>((ctx.x87_fsw & ~kSwTopMask) | ((top & 7) << 11));
}

u8 Physical(const ThreadContext64& ctx, u8 logical) {
    return static_cast<u8>((Top(ctx) + logical) & 7);
}

u16 TagPhysical(const ThreadContext64& ctx, u8 physical) {
    return static_cast<u16>((ctx.x87_ftw >> (physical * 2)) & 3);
}

u16 Tag(const ThreadContext64& ctx, u8 logical) {
    return TagPhysical(ctx, Physical(ctx, logical));
}

void SetTagPhysical(ThreadContext64& ctx, u8 physical, u16 tag) {
    const u16 shift = static_cast<u16>(physical * 2);
    ctx.x87_ftw =
            static_cast<u16>((ctx.x87_ftw & ~(u16(3) << shift)) | ((tag & 3) << shift));
}

bool IsNaN(const extFloat80_t& value) {
    return (value.signExp & 0x7FFF) == 0x7FFF &&
           (value.signif & UINT64_C(0x7FFFFFFFFFFFFFFF)) != 0;
}

bool IsSignalingNaN(const extFloat80_t& value) {
    return IsNaN(value) && !(value.signif & UINT64_C(0x4000000000000000));
}

bool IsInfinity(const extFloat80_t& value) {
    return (value.signExp & 0x7FFF) == 0x7FFF &&
           value.signif == UINT64_C(0x8000000000000000);
}

bool IsDenormal(const extFloat80_t& value) {
    return (value.signExp & 0x7FFF) == 0 && value.signif != 0;
}

bool IsZero(const extFloat80_t& value) {
    return (value.signExp & 0x7FFF) == 0 && value.signif == 0;
}

struct NormalizedMagnitude {
    u64 significand;
    s32 exponent;
};

NormalizedMagnitude NormalizeMagnitude(const extFloat80_t& value) {
    u64 significand = value.signif;
    s32 exponent = value.signExp & 0x7FFF;
    if (!exponent) exponent = 1;
    if (significand && !(significand & UINT64_C(0x8000000000000000))) {
        const int shift = std::countl_zero(significand);
        significand <<= shift;
        exponent -= shift;
    }
    return {significand, exponent};
}

bool MagnitudeAtLeastTwoTo63(const extFloat80_t& value) {
    return !IsNaN(value) && !IsInfinity(value) &&
           (value.signExp & 0x7FFF) >= 0x403E;
}

u16 Classify(const extFloat80_t& value) {
    const u16 exponent = value.signExp & 0x7FFF;
    if (exponent == 0 && value.signif == 0) {
        return kTagZero;
    }
    if (exponent == 0 || exponent == 0x7FFF ||
        !(value.signif & UINT64_C(0x8000000000000000))) {
        return kTagSpecial;
    }
    return kTagValid;
}

extFloat80_t ReadPhysical(const ThreadContext64& ctx, u8 physical) {
    return {
            .signif = ctx.x87_regs[physical].significand,
            .signExp = ctx.x87_regs[physical].sign_exp,
    };
}

extFloat80_t Read(const ThreadContext64& ctx, u8 logical) {
    return ReadPhysical(ctx, Physical(ctx, logical));
}

void WritePhysical(ThreadContext64& ctx, u8 physical, const extFloat80_t& value) {
    ctx.x87_regs[physical].significand = value.signif;
    ctx.x87_regs[physical].sign_exp = value.signExp;
    SetTagPhysical(ctx, physical, Classify(value));
}

void Write(ThreadContext64& ctx, u8 logical, const extFloat80_t& value) {
    WritePhysical(ctx, Physical(ctx, logical), value);
}

void RecomputeSummary(ThreadContext64& ctx) {
    const u16 pending = static_cast<u16>((ctx.x87_fsw & 0x3F) & ~(ctx.x87_fcw & 0x3F));
    if (pending) {
        ctx.x87_fsw |= kSwES | kSwB;
    } else {
        ctx.x87_fsw &= static_cast<u16>(~(kSwES | kSwB));
    }
}

void Raise(ThreadContext64& ctx, u16 exceptions) {
    ctx.x87_fsw |= exceptions;
    RecomputeSummary(ctx);
}

void RaiseSoftFloat(ThreadContext64& ctx, u8 flags) {
    u16 exceptions = 0;
    if (flags & softfloat_flag_invalid) exceptions |= kSwIE;
    if (flags & softfloat_flag_infinite) exceptions |= kSwZE;
    if (flags & softfloat_flag_overflow) exceptions |= kSwOE;
    if (flags & softfloat_flag_underflow) exceptions |= kSwUE;
    if (flags & softfloat_flag_inexact) exceptions |= kSwPE;
    Raise(ctx, exceptions);
}

extFloat80_t QuietNaN(ThreadContext64& ctx, extFloat80_t value) {
    if (IsSignalingNaN(value)) Raise(ctx, kSwIE);
    value.signif |= UINT64_C(0xC000000000000000);
    return value;
}

softfloat_state StateFromControl(const ThreadContext64& ctx, bool force_extended = false) {
    softfloat_state state{};
    state.detectTininess = softfloat_tininess_afterRounding;
    state.roundingPrecision = 80;
    if (!force_extended) {
        switch ((ctx.x87_fcw >> 8) & 3) {
            case 0: state.roundingPrecision = 32; break;
            case 2: state.roundingPrecision = 64; break;
            case 3: state.roundingPrecision = 80; break;
            default: state.roundingPrecision = 80; break;
        }
    }
    switch ((ctx.x87_fcw >> 10) & 3) {
        case 0: state.roundingMode = softfloat_round_near_even; break;
        case 1: state.roundingMode = softfloat_round_min; break;
        case 2: state.roundingMode = softfloat_round_max; break;
        case 3: state.roundingMode = softfloat_round_minMag; break;
    }
    return state;
}

u8* GuestPointer(u64 address) {
    return reinterpret_cast<u8*>(address + GetGuestMemBias());
}

template <typename T>
T LoadGuest(u64 address) {
    T value{};
    std::memcpy(&value, GuestPointer(address), sizeof(value));
    return value;
}

template <typename T>
void StoreGuest(u64 address, T value) {
    std::memcpy(GuestPointer(address), &value, sizeof(value));
}

extFloat80_t LoadExt80(u64 address) {
    extFloat80_t value{};
    std::memcpy(&value.signif, GuestPointer(address), 8);
    std::memcpy(&value.signExp, GuestPointer(address) + 8, 2);
    return value;
}

void StoreExt80(u64 address, const extFloat80_t& value) {
    std::memcpy(GuestPointer(address), &value.signif, 8);
    std::memcpy(GuestPointer(address) + 8, &value.signExp, 2);
}

extFloat80_t Signed64ToExt80(s64 integer) {
    const bool negative = integer < 0;
    const u64 magnitude =
            negative ? (u64(-(integer + 1)) + 1) : static_cast<u64>(integer);
    auto value = ui64_to_extF80(magnitude);
    if (negative && magnitude) {
        value.signExp |= 0x8000;
    }
    return value;
}

extFloat80_t LoadMemoryValue(ThreadContext64& ctx, X87Format format, u64 address) {
    auto state = StateFromControl(ctx, true);
    switch (format) {
        case X87Format::Float32: {
            float32_t value{.v = LoadGuest<u32>(address)};
            return f32_to_extF80(&state, value);
        }
        case X87Format::Float64: {
            float64_t value{.v = LoadGuest<u64>(address)};
            return f64_to_extF80(&state, value);
        }
        case X87Format::Float80:
            return LoadExt80(address);
        case X87Format::Int16:
            return i32_to_extF80(static_cast<s16>(LoadGuest<u16>(address)));
        case X87Format::Int32:
            return i32_to_extF80(static_cast<s32>(LoadGuest<u32>(address)));
        case X87Format::Int64:
            return Signed64ToExt80(static_cast<s64>(LoadGuest<u64>(address)));
        default:
            return kIndefinite;
    }
}

void StackFault(ThreadContext64& ctx, bool push) {
    ctx.x87_fsw |= kSwIE | kSwSF;
    if (push) {
        ctx.x87_fsw |= kSwC1;
    } else {
        ctx.x87_fsw &= static_cast<u16>(~kSwC1);
    }
    RecomputeSummary(ctx);
}

void Push(ThreadContext64& ctx, const extFloat80_t& value) {
    const u8 top = static_cast<u8>((Top(ctx) - 1) & 7);
    SetTop(ctx, top);
    if (TagPhysical(ctx, top) != kTagEmpty) {
        StackFault(ctx, true);
        WritePhysical(ctx, top, kIndefinite);
        return;
    }
    WritePhysical(ctx, top, value);
    ctx.x87_fsw &= static_cast<u16>(~kSwC1);
}

void Pop(ThreadContext64& ctx) {
    const u8 top = Top(ctx);
    SetTagPhysical(ctx, top, kTagEmpty);
    SetTop(ctx, static_cast<u8>((top + 1) & 7));
    ctx.x87_fsw &= static_cast<u16>(~kSwC1);
}

bool Require(ThreadContext64& ctx, u8 logical) {
    if (Tag(ctx, logical) != kTagEmpty) {
        return true;
    }
    StackFault(ctx, false);
    return false;
}

void Reset(ThreadContext64& ctx) {
    ctx.x87_fcw = 0x037F;
    ctx.x87_fsw = 0;
    ctx.x87_ftw = 0xFFFF;
    ctx.x87_fop = 0;
    ctx.x87_fip = 0;
    ctx.x87_fdp = 0;
}

void StoreFloat(ThreadContext64& ctx, X87Format format, u64 address) {
    if (!Require(ctx, 0)) {
        switch (format) {
            case X87Format::Float32: StoreGuest<u32>(address, 0xFFC00000u); break;
            case X87Format::Float64:
                StoreGuest<u64>(address, UINT64_C(0xFFF8000000000000));
                break;
            case X87Format::Float80: StoreExt80(address, kIndefinite); break;
            default: break;
        }
        return;
    }

    const auto value = Read(ctx, 0);
    auto state = StateFromControl(ctx);
    ctx.x87_fsw &= static_cast<u16>(~kSwC1);
    if (format == X87Format::Float32) {
        const auto out = extF80_to_f32(&state, value);
        StoreGuest<u32>(address, out.v);
    } else if (format == X87Format::Float64) {
        const auto out = extF80_to_f64(&state, value);
        StoreGuest<u64>(address, out.v);
    } else {
        StoreExt80(address, value);
    }
    RaiseSoftFloat(ctx, state.exceptionFlags);
}

void StoreInteger(ThreadContext64& ctx,
                  X87Format format,
                  u64 address,
                  bool truncate) {
    const auto indefinite = [&] {
        switch (format) {
            case X87Format::Int16: StoreGuest<u16>(address, 0x8000); break;
            case X87Format::Int32: StoreGuest<u32>(address, 0x80000000u); break;
            case X87Format::Int64:
                StoreGuest<u64>(address, UINT64_C(0x8000000000000000));
                break;
            default: break;
        }
    };
    if (!Require(ctx, 0)) {
        indefinite();
        return;
    }

    const auto value = Read(ctx, 0);
    auto state = StateFromControl(ctx);
    const u8 rounding = truncate ? softfloat_round_minMag : state.roundingMode;
    ctx.x87_fsw &= static_cast<u16>(~kSwC1);

    // SoftFloat's integer conversion entry points are intended to return their
    // configured NaN sentinel, but x87 must not let any NaN payload reach the
    // finite significand-shift path.  All FIST/FISTP/FISTTP widths use the
    // architectural integer-indefinite result and raise invalid.
    if (IsNaN(value)) {
        indefinite();
        Raise(ctx, kSwIE);
        return;
    }

    if (format == X87Format::Int64) {
        const s64 out = extF80_to_i64(&state, value, rounding, true);
        if (state.exceptionFlags & softfloat_flag_invalid) {
            indefinite();
        } else {
            StoreGuest<u64>(address, static_cast<u64>(out));
        }
    } else {
        const s32 out = extF80_to_i32(&state, value, rounding, true);
        if (state.exceptionFlags & softfloat_flag_invalid) {
            indefinite();
        } else if (format == X87Format::Int16 &&
                   (out < std::numeric_limits<s16>::min() ||
                    out > std::numeric_limits<s16>::max())) {
            state.exceptionFlags |= softfloat_flag_invalid;
            StoreGuest<u16>(address, 0x8000);
        } else if (format == X87Format::Int16) {
            StoreGuest<u16>(address, static_cast<u16>(out));
        } else {
            StoreGuest<u32>(address, static_cast<u32>(out));
        }
    }
    RaiseSoftFloat(ctx, state.exceptionFlags);
}

extFloat80_t ApplyBinary(softfloat_state& state,
                         X87Binary operation,
                         extFloat80_t left,
                         extFloat80_t right) {
    switch (operation) {
        case X87Binary::Add: return extF80_add(&state, left, right);
        case X87Binary::Mul: return extF80_mul(&state, left, right);
        case X87Binary::Sub: return extF80_sub(&state, left, right);
        case X87Binary::Div: return extF80_div(&state, left, right);
    }
    return kIndefinite;
}

struct QuotientInfo {
    u64 magnitude;
    bool rounded_up;
};

QuotientInfo CompleteQuotient(extFloat80_t dividend,
                              extFloat80_t divisor,
                              bool nearest) {
    const auto a = NormalizeMagnitude(dividend);
    const auto b = NormalizeMagnitude(divisor);
    const s32 difference = a.exponent - b.exponent;
    if (difference < -1) return {0, false};
    __uint128_t numerator = a.significand;
    __uint128_t denominator = b.significand;
    if (difference >= 0) {
        numerator <<= difference;
    } else {
        denominator <<= -difference;
    }
    const __uint128_t floor = numerator / denominator;
    const __uint128_t remainder = numerator % denominator;
    bool rounded_up = false;
    __uint128_t quotient = floor;
    if (nearest) {
        const __uint128_t twice_remainder = remainder << 1;
        rounded_up = twice_remainder > denominator ||
                     (twice_remainder == denominator && (floor & 1));
        if (rounded_up) ++quotient;
    }
    return {static_cast<u64>(quotient), rounded_up};
}

extFloat80_t CompleteRemainder(ThreadContext64& ctx,
                               extFloat80_t dividend,
                               extFloat80_t divisor,
                               bool nearest,
                               u64* quotient_bits) {
    const auto quotient = CompleteQuotient(dividend, divisor, nearest);
    const auto nearest_quotient =
            nearest ? quotient : CompleteQuotient(dividend, divisor, true);
    if (quotient_bits) *quotient_bits = quotient.magnitude & 7;

    auto state = StateFromControl(ctx, true);
    auto result = extF80_rem(&state, dividend, divisor);
    if (!nearest && nearest_quotient.rounded_up) {
        auto adjustment = divisor;
        adjustment.signExp =
                static_cast<u16>((adjustment.signExp & 0x7FFF) |
                                 (dividend.signExp & 0x8000));
        result = extF80_add(&state, result, adjustment);
    }
    RaiseSoftFloat(ctx, state.exceptionFlags);
    return result;
}

extFloat80_t ScaleEncodingForPartial(extFloat80_t value, s32 scale) {
    const auto normalized = NormalizeMagnitude(value);
    const s32 exponent = normalized.exponent + scale;
    return {
            .signif = normalized.significand,
            .signExp = static_cast<u16>((value.signExp & 0x8000) | exponent),
    };
}

void SetRemainderQuotientFlags(ThreadContext64& ctx, u64 quotient) {
    ctx.x87_fsw &= static_cast<u16>(~kSwConditionMask);
    if (quotient & 4) ctx.x87_fsw |= kSwC0;  // Q2
    if (quotient & 2) ctx.x87_fsw |= kSwC3;  // Q1
    if (quotient & 1) ctx.x87_fsw |= kSwC1;  // Q0
}

void Remainder(ThreadContext64& ctx, X87Remainder operation) {
    if (!Require(ctx, 0) || !Require(ctx, 1)) {
        Write(ctx, 0, kIndefinite);
        return;
    }

    const auto dividend = Read(ctx, 0);
    const auto divisor = Read(ctx, 1);
    if (IsDenormal(dividend) || IsDenormal(divisor)) Raise(ctx, kSwDE);
    const bool nearest = operation == X87Remainder::Nearest;

    // Let SoftFloat provide the architectural NaN/Inf/zero-divisor result.
    // Quotient flags are all zero for these completed exceptional cases.
    if (IsNaN(dividend) || IsNaN(divisor) || IsInfinity(dividend) ||
        IsZero(divisor) || IsInfinity(divisor) || IsZero(dividend)) {
        auto state = StateFromControl(ctx, true);
        const auto result = extF80_rem(&state, dividend, divisor);
        Write(ctx, 0, result);
        SetRemainderQuotientFlags(ctx, 0);
        RaiseSoftFloat(ctx, state.exceptionFlags);
        return;
    }

    const s32 difference = NormalizeMagnitude(dividend).exponent -
                           NormalizeMagnitude(divisor).exponent;
    if (difference >= 64) {
        // Intel permits an implementation-selected reduction width N in
        // [32,63].  Contemporary x87 uses 32-bit exponent windows for both
        // instructions: N = 32 + (D mod 32).  A Rosetta real-x86 probe of
        // 2^100 mod 3 yields 2^64 with C2 set on the first FPREM and FPREM1.
        // C0/C1/C3 are undefined while C2 is set.
        const s32 width = 32 + (difference & 31);
        const auto scaled_divisor =
                ScaleEncodingForPartial(divisor, difference - width);
        const auto result =
                CompleteRemainder(ctx, dividend, scaled_divisor, nearest, nullptr);
        Write(ctx, 0, result);
        ctx.x87_fsw |= kSwC2;
        return;
    }

    u64 quotient = 0;
    const auto result =
            CompleteRemainder(ctx, dividend, divisor, nearest, &quotient);
    Write(ctx, 0, result);
    SetRemainderQuotientFlags(ctx, quotient);
}

extFloat80_t PowerOfTwo(s32 exponent) {
    return {
            .signif = UINT64_C(0x8000000000000000),
            .signExp = static_cast<u16>(0x3FFF + exponent),
    };
}

void Scale(ThreadContext64& ctx) {
    if (!Require(ctx, 0) || !Require(ctx, 1)) {
        Write(ctx, 0, kIndefinite);
        return;
    }
    auto value = Read(ctx, 0);
    const auto scale = Read(ctx, 1);
    if (IsDenormal(value) || IsDenormal(scale)) Raise(ctx, kSwDE);
    ctx.x87_fsw &= static_cast<u16>(~kSwC1);

    auto state = StateFromControl(ctx);
    if (IsNaN(value) || IsNaN(scale)) {
        value = extF80_mul(&state, value, scale);
    } else if (IsInfinity(scale)) {
        const extFloat80_t factor =
                (scale.signExp & 0x8000)
                        ? kZero
                        : extFloat80_t{.signif = UINT64_C(0x8000000000000000),
                                       .signExp = 0x7FFF};
        value = extF80_mul(&state, value, factor);
    } else {
        auto conversion = StateFromControl(ctx, true);
        s64 amount = extF80_to_i64(
                &conversion, scale, softfloat_round_minMag, false);
        if (conversion.exceptionFlags & softfloat_flag_invalid) {
            amount = (scale.signExp & 0x8000) ? -32768 : 32768;
        }
        amount = std::clamp<s64>(amount, -32768, 32768);
        while (amount && !IsZero(value) && !IsInfinity(value) && !IsNaN(value)) {
            const s32 chunk =
                    static_cast<s32>(std::clamp<s64>(amount, -16000, 16000));
            value = extF80_mul(&state, value, PowerOfTwo(chunk));
            amount -= chunk;
        }
    }
    Write(ctx, 0, value);
    RaiseSoftFloat(ctx, state.exceptionFlags);
}

void Extract(ThreadContext64& ctx) {
    if (!Require(ctx, 0)) {
        Write(ctx, 0, kIndefinite);
        Push(ctx, kIndefinite);
        return;
    }
    const auto value = Read(ctx, 0);
    extFloat80_t significand{};
    extFloat80_t exponent{};
    if (IsNaN(value)) {
        significand = exponent = QuietNaN(ctx, value);
    } else if (IsInfinity(value)) {
        significand = value;
        exponent = {.signif = UINT64_C(0x8000000000000000), .signExp = 0x7FFF};
    } else if (IsZero(value)) {
        significand = value;
        exponent = {.signif = UINT64_C(0x8000000000000000), .signExp = 0xFFFF};
        Raise(ctx, kSwZE);
    } else {
        const auto normalized = NormalizeMagnitude(value);
        significand = {
                .signif = normalized.significand,
                .signExp = static_cast<u16>((value.signExp & 0x8000) | 0x3FFF),
        };
        exponent = i32_to_extF80(normalized.exponent - 0x3FFF);
        if (IsDenormal(value)) Raise(ctx, kSwDE);
    }

    // FXTRACT first replaces the old ST0 with the exponent, then pushes the
    // significand.  Final ST0 is significand and final ST1 is exponent.
    Write(ctx, 0, exponent);
    Push(ctx, significand);
}

double ToHostDouble(ThreadContext64& ctx, extFloat80_t value) {
    auto state = StateFromControl(ctx, true);
    const auto converted = extF80_to_f64(&state, value);
    RaiseSoftFloat(ctx, state.exceptionFlags);
    return std::bit_cast<double>(static_cast<u64>(converted.v));
}

extFloat80_t FromHostDouble(ThreadContext64& ctx, double value) {
    auto state = StateFromControl(ctx, true);
    const float64_t converted{.v = std::bit_cast<u64>(value)};
    const auto result = f64_to_extF80(&state, converted);
    RaiseSoftFloat(ctx, state.exceptionFlags);
    return result;
}

extFloat80_t PropagateNaN(ThreadContext64& ctx,
                          extFloat80_t first,
                          extFloat80_t second) {
    auto state = StateFromControl(ctx, true);
    const auto result = extF80_add(&state, first, second);
    RaiseSoftFloat(ctx, state.exceptionFlags);
    return result;
}

bool IsTrigRangeLimited(X87Transcendental operation) {
    return operation == X87Transcendental::Sin ||
           operation == X87Transcendental::Cos ||
           operation == X87Transcendental::SinCos ||
           operation == X87Transcendental::Tan;
}

bool IsPushTranscendental(X87Transcendental operation) {
    return operation == X87Transcendental::SinCos ||
           operation == X87Transcendental::Tan;
}

void Transcendental(ThreadContext64& ctx, X87Transcendental operation) {
    const bool binary = operation == X87Transcendental::Atan ||
                        operation == X87Transcendental::YLog2X ||
                        operation == X87Transcendental::YLog2XPlusOne;
    if (binary) {
        if (!Require(ctx, 0) || !Require(ctx, 1)) {
            Write(ctx, 1, kIndefinite);
            Pop(ctx);
            return;
        }
        const auto x = Read(ctx, 0);
        const auto y = Read(ctx, 1);
        if (IsDenormal(x) || IsDenormal(y)) Raise(ctx, kSwDE);

        extFloat80_t result{};
        if (IsNaN(y) || IsNaN(x)) {
            result = PropagateNaN(ctx, y, x);
        } else {
            const double host_x = ToHostDouble(ctx, x);
            const double host_y = ToHostDouble(ctx, y);
            double host_result = 0;
            switch (operation) {
                case X87Transcendental::Atan:
                    host_result = std::atan2(host_y, host_x);
                    break;
                case X87Transcendental::YLog2X:
                    host_result =
                            host_y *
                            (std::log(host_x) /
                             std::bit_cast<double>(UINT64_C(0x3FE62E42FEFA39EF)));
                    if (IsZero(x) && !IsZero(y)) Raise(ctx, kSwZE);
                    break;
                case X87Transcendental::YLog2XPlusOne:
                    host_result =
                            host_y *
                            (std::log(host_x + 1.0) /
                             std::bit_cast<double>(UINT64_C(0x3FE62E42FEFA39EF)));
                    if ((x.signExp & 0x8000) && host_x == -1.0 && !IsZero(y)) {
                        Raise(ctx, kSwZE);
                    }
                    break;
                default: break;
            }
            if (std::isnan(host_result)) {
                result = kIndefinite;
                Raise(ctx, kSwIE);
            } else {
                result = FromHostDouble(ctx, host_result);
                if (std::isfinite(host_result) && host_result != 0.0) {
                    Raise(ctx, kSwPE);
                }
            }
        }
        Write(ctx, 1, result);
        Pop(ctx);
        return;
    }

    if (!Require(ctx, 0)) {
        Write(ctx, 0, kIndefinite);
        if (IsPushTranscendental(operation)) Push(ctx, kIndefinite);
        return;
    }
    const auto input = Read(ctx, 0);
    if (IsDenormal(input)) Raise(ctx, kSwDE);
    ctx.x87_fsw &= static_cast<u16>(~kSwC2);

    if (IsNaN(input)) {
        const auto result = QuietNaN(ctx, input);
        Write(ctx, 0, result);
        if (IsPushTranscendental(operation)) Push(ctx, result);
        return;
    }
    if (IsInfinity(input) && IsTrigRangeLimited(operation)) {
        Write(ctx, 0, kIndefinite);
        Raise(ctx, kSwIE);
        if (IsPushTranscendental(operation)) Push(ctx, kIndefinite);
        return;
    }
    if (IsTrigRangeLimited(operation) && MagnitudeAtLeastTwoTo63(input)) {
        // C2=1 means argument reduction was not performed.  The source and
        // stack depth are both unchanged, including for the nominal push ops.
        // The boundary is inclusive on real x87.  QEMU's helper incorrectly
        // accepts exact +/-2^63 after narrowing to f64; the differential fuzz
        // excludes that oracle deviation while directed tests pin this path.
        ctx.x87_fsw |= kSwC2;
        return;
    }

    const double host_input = ToHostDouble(ctx, input);
    if (operation == X87Transcendental::SinCos) {
        double sine = 0;
        double cosine = 0;
#ifdef __APPLE__
        __sincos(host_input, &sine, &cosine);
#else
        ::sincos(host_input, &sine, &cosine);
#endif
        const auto sine80 = FromHostDouble(ctx, sine);
        const auto cosine80 = FromHostDouble(ctx, cosine);
        Write(ctx, 0, sine80);
        Push(ctx, cosine80);
        if (host_input != 0.0) Raise(ctx, kSwPE);
        return;
    }
    if (operation == X87Transcendental::Tan) {
        const double tangent = std::tan(host_input);
        if (std::isnan(tangent)) {
            Write(ctx, 0, kIndefinite);
            Raise(ctx, kSwIE);
            Push(ctx, kIndefinite);
        } else {
            Write(ctx, 0, FromHostDouble(ctx, tangent));
            Push(ctx, kOne);
            if (host_input != 0.0) Raise(ctx, kSwPE);
        }
        return;
    }

    double host_result = 0;
    switch (operation) {
        case X87Transcendental::Sin: host_result = std::sin(host_input); break;
        case X87Transcendental::Cos: host_result = std::cos(host_input); break;
        case X87Transcendental::TwoToXMinusOne:
            host_result = std::exp2(host_input) - 1.0;
            break;
        default: break;
    }
    if (std::isnan(host_result)) {
        Write(ctx, 0, kIndefinite);
        Raise(ctx, kSwIE);
    } else {
        Write(ctx, 0, FromHostDouble(ctx, host_result));
        if (host_input != 0.0 && std::isfinite(host_input)) Raise(ctx, kSwPE);
    }
}

void Binary(ThreadContext64& ctx, const DecodedCommand& command, u64 address) {
    const bool memory = command.format != X87Format::Register;
    const u8 dest = (command.flags & X87DestIndex) ? command.index : 0;
    const u8 source_index = command.index;
    if (!Require(ctx, dest) || (!memory && !Require(ctx, source_index))) {
        Write(ctx, dest, kIndefinite);
        if (command.flags & X87Pop) Pop(ctx);
        return;
    }

    extFloat80_t left = Read(ctx, dest);
    extFloat80_t right =
            memory ? LoadMemoryValue(ctx, command.format, address) : Read(ctx, source_index);
    if ((command.flags & X87DestIndex) && !memory) {
        right = Read(ctx, 0);
    }
    if (command.flags & X87Reverse) {
        std::swap(left, right);
    }
    if (IsDenormal(left) || IsDenormal(right)) {
        Raise(ctx, kSwDE);
    }
    auto state = StateFromControl(ctx);
    const auto result =
            ApplyBinary(state, static_cast<X87Binary>(command.operation), left, right);
    Write(ctx, dest, result);
    ctx.x87_fsw &= static_cast<u16>(~kSwC1);
    RaiseSoftFloat(ctx, state.exceptionFlags);
    if (command.flags & X87Pop) Pop(ctx);
}

void SetCompareStatus(ThreadContext64& ctx, bool less, bool equal, bool unordered) {
    ctx.x87_fsw &= static_cast<u16>(~kSwConditionMask);
    if (unordered) {
        ctx.x87_fsw |= kSwC0 | kSwC2 | kSwC3;
    } else if (less) {
        ctx.x87_fsw |= kSwC0;
    } else if (equal) {
        ctx.x87_fsw |= kSwC3;
    }
}

u64 Compare(ThreadContext64& ctx, const DecodedCommand& command, u64 address) {
    const bool memory = command.format != X87Format::Register;
    bool unordered = false;
    bool less = false;
    bool equal = false;
    if (!Require(ctx, 0) || (!memory && !Require(ctx, command.index))) {
        unordered = true;
        StackFault(ctx, false);
    } else {
        const auto left = Read(ctx, 0);
        const auto right =
                memory ? LoadMemoryValue(ctx, command.format, address) : Read(ctx, command.index);
        unordered = IsNaN(left) || IsNaN(right);
        if (unordered) {
            if (!(command.flags & X87Unordered) || IsSignalingNaN(left) ||
                IsSignalingNaN(right)) {
                Raise(ctx, kSwIE);
            }
        } else {
            auto state = StateFromControl(ctx, true);
            equal = extF80_eq(&state, left, right);
            less = !equal && extF80_lt(&state, left, right);
            if (IsDenormal(left) || IsDenormal(right)) Raise(ctx, kSwDE);
            RaiseSoftFloat(ctx, state.exceptionFlags);
        }
    }

    if (!(command.flags & X87ToEFlags)) {
        SetCompareStatus(ctx, less, equal, unordered);
    }
    if (command.flags & X87PopTwice) {
        Pop(ctx);
        Pop(ctx);
    } else if (command.flags & X87Pop) {
        Pop(ctx);
    }

    // Same compact encoding as VecFCmp: CF/PF/ZF in bits 0/1/2.
    return unordered ? 0b111 : less ? 0b001 : equal ? 0b100 : 0;
}

void Unary(ThreadContext64& ctx, X87Unary operation) {
    if (operation == X87Unary::Examine) {
        ctx.x87_fsw &= static_cast<u16>(~kSwConditionMask);
        const bool empty = Tag(ctx, 0) == kTagEmpty;
        const auto value = empty ? kZero : Read(ctx, 0);
        if (value.signExp & 0x8000) ctx.x87_fsw |= kSwC1;
        if (empty) {
            ctx.x87_fsw |= kSwC3 | kSwC0;
        } else if (IsNaN(value)) {
            ctx.x87_fsw |= kSwC0;
        } else if (IsInfinity(value)) {
            ctx.x87_fsw |= kSwC2 | kSwC0;
        } else if (IsDenormal(value)) {
            ctx.x87_fsw |= kSwC3 | kSwC2;
        } else if ((value.signExp & 0x7FFF) == 0 && value.signif == 0) {
            ctx.x87_fsw |= kSwC3;
        } else {
            ctx.x87_fsw |= kSwC2;
        }
        return;
    }
    if (operation == X87Unary::Test) {
        DecodedCommand compare{
                .action = X87Action::Compare,
                .format = X87Format::Register,
                .index = 0,
                .operation = 0,
                .flags = 0,
        };
        if (!Require(ctx, 0)) {
            SetCompareStatus(ctx, false, false, true);
            return;
        }
        const auto saved = Read(ctx, 0);
        Write(ctx, 0, saved);
        const auto right_slot = Physical(ctx, 7);
        const auto old = ReadPhysical(ctx, right_slot);
        const auto old_tag = TagPhysical(ctx, right_slot);
        WritePhysical(ctx, right_slot, kZero);
        compare.index = 7;
        Compare(ctx, compare, 0);
        ctx.x87_regs[right_slot].significand = old.signif;
        ctx.x87_regs[right_slot].sign_exp = old.signExp;
        SetTagPhysical(ctx, right_slot, old_tag);
        return;
    }
    if (!Require(ctx, 0)) {
        Write(ctx, 0, kIndefinite);
        return;
    }

    auto value = Read(ctx, 0);
    ctx.x87_fsw &= static_cast<u16>(~kSwC1);
    switch (operation) {
        case X87Unary::ChangeSign:
            value.signExp ^= 0x8000;
            break;
        case X87Unary::Abs:
            value.signExp &= 0x7FFF;
            break;
        case X87Unary::Sqrt: {
            auto state = StateFromControl(ctx);
            value = extF80_sqrt(&state, value);
            RaiseSoftFloat(ctx, state.exceptionFlags);
            break;
        }
        case X87Unary::Round: {
            auto state = StateFromControl(ctx, true);
            value = extF80_roundToInt(&state, value, state.roundingMode, true);
            RaiseSoftFloat(ctx, state.exceptionFlags);
            break;
        }
        default:
            break;
    }
    Write(ctx, 0, value);
}

extFloat80_t Constant(X87Constant constant) {
    switch (constant) {
        case X87Constant::One:
            return {.signif = UINT64_C(0x8000000000000000), .signExp = 0x3FFF};
        case X87Constant::Log2Ten:
            return {.signif = UINT64_C(0xD49A784BCD1B8AFE), .signExp = 0x4000};
        case X87Constant::Log2E:
            return {.signif = UINT64_C(0xB8AA3B295C17F0BC), .signExp = 0x3FFF};
        case X87Constant::Pi:
            return {.signif = UINT64_C(0xC90FDAA22168C235), .signExp = 0x4000};
        case X87Constant::Log10Two:
            return {.signif = UINT64_C(0x9A209A84FBCFF799), .signExp = 0x3FFD};
        case X87Constant::LnTwo:
            return {.signif = UINT64_C(0xB17217F7D1CF79AC), .signExp = 0x3FFE};
        case X87Constant::Zero:
            return kZero;
    }
    return kIndefinite;
}

u8 AbridgedTag(const ThreadContext64& ctx) {
    u8 abridged = 0;
    for (u8 physical = 0; physical < 8; ++physical) {
        if (TagPhysical(ctx, physical) != kTagEmpty) {
            abridged |= static_cast<u8>(1u << physical);
        }
    }
    return abridged;
}

void StoreEnvironment(ThreadContext64& ctx, u64 address) {
    auto* out = GuestPointer(address);
    const u32 fcw = ctx.x87_fcw;
    const u32 fsw = ctx.x87_fsw;
    const u32 ftw = ctx.x87_ftw;
    const u32 fip = static_cast<u32>(ctx.x87_fip);
    const u32 fcs_fop = static_cast<u32>(ctx.x87_fop) << 16;
    const u32 fdp = static_cast<u32>(ctx.x87_fdp);
    const u32 fds = 0;
    std::memcpy(out + 0, &fcw, 4);
    std::memcpy(out + 4, &fsw, 4);
    std::memcpy(out + 8, &ftw, 4);
    std::memcpy(out + 12, &fip, 4);
    std::memcpy(out + 16, &fcs_fop, 4);
    std::memcpy(out + 20, &fdp, 4);
    std::memcpy(out + 24, &fds, 4);
    // FNSTENV masks every exception after storing the original environment.
    ctx.x87_fcw |= 0x3F;
    RecomputeSummary(ctx);
}

void LoadEnvironment(ThreadContext64& ctx, u64 address) {
    const auto* in = GuestPointer(address);
    u32 fcw{}, fsw{}, ftw{}, fip{}, fcs_fop{}, fdp{};
    std::memcpy(&fcw, in + 0, 4);
    std::memcpy(&fsw, in + 4, 4);
    std::memcpy(&ftw, in + 8, 4);
    std::memcpy(&fip, in + 12, 4);
    std::memcpy(&fcs_fop, in + 16, 4);
    std::memcpy(&fdp, in + 20, 4);
    ctx.x87_fcw = static_cast<u16>(fcw);
    ctx.x87_fsw = static_cast<u16>(fsw);
    ctx.x87_ftw = static_cast<u16>(ftw);
    ctx.x87_fip = fip;
    ctx.x87_fop = static_cast<u16>(fcs_fop >> 16);
    ctx.x87_fdp = fdp;
    RecomputeSummary(ctx);
}

}  // namespace

u64 X87Dispatch(u64 context, u64 command_word, u64 guest_address) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context);
    const auto command = DecodeCommand(command_word);
    switch (command.action) {
        case X87Action::Init:
            Reset(ctx);
            break;
        case X87Action::ClearExceptions:
            ctx.x87_fsw &= static_cast<u16>(~0x80FFu);
            RecomputeSummary(ctx);
            break;
        case X87Action::LoadFloat:
        case X87Action::LoadInt:
            Push(ctx, LoadMemoryValue(ctx, command.format, guest_address));
            break;
        case X87Action::StoreFloat:
            StoreFloat(ctx, command.format, guest_address);
            if (command.flags & X87Pop) Pop(ctx);
            break;
        case X87Action::StoreInt:
            StoreInteger(ctx,
                         command.format,
                         guest_address,
                         (command.flags & X87Truncate) != 0);
            if (command.flags & X87Pop) Pop(ctx);
            break;
        case X87Action::LoadReg: {
            const auto value =
                    Require(ctx, command.index) ? Read(ctx, command.index) : kIndefinite;
            Push(ctx, value);
            break;
        }
        case X87Action::StoreReg:
            if (Require(ctx, 0)) {
                Write(ctx, command.index, Read(ctx, 0));
            } else {
                Write(ctx, command.index, kIndefinite);
            }
            if (command.flags & X87Pop) Pop(ctx);
            break;
        case X87Action::Binary:
            Binary(ctx, command, guest_address);
            break;
        case X87Action::Compare:
            return Compare(ctx, command, guest_address);
        case X87Action::Unary:
            Unary(ctx, static_cast<X87Unary>(command.operation));
            break;
        case X87Action::Remainder:
            Remainder(ctx, static_cast<X87Remainder>(command.operation));
            break;
        case X87Action::Scale:
            Scale(ctx);
            break;
        case X87Action::Extract:
            Extract(ctx);
            break;
        case X87Action::Transcendental:
            Transcendental(ctx,
                           static_cast<X87Transcendental>(command.operation));
            break;
        case X87Action::LoadConstant:
            Push(ctx, Constant(static_cast<X87Constant>(command.operation)));
            break;
        case X87Action::Exchange: {
            if (!Require(ctx, 0) || !Require(ctx, command.index)) {
                Write(ctx, 0, kIndefinite);
                Write(ctx, command.index, kIndefinite);
                break;
            }
            const auto left = Read(ctx, 0);
            const auto right = Read(ctx, command.index);
            Write(ctx, 0, right);
            Write(ctx, command.index, left);
            ctx.x87_fsw &= static_cast<u16>(~kSwC1);
            break;
        }
        case X87Action::Free:
            SetTagPhysical(ctx, Physical(ctx, command.index), kTagEmpty);
            if (command.flags & X87Pop) Pop(ctx);
            break;
        case X87Action::AdjustTop:
            SetTop(ctx,
                   static_cast<u8>((Top(ctx) +
                                    ((command.flags & X87IncrementTop) ? 1 : 7)) &
                                   7));
            ctx.x87_fsw &= static_cast<u16>(~kSwC1);
            break;
        case X87Action::StoreControl:
            StoreGuest<u16>(guest_address, ctx.x87_fcw);
            break;
        case X87Action::LoadControl:
            ctx.x87_fcw = LoadGuest<u16>(guest_address);
            RecomputeSummary(ctx);
            break;
        case X87Action::StoreStatus:
            if (command.format == X87Format::Register) {
                return ctx.x87_fsw;
            }
            StoreGuest<u16>(guest_address, ctx.x87_fsw);
            break;
        case X87Action::StoreEnvironment:
            StoreEnvironment(ctx, guest_address);
            break;
        case X87Action::LoadEnvironment:
            LoadEnvironment(ctx, guest_address);
            break;
    }
    return 0;
}

u64 X87Fxsave(u64 context, u64 guest_address) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context);
    auto* out = GuestPointer(guest_address);
    std::memset(out, 0, 512);
    std::memcpy(out + 0, &ctx.x87_fcw, 2);
    std::memcpy(out + 2, &ctx.x87_fsw, 2);
    const u8 ftw = AbridgedTag(ctx);
    std::memcpy(out + 4, &ftw, 1);
    std::memcpy(out + 6, &ctx.x87_fop, 2);
    std::memcpy(out + 8, &ctx.x87_fip, 8);
    std::memcpy(out + 16, &ctx.x87_fdp, 8);
    const u32 mask = 0x0000FFFF;
    std::memcpy(out + 28, &mask, 4);
    for (u8 logical = 0; logical < 8; ++logical) {
        const auto value = Read(ctx, logical);
        auto* slot = out + 32 + logical * 16;
        std::memcpy(slot, &value.signif, 8);
        std::memcpy(slot + 8, &value.signExp, 2);
    }
    return 0;
}

u64 X87Fxrstor(u64 context, u64 guest_address) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context);
    const auto* in = GuestPointer(guest_address);
    std::memcpy(&ctx.x87_fcw, in + 0, 2);
    std::memcpy(&ctx.x87_fsw, in + 2, 2);
    u8 abridged{};
    std::memcpy(&abridged, in + 4, 1);
    std::memcpy(&ctx.x87_fop, in + 6, 2);
    std::memcpy(&ctx.x87_fip, in + 8, 8);
    std::memcpy(&ctx.x87_fdp, in + 16, 8);
    ctx.x87_ftw = 0xFFFF;
    for (u8 logical = 0; logical < 8; ++logical) {
        const u8 physical = Physical(ctx, logical);
        if (!(abridged & (1u << physical))) {
            SetTagPhysical(ctx, physical, kTagEmpty);
            continue;
        }
        extFloat80_t value{};
        const auto* slot = in + 32 + logical * 16;
        std::memcpy(&value.signif, slot, 8);
        std::memcpy(&value.signExp, slot + 8, 2);
        WritePhysical(ctx, physical, value);
    }
    RecomputeSummary(ctx);
    return 0;
}

}  // namespace swift::x86
