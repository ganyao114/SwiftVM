#pragma once

#include "base/types.h"

namespace swift::arm64 {

bool IsSupportedScalarFP(u32 instruction);
bool IsSupportedFPCR(u32 fpcr);
// Operations use the A64 scalar two-source encoding: mul/div/add/sub,
// max/min/maxNum/minNum/negated mul. Inputs and output are raw H/S/D bits.
struct FPBinaryResult { u64 value; u32 exceptions; };
enum class FPCompareOperation { Eq, Ge, Gt, Le, Lt, AbsGe, AbsGt };
// SIMD comparison mask and sticky exceptions. Eq is quiet; ordered relations
// signal on any NaN. FPCR controls are checked by the architectural caller.
FPBinaryResult EvaluateCompareFP(u64 lhs, u64 rhs, u32 bits, FPCompareOperation operation, u32 fpcr);
// Same-width SIMD integer conversions: signed/unsigned integer-to-FP, then
// FP-to-integer N/M/P/Z/A rounding pairs. Integer rounding ignores FPCR.RMode.
FPBinaryResult EvaluateIntegerFP(u64 value, u32 bits, u32 operation, u32 fpcr, u32 fractional_bits = 0);
// Precision conversions preserve half subnormals even under FZ16. Narrowing
// uses FPCR.RMode, except FCVTXN's round-to-odd result.
FPBinaryResult EvaluatePrecisionFP(u64 value, u32 source_bits, u32 destination_bits, bool odd, u32 fpcr);
FPBinaryResult EvaluateBinaryFP(u64 lhs, u64 rhs, u32 bits, u32 operation, u32 fpcr);
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
u64 ExecuteScalarPairwiseFP(u64 context_address, u64 instruction, u64 guest_pc);
u64 ExecuteVectorFP16Unary(u64 context_address, u64 instruction, u64 guest_pc);

}  // namespace swift::arm64
