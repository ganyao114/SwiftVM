#include <bit>
#include <cstring>
#include <limits>
#include "numeric.h"
#include "runtime/arch/arm64/fp_estimate.h"
#include "runtime/arch/arm64/numeric.h"
#include "state.h"

namespace swift::runtime::arm32 {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;
using Wide = __int128;
using UWide = unsigned __int128;

constexpr u64 Mask(unsigned bits) { return bits == 64 ? ~u64{} : (u64{1} << bits) - 1; }

constexpr Wide Signed(u64 value, unsigned bits) {
    value &= Mask(bits);
    return value & (u64{1} << (bits - 1)) ? Wide(value) - (Wide{1} << bits) : Wide(value);
}

u64 Clamp(Wide value, unsigned bits, bool is_signed, u32& fpscr) {
    const Wide low = is_signed ? -(Wide{1} << (bits - 1)) : 0;
    const Wide high = is_signed ? (Wide{1} << (bits - 1)) - 1 : (Wide{1} << bits) - 1;
    if (value < low) {
        value = low;
        fpscr |= 1u << 27;
    }
    if (value > high) {
        value = high;
        fpscr |= 1u << 27;
    }
    return u64(value) & Mask(bits);
}

u32 RoundControl(u32 fpcr, unsigned mode) {
    return (fpcr & ~(3u << 22)) | ((mode < 4 ? mode : 0) << 22);
}

constexpr u32 RoundOpcode(unsigned mode, bool exact) {
    if (exact) return 0x1e274000;  // FRINTX
    constexpr u32 codes[]{0x1e244000, 0x1e24c000, 0x1e254000, 0x1e25c000, 0x1e264000};
    return codes[mode < 5 ? mode : 0];
}

bool NaN(u64 value, unsigned bits) {
    const unsigned f = bits == 64 ? 52 : bits == 32 ? 23 : 10;
    const u64 exp = bits == 64 ? 0x7ff : bits == 32 ? 0xff : 0x1f;
    return ((value >> f) & exp) == exp && (value & ((u64{1} << f) - 1));
}

// ARM's alternative half format has exponent 31 as a finite binade.
// The conversion is integer-only so host half/NaN policy cannot leak in.
arm64::FPBinaryResult ConvertAlternativeHalf(u64 input,
                                             unsigned source,
                                             unsigned destination,
                                             u32 fpcr) {
    if (source == 16) {
        if ((input & 0x7c00) != 0x7c00)
            return arm64::EvaluatePrecisionFP(input, 16, destination, false, fpcr);
        const u32 single = (u32(input & 0x8000) << 16) | (143u << 23) | (u32(input & 1023) << 13);
        if (destination == 32) return {single, 0};
        return arm64::EvaluatePrecisionFP(single, 32, destination, false, fpcr);
    }
    const unsigned fraction_bits = source == 64 ? 52 : 23;
    const int bias = source == 64 ? 1023 : 127;
    const u64 fraction_mask = (u64{1} << fraction_bits) - 1;
    const u64 sign = (input >> (source - 1)) << 15;
    const unsigned exponent = unsigned(input >> fraction_bits) & unsigned(2 * bias + 1);
    u64 mantissa = input & fraction_mask;
    if (exponent == unsigned(2 * bias + 1)) return {sign | (mantissa ? 0 : 0x7fff), 1};
    if (!exponent && !mantissa) return {sign, 0};
    if (!exponent && (fpcr & (1u << 24))) return {sign, 128};
    int power = exponent ? int(exponent) - bias : 1 - bias;
    if (exponent)
        mantissa |= u64{1} << fraction_bits;
    else
        while (!(mantissa & (u64{1} << fraction_bits))) {
            mantissa <<= 1;
            --power;
        }
    const unsigned shift = fraction_bits - 10 + (power < -14 ? unsigned(-14 - power) : 0);
    u64 rounded = shift < 64 ? mantissa >> shift : 0;
    const u64 discarded = shift < 64 ? mantissa & ((u64{1} << shift) - 1) : mantissa;
    const u64 halfway = shift < 64 ? u64{1} << (shift - 1) : ~u64{};
    const unsigned mode = (fpcr >> 22) & 3;
    const bool increment = mode == 0
                                   ? discarded > halfway || (discarded == halfway && (rounded & 1))
                           : mode == 1 ? discarded && !sign
                           : mode == 2 ? discarded && sign
                                       : false;
    if (increment) ++rounded;
    unsigned encoded_exp = power < -14 ? 0 : unsigned(power + 15);
    u32 exceptions = discarded ? 16 : 0;
    if (!encoded_exp && discarded) exceptions |= 8;
    if (!encoded_exp && rounded >= 1024) encoded_exp = 1;
    if (rounded >= 2048) {
        rounded >>= 1;
        ++encoded_exp;
    }
    if (encoded_exp >= 32) return {sign | 0x7fff, 1};
    return {sign | (u64(encoded_exp) << 10) | (rounded & 1023), exceptions};
}
}  // namespace

