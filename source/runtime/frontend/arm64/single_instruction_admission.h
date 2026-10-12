#pragma once

#include "base/types.h"
#include "runtime/common/cast_utils.h"

namespace swift::runtime::ir {
class Block;
}

namespace swift::arm64 {

// Validate host calls in a fresh, unoptimized DecodeSingle block before any
// generated code enters. Guest-memory fault coverage is checked separately.
bool Arm64SingleInstructionAdmitted(const runtime::ir::Block& block);

namespace detail {
// Internal read-only queries keep anonymous helpers private to their TUs.
// Every admitted helper has the exact u64(u64 context, u64 word, u64 pc) ABI,
// accesses only ThreadContext/State and bounded local values, and cannot call
// user callbacks or access guest data. Instruction checks retain the decoder's
// allocation guards, including the guards needed for safe lane counts.
template <typename Function>
bool SingleInstructionTarget(u64 target, Function function) noexcept {
    return target == reinterpret_cast<u64>(runtime::FptrCast(function));
}

bool AdmitScalarFP(u64 target, u32 word) noexcept;
bool AdmitScalarPairwiseFP(u64 target, u32 word) noexcept;
bool AdmitVectorFP16Unary(u64 target, u32 word) noexcept;
bool AdmitFP16Binary(u64 target, u32 word) noexcept;
bool AdmitVectorFPBinary(u64 target, u32 word) noexcept;
bool AdmitVectorUnary(u64 target, u32 word) noexcept;
bool AdmitFPCompare(u64 target, u32 word) noexcept;
bool AdmitFPInteger(u64 target, u32 word) noexcept;
bool AdmitFixedSIMD(u64 target, u32 word) noexcept;
bool AdmitRangeRound(u64 target, u32 word) noexcept;
bool AdmitPrecision(u64 target, u32 word) noexcept;
bool AdmitEstimate(u64 target, u32 word) noexcept;
bool AdmitReduction(u64 target, u32 word) noexcept;
bool AdmitScalarFP16Misc(u64 target, u32 word) noexcept;
bool AdmitVectorFMA(u64 target, u32 word) noexcept;
bool AdmitExtendedFP(u64 target, u32 word) noexcept;
bool AdmitIndexedFP(u64 target, u32 word) noexcept;
}  // namespace detail
}  // namespace swift::arm64

