#pragma once
#include "state.h"

namespace swift::runtime::arm64 {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
}  // namespace swift::runtime::arm64

namespace swift::runtime::arm64 {

bool IsSupportedScalarFP(u32 instruction);
bool IsSupportedFPCR(u32 fpcr);

// Dense identity for every supported FZ16, RMode, FZ and DN combination.
constexpr u8 FPCRDomain(u32 fpcr) {
    return ((fpcr >> 19) & 1) | (((fpcr >> 22) & 3) << 1) | (((fpcr >> 24) & 3) << 3);
}

constexpr u32 FPCRForDomain(u8 domain) {
    return (u32{domain & 1u} << 19) | (u32{(domain >> 1) & 3u} << 22) |
           (u32{(domain >> 3) & 3u} << 24);
}

// Operations use the A64 scalar two-source encoding: mul/div/add/sub,
// max/min/maxNum/minNum/negated mul. Inputs and output are raw H/S/D bits.
struct FPBinaryResult {
    u64 value;
    u32 exceptions;
};
enum class FPCompareOperation { Eq, Ge, Gt, Le, Lt, AbsGe, AbsGt };
// SIMD comparison mask and sticky exceptions. Eq is quiet; ordered relations
// signal on any NaN. FPCR controls are checked by the architectural caller.
FPBinaryResult EvaluateCompareFP(
        u64 lhs, u64 rhs, u32 bits, FPCompareOperation operation, u32 fpcr);
// Same-width SIMD integer conversions: signed/unsigned integer-to-FP, then
// FP-to-integer N/M/P/Z/A rounding pairs. Integer rounding ignores FPCR.RMode.
FPBinaryResult EvaluateIntegerFP(
        u64 value, u32 bits, u32 operation, u32 fpcr, u32 fractional_bits = 0);
// Precision conversions preserve half subnormals even under FZ16. Narrowing
// uses FPCR.RMode, except FCVTXN's round-to-odd result.
FPBinaryResult EvaluatePrecisionFP(
        u64 value, u32 source_bits, u32 destination_bits, bool odd, u32 fpcr);
FPBinaryResult EvaluateBinaryFP(u64 lhs, u64 rhs, u32 bits, u32 operation, u32 fpcr);

// kind: 0=add, 1=sub, 2=mul, 3=div; bits: 32/64; lanes: 1/2/4.
// An indexed RHS broadcasts one of its original 128-bit input's lanes.
constexpr u32 ArmFPBinaryMode(u32 kind, u32 bits, u32 lanes, bool indexed = false, u32 index = 0) {
    return kind | (bits == 64 ? 4u : 0u) | (lanes << 3) | (indexed ? 64u : 0u) | (index << 7);
}

constexpr bool ValidArmFPBinaryMode(u32 mode) {
    const auto bits = mode & 4 ? 64u : 32u, lanes = (mode >> 3) & 7;
    return !(mode & ~511u) && (lanes == 1 || lanes == 2 || lanes == 4) && lanes * bits <= 128 &&
           ((mode & 3) != 3 || lanes == 1) &&
           ((mode & 64) ? (mode >> 7) < 128 / bits : !(mode >> 7));
}

// Inputs are snapshotted before any output; inactive destination lanes are
// zero. The caller validates the mode and supported FPCR at its normal boundary.
void EvaluateVectorBinaryFP(
        void* output, const void* lhs, const void* rhs, u32 mode, u32 fpcr, void* fpsr) noexcept;
// Operation is an A64 scalar one-source opcode with precision bits cleared.
// A nonzero integer_bits implements FPRoundIntN's signed range and indefinite
// result contract. Both Z/X report inexact, unless this lane is invalid.
FPBinaryResult EvaluateUnaryFP(u64 value, u32 bits, u32 operation, u32 fpcr, u32 integer_bits = 0);
// Exact fused multiply-add. Control bit 0 negates lhs before multiplication;
// bit 1 negates the addend, including NaN signs before ARM source selection.
FPBinaryResult EvaluateFusedFP(u64 lhs, u64 rhs, u64 addend, u32 bits, u32 control, u32 fpcr);
// FMULX preserves the product sign and returns 2 for zero times infinity.
FPBinaryResult EvaluateExtendedMultiplyFP(u64 lhs, u64 rhs, u32 bits, u32 fpcr);
// FRECPS is 2-lhs*rhs; FRSQRTS is (3-lhs*rhs)/2. Negation precedes
// NaN selection, and the reciprocal-square-root scale precedes narrowing.
FPBinaryResult EvaluateReciprocalStepFP(u64 lhs, u64 rhs, u32 bits, bool rsqrt, u32 fpcr);
// Returns NZCV in bits 3:0. Unsupported controls request a precise host exit.
u64 ExecuteScalarFP(u64 context_address, u64 instruction, u64 guest_pc);

}  // namespace swift::runtime::arm64

namespace swift::runtime::arm64 {
enum class Numeric : std::uint32_t {
    Scalar,
    Unary16,
    Binary16,
    VectorBinary,
    VectorUnary,
    Compare00,
    Compare01,
    Compare10,
    Compare11,
    Scalar16,
    Pairwise,
    Integer,
    Integer16,
    Fixed,
    RangeRound,
    Precision,
    Estimate,
    Estimate16,
    Reduction00,
    Reduction01,
    Reduction10,
    Reduction11,
    Fused,
    Extended,
    Indexed,
    Dot,
    SatMultiply,
    SatShift,
    UnsignedEstimate,
    Crc32,
    Count
};
bool SupportsNumeric(Numeric family, u32 word) noexcept;
u64 ExecuteNumeric(u64 state, u64 family_and_instruction, u64 pc);

constexpr u64 NumericCall(Numeric family, u32 word, u32 extra = 0) {
    return (u64(extra) << 40) | (u64(family) << 32) | word;
}

enum class FPValueKind { Invalid, Compare, Fused, Divide, Sqrt, Precision, FromInt, ToInt };

struct FPValueInfo {
    FPValueKind kind{FPValueKind::Invalid};
    u32 bits{32}, lanes{1}, output_bits{32}, integer_bits{32}, control{}, fractional_bits{};
    bool indexed{}, is_unsigned{}, vector_int{};
};

FPValueInfo DescribeFPValues(u64 encoded) noexcept;
u64 CanonicalFPValues(u64 encoded, const FPValueInfo&) noexcept;

// Packed offsets keep exact ternary operations within the IR's five arguments.
constexpr u64 FPStateOffsets(u32 control, u32 status) { return u64{control} | (u64{status} << 32); }

// Inputs are three raw 128-bit SSA values. The temporary architectural state
// belongs to this call and observes only these values and the explicit controls.
u64 EvaluateFPValues(void* output, const void* inputs, u64 encoded, u32 fpcr, void* fpsr);
bool IsFPBitUnary(u32 word) noexcept;
u64 EvaluateFPBitUnary(u64 value, u32 word, u32 fpcr) noexcept;
}  // namespace swift::runtime::arm64
