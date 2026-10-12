#include "fp_helpers.h"
#include "single_instruction_admission.h"

#include <cstring>
#include "cpu.h"
#include "runtime/backend/context.h"
#include "aarch64/constants-aarch64.h"
extern "C" {
#include "softfloat.h"
}

namespace swift::arm64 {
namespace {
using namespace vixl::aarch64;
constexpr u32 kSupportedFPCR = (1u << 19) | (3u << 22) | (1u << 24) | (1u << 25);

struct Format {
    u32 bits, fraction_bits;
    u64 sign, exponent, fraction, quiet;
    explicit Format(u32 width) : bits(width), fraction_bits(width == 16 ? 10 : width == 32 ? 23 : 52),
        sign(u64{1} << (width - 1)),
        exponent(width == 16 ? 0x7C00 : width == 32 ? 0x7F800000 : 0x7FF0000000000000),
        fraction((u64{1} << fraction_bits) - 1), quiet(u64{1} << (fraction_bits - 1)) {}
    bool NaN(u64 a) const { return (a & exponent) == exponent && (a & fraction); }
    bool Signaling(u64 a) const { return NaN(a) && !(a & quiet); }
    bool Subnormal(u64 a) const { return !(a & exponent) && (a & fraction); }
    u64 DefaultNaN() const { return exponent | quiet; }
};

u64 Read(const ThreadContext64& ctx, u32 reg, u32 bits) {
    u64 value;
    std::memcpy(&value, &ctx.v[reg], 8);
    return bits == 64 ? value : value & ((u64{1} << bits) - 1);
}
void Write(ThreadContext64& ctx, u32 reg, u64 value) {
    const u64 halves[] = {value, 0};
    std::memcpy(&ctx.v[reg], halves, 16);
}
u8 Rounding(u32 fpcr) {
    constexpr u8 modes[] = {softfloat_round_near_even, softfloat_round_max,
                           softfloat_round_min, softfloat_round_minMag};
    return modes[(fpcr >> 22) & 3];
}
softfloat_state State(u8 rounding) {
    return {softfloat_tininess_beforeRounding, rounding, 0, 80};
}
template<typename Environment>
void Exceptions(Environment& ctx, u8 flags) {
    if (flags & softfloat_flag_invalid) ctx.fpsr |= 1;
    if (flags & softfloat_flag_infinite) ctx.fpsr |= 2;
    if (flags & softfloat_flag_overflow) ctx.fpsr |= 4;
    if (flags & softfloat_flag_underflow) ctx.fpsr |= 8;
    if (flags & softfloat_flag_inexact) ctx.fpsr |= 16;
}
template<typename Environment>
u64 Input(Environment& ctx, const Format& f, u64 value, bool preserve_half_subnormal = false) {
    if (!(f.bits == 16 && preserve_half_subnormal) &&
        (ctx.fpcr & (1u << (f.bits == 16 ? 19 : 24))) && f.Subnormal(value)) {
        if (f.bits != 16) ctx.fpsr |= 128;
        return value & f.sign;
    }
    return value;
}
float128_t Widen(softfloat_state& s, const Format& f, u64 value) {
    if (f.bits == 16) {
        const u32 sign = (u32(value) & 0x8000) << 16;
        u32 exponent = (u32(value) >> 10) & 31;
        u32 fraction = u32(value) & 1023;
        if (!exponent && fraction) {
            exponent = 113;
            while (!(fraction & 1024)) { fraction <<= 1; --exponent; }
            fraction &= 1023;
        } else if (exponent) {
            exponent = exponent == 31 ? 255 : exponent + 112;
        }
        return f32_to_f128(&s, float32_t{sign | (exponent << 23) | (fraction << 13)});
    }
    if (f.bits == 32) return f32_to_f128(&s, float32_t{u32(value)});
    return extF80_to_f128(&s, f64_to_extF80(&s, float64_t{value}));
}
template<typename Environment>
u64 Narrow(Environment& ctx, const Format& f, float128_t value,
           softfloat_state& s, bool preserve_half_subnormal = false, u8 rounding_override = 255) {
    const u32 exponent = u32((value.v[1] >> 48) & 0x7FFF);
    const bool nonzero = value.v[0] || (value.v[1] & 0x7FFFFFFFFFFFFFFF);
    const u32 minimum_normal_exponent = 16383 - (f.bits == 16 ? 14 : f.bits == 32 ? 126 : 1022);
    if (!(f.bits == 16 && preserve_half_subnormal) &&
        (ctx.fpcr & (1u << (f.bits == 16 ? 19 : 24))) && nonzero && exponent < minimum_normal_exponent) {
        s.exceptionFlags = (s.exceptionFlags & ~softfloat_flag_inexact) | softfloat_flag_underflow;
        Exceptions(ctx, s.exceptionFlags);
        return (value.v[1] >> 63) ? f.sign : 0;
    }
    s.roundingMode = rounding_override == 255 ? Rounding(ctx.fpcr) : rounding_override;
    u64 bits = f.bits == 16 ? f128_to_f16(&s, value).v :
               f.bits == 32 ? f128_to_f32(&s, value).v : f128_to_f64(&s, value).v;
    if (f.NaN(bits)) bits = f.DefaultNaN();
    Exceptions(ctx, s.exceptionFlags);
    return bits;
}
template<typename Environment>
u64 NaN(Environment& ctx, const Format& f, u64 a, u64 b = 0, u64 c = 0) {
    const u64 inputs[] = {a, b, c};
    for (auto value : inputs) {
        if (f.Signaling(value)) {
            ctx.fpsr |= 1;
            return ctx.fpcr & (1u << 25) ? f.DefaultNaN() : value | f.quiet;
        }
    }
    if (ctx.fpcr & (1u << 25)) return f.DefaultNaN();
    for (auto value : inputs) if (f.NaN(value)) return value | f.quiet;
    return f.DefaultNaN();
}
bool TwoSource(u32 word) {
    return (word & FPDataProcessing2SourceFMask) == FPDataProcessing2SourceFixed;
}
bool ThreeSource(u32 word) {
    return (word & FPDataProcessing3SourceFMask) == FPDataProcessing3SourceFixed;
}
bool Compare(u32 word) {
    return (word & FPCompareFMask) == FPCompareFixed ||
           (word & FPConditionalCompareFMask) == FPConditionalCompareFixed;
}
bool IntegerConvert(u32 word) {
    return (word & FPIntegerConvertFMask) == FPIntegerConvertFixed;
}
bool FixedPointConvert(u32 word) {
    return (word & FPFixedPointConvertFMask) == FPFixedPointConvertFixed;
}
float128_t ScalePowerOfTwo(float128_t value, s32 amount) {
    const u32 exponent = u32((value.v[1] >> 48) & 0x7FFF);
    if (exponent && exponent != 0x7FFF) {
        // H/S/D inputs and 64-bit integers stay normal in binary128 for these scales.
        value.v[1] = (value.v[1] & ~u64{0x7FFF000000000000}) |
                     (u64(s32(exponent) + amount) << 48);
    }
    return value;
}
u64 ReadGPR(const ThreadContext64& ctx, u32 reg) {
    if (reg == 31) return 0;
    return reg < 29 ? ctx.r[reg] : reg == 29 ? ctx.fp : ctx.lr;
}
void WriteGPR(ThreadContext64& ctx, u32 reg, u64 value) {
    if (reg < 29) ctx.r[reg] = value;
    else if (reg == 29) ctx.fp = value;
    else if (reg == 30) ctx.lr = value;
}
}

bool IsSupportedFPCR(u32 fpcr) { return !(fpcr & ~kSupportedFPCR); }

FPBinaryResult EvaluateCompareFP(u64 lhs, u64 rhs, u32 bits, FPCompareOperation op, u32 fpcr) {
    struct Environment { u32 fpcr, fpsr; } env{fpcr, 0};
    const Format f(bits);
    u64 a = Input(env, f, lhs), b = Input(env, f, rhs);
    if (f.NaN(a) || f.NaN(b)) {
        if (op != FPCompareOperation::Eq || f.Signaling(a) || f.Signaling(b)) env.fpsr |= 1;
        return {0, env.fpsr};
    }
    if (op == FPCompareOperation::AbsGe || op == FPCompareOperation::AbsGt) {
        a &= ~f.sign;
        b &= ~f.sign;
    }
    auto s = State(softfloat_round_near_even);
    const auto x = Widen(s, f, a), y = Widen(s, f, b);
    bool match;
    switch (op) {
        case FPCompareOperation::Eq: match = f128_eq(&s, x, y); break;
        case FPCompareOperation::Ge:
        case FPCompareOperation::AbsGe: match = f128_le(&s, y, x); break;
        case FPCompareOperation::Gt:
        case FPCompareOperation::AbsGt: match = f128_lt(&s, y, x); break;
        case FPCompareOperation::Le: match = f128_le(&s, x, y); break;
        default: match = f128_lt(&s, x, y); break;
    }
    return {match ? (bits == 64 ? ~u64{0} : (u64{1} << bits) - 1) : 0, env.fpsr};
}

FPBinaryResult EvaluateBinaryFP(u64 lhs, u64 rhs, u32 bits, u32 op, u32 fpcr) {
    struct Environment { u32 fpcr, fpsr; } env{fpcr, 0};
    const Format f(bits);
    auto s = State(softfloat_round_odd);
    const u64 a = Input(env, f, lhs), b = Input(env, f, rhs);
    if (op >= 4 && op <= 7) {
        const bool numeric = op >= 6, minimum = op & 1;
        if (f.Signaling(a) || f.Signaling(b) ||
            ((f.NaN(a) || f.NaN(b)) && (!numeric || (f.NaN(a) && f.NaN(b))))) {
            const auto result = NaN(env, f, a, b);
            return {result, env.fpsr};
        }
        if (f.NaN(a)) return {b, env.fpsr};
        if (f.NaN(b)) return {a, env.fpsr};
        if ((a & ~f.sign) == 0 && (b & ~f.sign) == 0)
            return {minimum ? (a | b) : (a & b), env.fpsr};
        const bool less = f128_lt(&s, Widen(s, f, a), Widen(s, f, b));
        return {minimum == less ? a : b, env.fpsr};
    }
    if (f.NaN(a) || f.NaN(b)) {
        u64 result = NaN(env, f, a, b);
        if (op == 8) result ^= f.sign;
        return {result, env.fpsr};
    }
    const auto x = Widen(s, f, a), y = Widen(s, f, b);
    float128_t result;
    switch (op) {
        case 0: case 8: result = f128_mul(&s, x, y); break;
        case 1: result = f128_div(&s, x, y); break;
        case 2: result = f128_add(&s, x, y); break;
        default: result = f128_sub(&s, x, y); break;
    }
    if ((op == 2 || op == 3) && result.v[0] == 0 && (result.v[1] << 1) == 0) {
        const bool bsign = bool(b & f.sign) ^ (op == 3);
        if (bool(a & f.sign) != bsign)
            result.v[1] = Rounding(fpcr) == softfloat_round_min ? u64{1} << 63 : 0;
    }
    u64 value = Narrow(env, f, result, s);
    if (op == 8) value ^= f.sign;
    return {value, env.fpsr};
}

FPBinaryResult EvaluateIntegerFP(u64 value, u32 bits, u32 operation, u32 fpcr, u32 fractional_bits) {
    struct Environment { u32 fpcr, fpsr; } env{fpcr, 0};
    const Format f(bits);
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    value &= mask;
    const bool is_unsigned = operation & 1;
    auto s = State(softfloat_round_near_even);
    if (operation < 2) {
        const bool negative = !is_unsigned && (value & f.sign);
        const u64 magnitude = negative ? (0 - value) & mask : value;
        auto result = extF80_to_f128(&s, ui64_to_extF80(magnitude));
        if (negative) result.v[1] |= u64{1} << 63;
        result = ScalePowerOfTwo(result, -s32(fractional_bits));
        const auto narrowed = Narrow(env, f, result, s);
        return {narrowed, env.fpsr};
    }
    const u64 a = Input(env, f, value);
    // Any NaN converts to integer zero and IOC, independently of DN and payload.
    if (f.NaN(a)) return {0, env.fpsr | 1};
    constexpr u8 modes[]{softfloat_round_near_even, softfloat_round_min,
                         softfloat_round_max, softfloat_round_minMag, softfloat_round_near_maxMag};
    const u8 mode = modes[(operation - 2) / 2];
    const auto x = ScalePowerOfTwo(Widen(s, f, a), s32(fractional_bits));
    u64 result;
    if (is_unsigned) {
        result = f128_to_ui64(&s, x, mode, true);
        if (result > mask) s.exceptionFlags |= softfloat_flag_invalid;
    } else {
        const s64 converted = f128_to_i64(&s, x, mode, true);
        result = u64(converted);
        if (bits != 64 && (converted < -s64(f.sign) || converted > s64(f.sign - 1)))
            s.exceptionFlags |= softfloat_flag_invalid;
    }
    if (s.exceptionFlags & softfloat_flag_invalid) {
        // Invalid suppresses only this lane's inexact contribution. Other
        // lanes and pre-existing sticky FPSR remain independent.
        s.exceptionFlags &= ~softfloat_flag_inexact;
        result = is_unsigned ? (a & f.sign ? 0 : mask) : (a & f.sign ? f.sign : f.sign - 1);
    }
    Exceptions(env, s.exceptionFlags);
    return {result & mask, env.fpsr};
}

FPBinaryResult EvaluatePrecisionFP(u64 value, u32 source_bits, u32 destination_bits, bool odd, u32 fpcr) {
    struct Environment { u32 fpcr, fpsr; } env{fpcr, 0};
    const Format source(source_bits), destination(destination_bits);
    const u64 a = Input(env, source, value, true);
    if (source.NaN(a)) {
        const u64 nan = NaN(env, source, a);
        u64 payload = nan & source.fraction;
        payload = source.fraction_bits < destination.fraction_bits ?
                  payload << (destination.fraction_bits - source.fraction_bits) :
                  payload >> (source.fraction_bits - destination.fraction_bits);
        return {(nan & source.sign ? destination.sign : 0) | destination.exponent | payload | destination.quiet,env.fpsr};
    }
    auto s = State(softfloat_round_near_even);
    const auto result = Narrow(env, destination, Widen(s, source, a), s, true,
                              odd ? u8(softfloat_round_odd) : u8(255));
    return {result,env.fpsr};
}

FPBinaryResult EvaluateUnaryFP(u64 value, u32 bits, u32 op, u32 fpcr, u32 integer_bits) {
    struct Environment { u32 fpcr, fpsr; } env{fpcr, 0};
    const Format f(bits);
    auto s = State(softfloat_round_odd);
    const u64 a = Input(env, f, value);
    const u32 bias = bits == 16 ? 15 : bits == 32 ? 127 : 1023;
    const u64 indefinite = integer_bits ? f.sign | (u64(bias+integer_bits-1)<<f.fraction_bits) : 0;
    if (integer_bits && (a & f.exponent) == f.exponent)
        return {indefinite,env.fpsr|1};
    if (f.NaN(a)) {
        const auto result = NaN(env, f, a);
        return {result, env.fpsr};
    }
    if (op == FSQRT_s) {
        const auto result = Narrow(env, f, f128_sqrt(&s, Widen(s, f, a)), s);
        return {result, env.fpsr};
    }
    u8 mode;
    switch (op) {
        case FRINTN_s: mode = softfloat_round_near_even; break;
        case FRINTP_s: mode = softfloat_round_max; break;
        case FRINTM_s: mode = softfloat_round_min; break;
        case FRINTZ_s: mode = softfloat_round_minMag; break;
        case FRINTA_s: mode = softfloat_round_near_maxMag; break;
        default: mode = Rounding(fpcr); break;
    }
    auto extended = f128_to_extF80(&s, Widen(s, f, a));
    extended = extF80_roundToInt(&s, extended, mode, integer_bits || op == FRINTX_s);
    const auto rounded = extF80_to_f128(&s, extended);
    if (integer_bits) {
        // Binary128 positive encodings are ordered. Positive +2^(N-1) is
        // invalid; negative -2^(N-1) is valid only at the exact boundary.
        const u64 magnitude = rounded.v[1] & ~(u64{1}<<63);
        const u64 boundary = u64(16383+integer_bits-1)<<48;
        const bool negative = rounded.v[1] & (u64{1}<<63);
        const bool invalid = negative ? magnitude > boundary || (magnitude == boundary && rounded.v[0]) : magnitude >= boundary;
        if (invalid) return {indefinite,env.fpsr|1};
    }
    const auto result = Narrow(env, f, rounded, s);
    return {result, env.fpsr};
}

FPBinaryResult EvaluateFusedFP(u64 lhs, u64 rhs, u64 addend, u32 bits, u32 control, u32 fpcr) {
    struct Environment { u32 fpcr, fpsr; } env{fpcr, 0};
    const Format f(bits);
    auto s = State(softfloat_round_odd);
    u64 a = Input(env, f, lhs), b = Input(env, f, rhs), c = Input(env, f, addend);
    if (control & 1) a ^= f.sign;
    if (control & 2) c ^= f.sign;
    const bool invalid_product = ((a & ~f.sign) == f.exponent && (b & ~f.sign) == 0) ||
                                 ((b & ~f.sign) == f.exponent && (a & ~f.sign) == 0);
    if (invalid_product && !f.Signaling(a) && !f.Signaling(b) && !f.Signaling(c))
        return {f.DefaultNaN(), env.fpsr | 1};
    if (f.NaN(a) || f.NaN(b) || f.NaN(c)) {
        const auto result = NaN(env, f, c, a, b);
        return {result, env.fpsr};
    }
    // A double product has at most 106 significant bits and remains within
    // binary128's exponent range. Round-to-odd preserves a small addend's
    // correction so the final H/S/D narrowing rounds only once.
    const auto product = f128_mul(&s, Widen(s, f, a), Widen(s, f, b));
    auto result = f128_add(&s, product, Widen(s, f, c));
    if (result.v[0] == 0 && (result.v[1] << 1) == 0 &&
        bool(product.v[1] >> 63) != bool(c & f.sign))
        result.v[1] = Rounding(fpcr) == softfloat_round_min ? u64{1} << 63 : 0;
    const auto value = Narrow(env, f, result, s);
    return {value, env.fpsr};
}

FPBinaryResult EvaluateExtendedMultiplyFP(u64 lhs, u64 rhs, u32 bits, u32 fpcr) {
    struct Environment { u32 fpcr, fpsr; } env{fpcr, 0};
    const Format f(bits);
    const u64 a = Input(env, f, lhs), b = Input(env, f, rhs);
    if (f.NaN(a) || f.NaN(b)) {
        const auto result = NaN(env, f, a, b);
        return {result, env.fpsr};
    }
    if (((a & ~f.sign) == f.exponent && (b & ~f.sign) == 0) ||
        ((b & ~f.sign) == f.exponent && (a & ~f.sign) == 0)) {
        const u64 two = bits == 16 ? 0x4000 : bits == 32 ? 0x40000000 : 0x4000000000000000;
        return {((a ^ b) & f.sign) | two, env.fpsr};
    }
    auto result = EvaluateBinaryFP(a, b, bits, 0, fpcr);
    result.exceptions |= env.fpsr;
    return result;
}

FPBinaryResult EvaluateReciprocalStepFP(u64 lhs, u64 rhs, u32 bits, bool rsqrt, u32 fpcr) {
    struct Environment { u32 fpcr, fpsr; } env{fpcr, 0};
    const Format f(bits);
    const u64 a = Input(env, f, lhs) ^ f.sign, b = Input(env, f, rhs);
    if (f.NaN(a) || f.NaN(b)) {
        const auto result = NaN(env, f, a, b);
        return {result, env.fpsr};
    }
    const u64 two = bits == 16 ? 0x4000 : bits == 32 ? 0x40000000 : 0x4000000000000000;
    const u64 three = two | (u64{1} << (f.fraction_bits - 1));
    if (((a & ~f.sign) == f.exponent && (b & ~f.sign) == 0) ||
        ((b & ~f.sign) == f.exponent && (a & ~f.sign) == 0)) {
        const u64 one_and_half = bits == 16 ? 0x3E00 : bits == 32 ? 0x3FC00000 : 0x3FF8000000000000;
        return {rsqrt ? one_and_half : two, env.fpsr};
    }
    auto s = State(softfloat_round_odd);
    const auto product = f128_mul(&s, Widen(s, f, a), Widen(s, f, b));
    auto result = f128_add(&s, product, Widen(s, f, rsqrt ? three : two));
    if (result.v[0] == 0 && (result.v[1] << 1) == 0)
        result.v[1] = Rounding(fpcr) == softfloat_round_min ? u64{1} << 63 : 0;
    // Round-to-odd retains even a minimum-subnormal squared correction.
    // Scaling here avoids destination overflow/underflow before division.
    if (rsqrt) result = ScalePowerOfTwo(result, -1);
    const auto value = Narrow(env, f, result, s);
    return {value, env.fpsr};
}

bool IsSupportedScalarFP(u32 word) {
    const u32 type = (word >> 22) & 3;
    if (type == 2) return false;
    if (FixedPointConvert(word)) {
        const u32 opcode = (word >> 16) & 31;
        const u32 fractional_bits = 64 - ((word >> 10) & 63);
        return fractional_bits <= ((word >> 31) ? 64u : 32u) &&
               (opcode == 2 || opcode == 3 || opcode == 24 || opcode == 25);
    }
    if (IntegerConvert(word)) {
        const u32 opcode = (word >> 16) & 31;
        return opcode <= 5 || opcode == 8 || opcode == 9 ||
               opcode == 16 || opcode == 17 || opcode == 24 || opcode == 25;
    }
    if (TwoSource(word)) return ((word >> 12) & 15) <= 8;
    if (ThreeSource(word) || Compare(word)) return true;
    const u32 op = (word & FPDataProcessing1SourceMask) & ~(3u << 22);
    return op == FSQRT_s || op == FCVT_ds || op == (FCVT_sd & ~(3u << 22)) || op == FCVT_hs ||
           op == FRINTN_s || op == FRINTP_s || op == FRINTM_s || op == FRINTZ_s ||
           op == FRINTA_s || op == FRINTX_s || op == FRINTI_s;
}

u64 ExecuteScalarFP(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context_address);
    const u32 word = u32(instruction);
    if (!IsSupportedFPCR(ctx.fpcr) || !IsSupportedScalarFP(word)) {
        auto& state = *reinterpret_cast<runtime::backend::State*>(
                context_address - runtime::backend::state_offset_uniform_buffer);
        ctx.pc = guest_pc;
        ctx.padding = u32(InterruptReason::FALLBACK);
        state.current_loc = runtime::ir::Location{guest_pc};
        state.halt_reason = runtime::HaltReason::CallHost;
        return 0;
    }
    const u32 type = (word >> 22) & 3;
    const Format f(type == 3 ? 16 : type == 1 ? 64 : 32);
    const u32 rd = word & 31, rn = (word >> 5) & 31, rm = (word >> 16) & 31;
    const bool fixed = FixedPointConvert(word);
    const bool integer = fixed || IntegerConvert(word);
    const u32 fractional_bits = fixed ? 64 - ((word >> 10) & 63) : 0;
    const u32 one_source_op = (word & FPDataProcessing1SourceMask) & ~(3u << 22);
    const bool precision_convert = one_source_op == FCVT_ds ||
            one_source_op == (FCVT_sd & ~(3u << 22)) || one_source_op == FCVT_hs;
    auto s = State(softfloat_round_odd);
    if (integer && (((word >> 16) & 31) == 2 || ((word >> 16) & 31) == 3)) {
        const bool is64 = word >> 31, is_unsigned = word & (1u << 16);
        u64 value = ReadGPR(ctx, rn);
        if (!is64) value = is_unsigned ? u64(u32(value)) : u64(s64(s32(value)));
        const bool negative = !is_unsigned && (value >> 63);
        const u64 magnitude = negative ? 0 - value : value;
        auto result = extF80_to_f128(&s, ui64_to_extF80(magnitude));
        if (negative) result.v[1] |= u64{1} << 63;
        result = ScalePowerOfTwo(result, -s32(fractional_bits));
        Write(ctx, rd, Narrow(ctx, f, result, s));
        return 0;
    }
    if (TwoSource(word)) {
        const auto result = EvaluateBinaryFP(Read(ctx, rn, f.bits), Read(ctx, rm, f.bits),
                                             f.bits, (word >> 12) & 15, ctx.fpcr);
        ctx.fpsr |= result.exceptions;
        Write(ctx, rd, result.value);
        return 0;
    }
    if (ThreeSource(word)) {
        const bool negate_addend = word & (1u << 21);
        const bool negate_lhs = bool(word & (1u << 15)) != negate_addend;
        const u32 control = (negate_lhs ? 1u : 0u) | (negate_addend ? 2u : 0u);
        const auto result = EvaluateFusedFP(Read(ctx, rn, f.bits), Read(ctx, rm, f.bits),
            Read(ctx, (word >> 10) & 31, f.bits), f.bits, control, ctx.fpcr);
        ctx.fpsr |= result.exceptions;
        Write(ctx, rd, result.value);
        return 0;
    }
    if (!integer && !ThreeSource(word) && !Compare(word) && !precision_convert) {
        const auto result = EvaluateUnaryFP(Read(ctx, rn, f.bits), f.bits, one_source_op, ctx.fpcr);
        ctx.fpsr |= result.exceptions;
        Write(ctx, rd, result.value);
        return 0;
    }
    u64 a = Input(ctx, f, Read(ctx, rn, f.bits), precision_convert);
    const bool compare_zero = (word & FPCompareFMask) == FPCompareFixed && (word & 8);
    u64 b = !compare_zero && (TwoSource(word) || ThreeSource(word) || Compare(word))
            ? Input(ctx, f, Read(ctx, rm, f.bits)) : 0;
    if (integer) {
        const bool is64 = word >> 31, is_unsigned = word & (1u << 16);
        const u32 opcode = (word >> 16) & 31;
        u8 mode = opcode <= 1 ? softfloat_round_near_even :
                  opcode <= 5 ? softfloat_round_near_maxMag :
                  opcode <= 9 ? softfloat_round_max :
                  opcode <= 17 ? softfloat_round_min : softfloat_round_minMag;
        u64 result;
        if (f.NaN(a)) { result = 0; s.exceptionFlags |= softfloat_flag_invalid; }
        else {
            const auto x = ScalePowerOfTwo(Widen(s, f, a), s32(fractional_bits));
            if (is_unsigned) {
                result = f128_to_ui64(&s, x, mode, true);
                if (!is64 && result > 0xFFFFFFFF) s.exceptionFlags |= softfloat_flag_invalid;
            }
            else result = is64 ? u64(f128_to_i64(&s, x, mode, true)) : u32(f128_to_i32(&s, x, mode, true));
            if (s.exceptionFlags & softfloat_flag_invalid) {
                s.exceptionFlags &= ~softfloat_flag_inexact;
                if (is_unsigned) result = (a & f.sign) ? 0 : is64 ? ~u64{0} : u64{0xFFFFFFFF};
                else result = (a & f.sign) ? (is64 ? u64{1} << 63 : u64{1} << 31) :
                                                            (is64 ? (u64{1} << 63) - 1 : (u64{1} << 31) - 1);
            }
        }
        Exceptions(ctx, s.exceptionFlags);
        WriteGPR(ctx, rd, is64 ? result : u32(result));
        return 0;
    }
    if (Compare(word)) {
        if ((word & FPCompareFMask) == FPCompareFixed && (word & 8)) b = 0;
        if (f.NaN(a) || f.NaN(b)) {
            if ((word & 16) || f.Signaling(a) || f.Signaling(b)) ctx.fpsr |= 1;
            return 3;
        }
        const auto x = Widen(s, f, a), y = Widen(s, f, b);
        if (f128_eq(&s, x, y)) return 6;
        return f128_lt(&s, x, y) ? 8 : 2;
    }
    if (precision_convert) {
        const u32 conversion_opcode = (word >> 15) & 63;
        const Format destination(conversion_opcode == 7 ? 16 : conversion_opcode == 5 ? 64 : 32);
        if (f.NaN(a)) {
            u64 payload = NaN(ctx, f, a);
            const u64 sign = payload & f.sign ? destination.sign : 0;
            payload &= f.fraction;
            payload = f.fraction_bits < destination.fraction_bits ?
                      payload << (destination.fraction_bits - f.fraction_bits) :
                      payload >> (f.fraction_bits - destination.fraction_bits);
            Write(ctx, rd, sign | destination.exponent | payload | destination.quiet);
        } else Write(ctx, rd, Narrow(ctx, destination, Widen(s, f, a), s, true));
        return 0;
    }
    return 0;
}
bool detail::AdmitScalarFP(u64 target, u32 word) noexcept {
    return SingleInstructionTarget(target, &ExecuteScalarFP) && IsSupportedScalarFP(word);
}

}  // namespace swift::arm64