u64 EvaluateNumeric(
        u64 fpscr_address, Numeric operation, unsigned bits, bool standard_fp, u64 lhs, u64 rhs) {
    auto& fpscr = *reinterpret_cast<u32*>(fpscr_address);
    const u32 fpcr = standard_fp ? ((fpscr & (1u << 26)) | (3u << 24)) : fpscr & 0x07c00000;
    const u64 mask = Mask(bits);
    lhs &= mask;
    const auto commit = [&](arm64::FPBinaryResult result) {
        fpscr |= result.exceptions & 0x9f;
        return result.value;
    };
    if (operation <= Numeric::MinNumeric) {
        constexpr u32 ops[]{2, 3, 0, 1, 4, 5, 6, 7};
        return commit(arm64::EvaluateBinaryFP(lhs, rhs, bits, ops[unsigned(operation)], fpcr));
    }
    switch (operation) {
        case Numeric::CompareEqual:
        case Numeric::CompareGreater:
        case Numeric::CompareGreaterEqual: {
            using C = arm64::FPCompareOperation;
            const C op = operation == Numeric::CompareEqual     ? C::Eq
                         : operation == Numeric::CompareGreater ? C::Gt
                                                                : C::Ge;
            return commit(arm64::EvaluateCompareFP(lhs, rhs, bits, op, fpcr));
        }
        case Numeric::CompareFlags:
        case Numeric::CompareFlagsSignal: {
            auto eq = arm64::EvaluateCompareFP(lhs, rhs, bits, arm64::FPCompareOperation::Eq, fpcr);
            if (NaN(lhs, bits) || NaN(rhs, bits)) {
                if (operation == Numeric::CompareFlagsSignal) eq.exceptions |= 1;
                commit(eq);
                return 0x30000000;
            }
            if (eq.value) {
                commit(eq);
                return 0x60000000;
            }
            auto gt = arm64::EvaluateCompareFP(lhs, rhs, bits, arm64::FPCompareOperation::Gt, fpcr);
            gt.exceptions |= eq.exceptions;
            commit(gt);
            return gt.value ? 0x20000000 : 0x80000000;
        }
        case Numeric::Sqrt:
            return commit(arm64::EvaluateUnaryFP(lhs, bits, 0x1e21c000, fpcr));
        case Numeric::Round: {
            const unsigned mode = (rhs >> 8) & 7;
            return commit(arm64::EvaluateUnaryFP(
                    lhs, bits, RoundOpcode(mode, rhs & (1u << 24)), RoundControl(fpcr, mode)));
        }
        case Numeric::Precision:
            if ((fpcr & (1u << 26)) && (bits == 16 || (rhs & 255) == 16))
                return commit(ConvertAlternativeHalf(
                        lhs, bits, rhs & 255, RoundControl(fpcr, (rhs >> 8) & 7)));
            return commit(arm64::EvaluatePrecisionFP(
                    lhs, bits, rhs & 255, false, RoundControl(fpcr, (rhs >> 8) & 7)));
        case Numeric::FromSigned:
        case Numeric::FromUnsigned: {
            const unsigned integer_bits = rhs & 255;
            const bool unsign = operation == Numeric::FromUnsigned;
            const u64 input = unsign ? lhs & Mask(integer_bits) : u64(Signed(lhs, integer_bits));
            // Widening the integer representation changes no value. Narrow the
            // numerical result once, in the guest's requested precision.
            auto value = arm64::EvaluateIntegerFP(input,
                                                  64,
                                                  unsigned(unsign),
                                                  RoundControl(fpcr, (rhs >> 8) & 7),
                                                  (rhs >> 16) & 127);
            if (bits != 64) {
                auto narrowed = arm64::EvaluatePrecisionFP(
                        value.value, 64, bits, false, RoundControl(fpcr, (rhs >> 8) & 7));
                narrowed.exceptions |= value.exceptions;
                return commit(narrowed);
            }
            return commit(value);
        }
        case Numeric::ToSigned:
        case Numeric::ToUnsigned: {
            const unsigned out_bits = rhs & 255, mode = (rhs >> 8) & 7;
            const bool unsign = operation == Numeric::ToUnsigned;
            constexpr unsigned modes[]{0, 2, 1, 3, 4};
            auto value = arm64::EvaluateIntegerFP(lhs,
                                                  bits,
                                                  2 + 2 * modes[mode < 5 ? mode : 0] + unsign,
                                                  fpcr,
                                                  (rhs >> 16) & 127);
            const Wide integer = unsign ? Wide(value.value) : Signed(value.value, bits);
            const Wide lo = unsign ? 0 : -(Wide{1} << (out_bits - 1));
            const Wide hi = unsign ? (Wide{1} << out_bits) - 1 : (Wide{1} << (out_bits - 1)) - 1;
            if (integer < lo || integer > hi) {
                value.value = u64(integer < lo ? lo : hi);
                value.exceptions = (value.exceptions & ~16u) | 1u;
            }
            return commit(value) & Mask(out_bits);
        }
        case Numeric::Fused:
        case Numeric::FusedSubtract: {
            u64 addend;
            std::memcpy(&addend,
                        reinterpret_cast<const void*>(fpscr_address +
                                                      offsetof(GuestState, numeric_addend) -
                                                      offsetof(GuestState, fpscr)),
                        sizeof(addend));
            return commit(arm64::EvaluateFusedFP(
                    lhs, rhs, addend, bits, operation == Numeric::FusedSubtract ? 1 : 0, fpcr));
        }
        case Numeric::ReciprocalEstimate:
        case Numeric::RSqrtEstimate: {
            const auto result = arm64::EvaluateEstimateFP(
                    lhs,
                    bits,
                    operation == Numeric::ReciprocalEstimate
                            ? arm64::FPEstimateOperation::Reciprocal
                            : arm64::FPEstimateOperation::ReciprocalSquareRoot,
                    fpcr);
            return commit({result.value, result.exceptions});
        }
        case Numeric::ReciprocalStep:
        case Numeric::RSqrtStep:
            return commit(arm64::EvaluateReciprocalStepFP(
                    lhs, rhs, bits, operation == Numeric::RSqrtStep, fpcr));
        default:
            break;
    }
    const Wide a = Signed(lhs, bits), b = Signed(rhs, bits);
    switch (operation) {
        case Numeric::HalvingAddSigned:
            return u64((a + b) >> 1) & mask;
        case Numeric::HalvingAddUnsigned:
            return u64((UWide(lhs) + (rhs & mask)) >> 1) & mask;
        case Numeric::HalvingSubSigned:
            return u64((a - b) >> 1) & mask;
        case Numeric::HalvingSubUnsigned:
            return u64((Wide(lhs) - (rhs & mask)) >> 1) & mask;
        case Numeric::RoundingHalvingAddSigned:
            return u64((a + b + 1) >> 1) & mask;
        case Numeric::RoundingHalvingAddUnsigned:
            return u64((UWide(lhs) + (rhs & mask) + 1) >> 1) & mask;
        case Numeric::SaturatingAddSigned:
            return Clamp(a + b, bits, true, fpscr);
        case Numeric::SaturatingAddUnsigned:
            return Clamp(Wide(lhs) + (rhs & mask), bits, false, fpscr);
        case Numeric::SaturatingSubSigned:
            return Clamp(a - b, bits, true, fpscr);
        case Numeric::SaturatingSubUnsigned:
            return Clamp(Wide(lhs) - (rhs & mask), bits, false, fpscr);
        case Numeric::SaturatingAbs:
            return Clamp(a < 0 ? -a : a, bits, true, fpscr);
        case Numeric::SaturatingNeg:
            return Clamp(-a, bits, true, fpscr);
        case Numeric::SaturatingNarrowSigned:
            return Clamp(a, bits / 2, true, fpscr);
        case Numeric::SaturatingNarrowUnsigned:
            return Clamp(lhs, bits / 2, false, fpscr);
        case Numeric::SaturatingNarrowSignedToUnsigned:
            return Clamp(a, bits / 2, false, fpscr);
        case Numeric::DoublingMultiplyLong:
            return Clamp(a * b * 2, bits * 2, true, fpscr);
        case Numeric::DoublingMultiplyHigh:
        case Numeric::RoundingDoublingMultiplyHigh: {
            Wide product = a * b * 2;
            if (operation == Numeric::RoundingDoublingMultiplyHigh)
                product += Wide{1} << (bits - 1);
            return Clamp(product >> bits, bits, true, fpscr);
        }
        case Numeric::ShiftSigned:
        case Numeric::ShiftUnsigned:
        case Numeric::RoundingShiftSigned:
        case Numeric::RoundingShiftUnsigned:
        case Numeric::SaturatingShiftSigned:
        case Numeric::SaturatingShiftUnsigned:
        case Numeric::SaturatingShiftSignedToUnsigned: {
            const int shift = static_cast<std::int8_t>(rhs);
            const bool sign = operation == Numeric::ShiftSigned ||
                              operation == Numeric::RoundingShiftSigned ||
                              operation == Numeric::SaturatingShiftSigned ||
                              operation == Numeric::SaturatingShiftSignedToUnsigned;
            const bool round = operation == Numeric::RoundingShiftSigned ||
                               operation == Numeric::RoundingShiftUnsigned;
            const bool sat = operation == Numeric::SaturatingShiftSigned ||
                             operation == Numeric::SaturatingShiftUnsigned ||
                             operation == Numeric::SaturatingShiftSignedToUnsigned;
            Wide value = sign ? a : Wide(lhs);
            if (shift < 0) {
                const unsigned count = unsigned(-shift);
                if (count > bits) return sign && !round && a < 0 ? mask : 0;
                if (round) value += Wide{1} << (count - 1);
                return u64(value >> count) & mask;
            }
            if (unsigned(shift) >= bits) {
                if (!sat || !value) return 0;
                return Clamp(value < 0 ? -(Wide{1} << bits) : Wide{1} << bits,
                             bits,
                             operation == Numeric::SaturatingShiftSigned,
                             fpscr);
            }
            value *= Wide{1} << shift;
            return sat ? Clamp(value, bits, operation == Numeric::SaturatingShiftSigned, fpscr)
                       : u64(value) & mask;
        }
        case Numeric::PolynomialMultiply: {
            u64 result = 0;
            for (unsigned i = 0; i < bits; ++i)
                if ((rhs >> i) & 1) result ^= lhs << i;
            return result;
        }
        case Numeric::CountLeadingZeros:
            return lhs ? std::countl_zero(lhs) - (64 - bits) : bits;
        case Numeric::PopulationCount:
            return std::popcount(lhs);
        case Numeric::UnsignedReciprocalEstimate:
            if (!(lhs & 0x80000000)) return 0xffffffff;
            return u64(arm64::ReciprocalMidpoint(u32(lhs >> 23))) << 23;
        case Numeric::UnsignedRSqrtEstimate: {
            if (!(lhs & 0xc0000000)) return 0xffffffff;
            const bool odd = !(lhs & 0x80000000);
            const u32 index = u32(lhs >> (odd ? 23 : 24));
            return u64(arm64::ReciprocalRootMidpoint(index, odd)) << 23;
        }
        default:
            return 0;
    }
}
}  // namespace swift::runtime::arm32
