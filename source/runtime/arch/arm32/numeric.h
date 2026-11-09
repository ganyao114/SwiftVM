#pragma once
#include <cstdint>

namespace swift::runtime::arm32 {
enum class RoundingMode : unsigned {
    ToNearest_TieEven,
    TowardsPlusInfinity,
    TowardsMinusInfinity,
    TowardsZero,
    ToNearest_TieAwayFromZero
};
// These are numerical lane operations, independent of instruction encodings and
// register layout. The first callback argument addresses only FPSCR; no A64
// architectural state is manufactured by the A32 frontend.
enum class Numeric {
    Add,
    Sub,
    Mul,
    Div,
    Max,
    Min,
    MaxNumeric,
    MinNumeric,
    CompareEqual,
    CompareGreater,
    CompareGreaterEqual,
    CompareFlags,
    CompareFlagsSignal,
    Sqrt,
    Round,
    Precision,
    FromSigned,
    FromUnsigned,
    ToSigned,
    ToUnsigned,
    Fused,
    FusedSubtract,
    ReciprocalEstimate,
    RSqrtEstimate,
    ReciprocalStep,
    RSqrtStep,
    HalvingAddSigned,
    HalvingAddUnsigned,
    HalvingSubSigned,
    HalvingSubUnsigned,
    RoundingHalvingAddSigned,
    RoundingHalvingAddUnsigned,
    ShiftSigned,
    ShiftUnsigned,
    RoundingShiftSigned,
    RoundingShiftUnsigned,
    SaturatingShiftSigned,
    SaturatingShiftUnsigned,
    SaturatingShiftSignedToUnsigned,
    SaturatingAbs,
    SaturatingNeg,
    SaturatingNarrowSigned,
    SaturatingNarrowUnsigned,
    SaturatingNarrowSignedToUnsigned,
    SaturatingAddSigned,
    SaturatingAddUnsigned,
    SaturatingSubSigned,
    SaturatingSubUnsigned,
    DoublingMultiplyHigh,
    RoundingDoublingMultiplyHigh,
    DoublingMultiplyLong,
    PolynomialMultiply,
    CountLeadingZeros,
    PopulationCount,
    UnsignedReciprocalEstimate,
    UnsignedRSqrtEstimate
};

// Exception traps are not represented by the user-mode helper protocol. All
// sticky flags, DN/FZ/AHP and rounding controls are otherwise implemented.
constexpr bool IsSupportedFPSCR(std::uint32_t value) noexcept { return (value & 0x00009f00u) == 0; }

using NumericCallback = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t);
std::uint64_t EvaluateNumeric(std::uint64_t fpscr_address,
                              Numeric operation,
                              unsigned bits,
                              bool standard_fp,
                              std::uint64_t lhs,
                              std::uint64_t rhs);

template <Numeric operation, unsigned bits, bool standard_fp = false>
std::uint64_t NumericLane(std::uint64_t fpscr, std::uint64_t lhs, std::uint64_t rhs) {
    return EvaluateNumeric(fpscr, operation, bits, standard_fp, lhs, rhs);
}

// Conversion descriptor in the third numerical operand. Rounding mode follows
// FPSCR's encoding, with 4 reserved for ties away; it is never host fenv state.
constexpr std::uint64_t NumericConvert(unsigned width,
                                       RoundingMode rounding,
                                       unsigned fractional_bits = 0,
                                       bool exact = false) {
    return width | (std::uint64_t(rounding) << 8) | (std::uint64_t(fractional_bits) << 16) |
           (std::uint64_t(exact) << 24);
}
}  // namespace swift::runtime::arm32
