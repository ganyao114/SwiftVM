#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include "aarch64/constants-aarch64.h"
#include "fp_estimate.h"
#include "numeric.h"

namespace swift::runtime::arm64 {
using namespace vixl::aarch64;

namespace family_fp_estimate {
namespace detail {
bool AdmitEstimate(u64, u32) noexcept;

template <class F> bool SingleInstructionTarget(u64 address, F f) noexcept {
    return address == reinterpret_cast<u64>(f);
}
}  // namespace detail

namespace {
bool DecodeEstimate(u32 word, bool half, FPEstimateOperation& operation) {
    word &= ~(NEON_Q | NEONScalar);
    const u32 code = word & (half ? NEON2RegMiscFP16Mask : NEON2RegMiscFPMask);
    if (code == u32(half ? NEON_FRECPE_H : NEON_FRECPE))
        operation = FPEstimateOperation::Reciprocal;
    else if (code == u32(half ? NEON_FRSQRTE_H : NEON_FRSQRTE))
        operation = FPEstimateOperation::ReciprocalSquareRoot;
    else
        return false;
    return true;
}

// FRECPX is allocated only in the SIMD scalar class and has its own opcode.
bool DecodeScalarExponent(u32 word, bool half) {
    return (word & (half ? NEONScalar2RegMiscFP16Mask : NEONScalar2RegMiscFPMask)) ==
           u32(half ? NEON_FRECPX_H_scalar : NEON_FRECPX_scalar);
}

template <bool Half> u64 ExecuteEstimate(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<GuestState*>(context_address);
    const u32 word = u32(instruction);
    FPEstimateOperation operation;
    const bool decoded = DecodeScalarExponent(word, Half)
                                 ? (operation = FPEstimateOperation::ReciprocalExponent, true)
                                 : DecodeEstimate(word, Half, operation);
    if (!IsSupportedFPCR(ctx.fpcr) || !decoded) {
        ctx.pc = guest_pc;
        ctx.helper_status = 1;
        return 0;
    }
    const u32 bits = Half ? 16 : word & (1u << 22) ? 64 : 32;
    const u32 rn = (word >> 5) & 31, rd = word & 31;
    u8 source[16], destination[16]{};
    std::memcpy(source, &ctx.q[rn], sizeof(source));
    const u32 active = word & NEONScalar ? 1 : (word & NEON_Q ? 128 : 64) / bits;
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        u64 value = 0;
        std::memcpy(&value, source + lane * bits / 8, bits / 8);
        const auto result = EvaluateEstimateFP(value, bits, operation, ctx.fpcr);
        std::memcpy(destination + lane * bits / 8, &result.value, bits / 8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.q[rd], destination, sizeof(destination));
    return 0;
}
}  // namespace

bool detail::AdmitEstimate(u64 target, u32 word) noexcept {
    const bool half = SingleInstructionTarget(target, &ExecuteEstimate<true>);
    if (!half && !SingleInstructionTarget(target, &ExecuteEstimate<false>)) return false;
    FPEstimateOperation operation;
    return (DecodeScalarExponent(word, half) || DecodeEstimate(word, half, operation)) &&
           (half || !(word & (1u << 22)) || (word & NEON_Q));
}
}  // namespace family_fp_estimate

u64 NumericEstimate(u64 state, u64 word, u64 pc) {
    return family_fp_estimate::ExecuteEstimate<false>(state, word, pc);
}

bool SupportsEstimate(u32 word) {
    return family_fp_estimate::detail::AdmitEstimate(
            reinterpret_cast<u64>(&family_fp_estimate::ExecuteEstimate<false>), word);
}

u64 NumericEstimate16(u64 state, u64 word, u64 pc) {
    return family_fp_estimate::ExecuteEstimate<true>(state, word, pc);
}

bool SupportsEstimate16(u32 word) {
    return family_fp_estimate::detail::AdmitEstimate(
            reinterpret_cast<u64>(&family_fp_estimate::ExecuteEstimate<true>), word);
}

namespace family_fp_reduction {
namespace detail {
bool AdmitReduction(u64, u32) noexcept;

template <class F> bool SingleInstructionTarget(u64 address, F f) noexcept {
    return address == reinterpret_cast<u64>(f);
}
}  // namespace detail

namespace {
bool DecodeReduction(u32 word, bool half, bool across, u32& operation) {
    const u32 mask = across ? (half ? NEONAcrossLanesFP16Mask : NEONAcrossLanesFPMask)
                            : (half ? NEON3SameFP16Mask : NEON3SameFPMask);
    const u32 code = word & mask;
    if (across) {
        if (code == u32(half ? NEON_FMAXV_H : NEON_FMAXV))
            operation = 4;
        else if (code == u32(half ? NEON_FMINV_H : NEON_FMINV))
            operation = 5;
        else if (code == u32(half ? NEON_FMAXNMV_H : NEON_FMAXNMV))
            operation = 6;
        else if (code == u32(half ? NEON_FMINNMV_H : NEON_FMINNMV))
            operation = 7;
        else
            return false;
    } else {
        if (code == u32(half ? NEON_FADDP_H : NEON_FADDP))
            operation = 2;
        else if (code == u32(half ? NEON_FMAXP_H : NEON_FMAXP))
            operation = 4;
        else if (code == u32(half ? NEON_FMINP_H : NEON_FMINP))
            operation = 5;
        else if (code == u32(half ? NEON_FMAXNMP_H : NEON_FMAXNMP))
            operation = 6;
        else if (code == u32(half ? NEON_FMINNMP_H : NEON_FMINNMP))
            operation = 7;
        else
            return false;
    }
    return true;
}

template <bool Half, bool Across>
u64 ExecuteReduction(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<GuestState*>(context_address);
    const u32 word = u32(instruction);
    u32 operation;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeReduction(word, Half, Across, operation)) {
        ctx.pc = guest_pc;
        ctx.helper_status = 1;
        return 0;
    }
    const u32 bits = Half ? 16 : word & (1u << 22) ? 64 : 32;
    const u32 active = (word & NEON_Q ? 128 : 64) / bits;
    const u32 rn = (word >> 5) & 31, rm = (word >> 16) & 31, rd = word & 31;
    u8 lhs[16], rhs[16]{}, destination[16]{};
    std::memcpy(lhs, &ctx.q[rn], sizeof(lhs));
    if (!Across) std::memcpy(rhs, &ctx.q[rm], sizeof(rhs));
    auto element = [&](const u8* source, u32 lane) {
        u64 value = 0;
        std::memcpy(&value, source + lane * bits / 8, bits / 8);
        return value;
    };
    u32 exceptions = 0;
    auto combine = [&](u64 a, u64 b) {
        const auto result = EvaluateBinaryFP(a, b, bits, operation, ctx.fpcr);
        exceptions |= result.exceptions;
        return result.value;
    };
    if (Across) {
        u64 values[8]{};
        for (u32 lane = 0; lane < active; ++lane) values[lane] = element(lhs, lane);
        // Recursively combine adjacent halves. With 8H this is
        // ((01,23),(45,67)); intermediate NaN quieting is observable.
        for (u32 remaining = active; remaining > 1; remaining /= 2)
            for (u32 lane = 0; lane < remaining / 2; ++lane)
                values[lane] = combine(values[lane * 2], values[lane * 2 + 1]);
        std::memcpy(destination, &values[0], bits / 8);
    } else {
        for (u32 lane = 0; lane < active; ++lane) {
            const u8* source = lane < active / 2 ? lhs : rhs;
            const u32 index = (lane % (active / 2)) * 2;
            const u64 value = combine(element(source, index), element(source, index + 1));
            std::memcpy(destination + lane * bits / 8, &value, bits / 8);
        }
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.q[rd], destination, sizeof(destination));
    return 0;
}
}  // namespace

bool detail::AdmitReduction(u64 target, u32 word) noexcept {
    const bool half = SingleInstructionTarget(target, &ExecuteReduction<true, true>) ||
                      SingleInstructionTarget(target, &ExecuteReduction<true, false>);
    const bool across = SingleInstructionTarget(target, &ExecuteReduction<true, true>) ||
                        SingleInstructionTarget(target, &ExecuteReduction<false, true>);
    if (!half && !across && !SingleInstructionTarget(target, &ExecuteReduction<false, false>))
        return false;
    u32 operation;
    return DecodeReduction(word, half, across, operation) && (across || !(word & NEONScalar)) &&
           (half || (across ? (word & NEON_Q) && !(word & (1u << 22))
                            : !(word & (1u << 22)) || (word & NEON_Q)));
}
}  // namespace family_fp_reduction

u64 NumericReduction00(u64 state, u64 word, u64 pc) {
    return family_fp_reduction::ExecuteReduction<false, false>(state, word, pc);
}

bool SupportsReduction00(u32 word) {
    return family_fp_reduction::detail::AdmitReduction(
            reinterpret_cast<u64>(&family_fp_reduction::ExecuteReduction<false, false>), word);
}

u64 NumericReduction01(u64 state, u64 word, u64 pc) {
    return family_fp_reduction::ExecuteReduction<false, true>(state, word, pc);
}

bool SupportsReduction01(u32 word) {
    return family_fp_reduction::detail::AdmitReduction(
            reinterpret_cast<u64>(&family_fp_reduction::ExecuteReduction<false, true>), word);
}

u64 NumericReduction10(u64 state, u64 word, u64 pc) {
    return family_fp_reduction::ExecuteReduction<true, false>(state, word, pc);
}

bool SupportsReduction10(u32 word) {
    return family_fp_reduction::detail::AdmitReduction(
            reinterpret_cast<u64>(&family_fp_reduction::ExecuteReduction<true, false>), word);
}

u64 NumericReduction11(u64 state, u64 word, u64 pc) {
    return family_fp_reduction::ExecuteReduction<true, true>(state, word, pc);
}

bool SupportsReduction11(u32 word) {
    return family_fp_reduction::detail::AdmitReduction(
            reinterpret_cast<u64>(&family_fp_reduction::ExecuteReduction<true, true>), word);
}

namespace family_vector_fma {
namespace detail {
bool AdmitVectorFMA(u64, u32) noexcept;

template <class F> bool SingleInstructionTarget(u64 address, F f) noexcept {
    return address == reinterpret_cast<u64>(f);
}
}  // namespace detail

namespace {
bool DecodeVectorFMA(u32 word, bool fp16, u32& bits, u32& control) {
    if (word & NEONScalar) return false;
    const u32 op = word & (fp16 ? NEON3SameFP16Mask : NEON3SameFPMask);
    if (op == (fp16 ? u32(NEON_FMLA_H) : u32(NEON_FMLA)))
        control = 0;
    else if (op == (fp16 ? u32(NEON_FMLS_H) : u32(NEON_FMLS)))
        control = 1;
    else
        return false;
    bits = fp16 ? 16 : (word & (1u << 22)) ? 64 : 32;
    return true;
}

u64 ExecuteVectorFMA(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<GuestState*>(context_address);
    const u32 word = u32(instruction);
    const bool fp16 = (word & NEON3SameFP16FMask) == NEON3SameFP16Fixed;
    u32 bits, control;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeVectorFMA(word, fp16, bits, control)) {
        ctx.pc = guest_pc;
        ctx.helper_status = 1;
        return 0;
    }
    const u32 rn = (word >> 5) & 31, rm = (word >> 16) & 31, rd = word & 31;
    u8 lhs[16], rhs[16], accumulator[16], destination[16]{};
    std::memcpy(lhs, &ctx.q[rn], sizeof(lhs));
    std::memcpy(rhs, &ctx.q[rm], sizeof(rhs));
    std::memcpy(accumulator, &ctx.q[rd], sizeof(accumulator));
    const u32 active = (word & NEON_Q ? 128 : 64) / bits;
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        u64 left = 0, right = 0, addend = 0;
        std::memcpy(&left, lhs + lane * bits / 8, bits / 8);
        std::memcpy(&right, rhs + lane * bits / 8, bits / 8);
        std::memcpy(&addend, accumulator + lane * bits / 8, bits / 8);
        const auto result = EvaluateFusedFP(left, right, addend, bits, control, ctx.fpcr);
        std::memcpy(destination + lane * bits / 8, &result.value, bits / 8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.q[rd], destination, sizeof(destination));
    return 0;
}
}  // namespace

bool detail::AdmitVectorFMA(u64 target, u32 word) noexcept {
    const bool fp16 = (word & NEON3SameFP16FMask) == NEON3SameFP16Fixed;
    u32 bits, control;
    return SingleInstructionTarget(target, &ExecuteVectorFMA) &&
           DecodeVectorFMA(word, fp16, bits, control) && (bits != 64 || (word & NEON_Q));
}
}  // namespace family_vector_fma

u64 NumericFused(u64 state, u64 word, u64 pc) {
    return family_vector_fma::ExecuteVectorFMA(state, word, pc);
}

bool SupportsFused(u32 word) {
    return family_vector_fma::detail::AdmitVectorFMA(
            reinterpret_cast<u64>(&family_vector_fma::ExecuteVectorFMA), word);
}

namespace family_reciprocal_step {
namespace detail {
bool AdmitExtendedFP(u64, u32) noexcept;

template <class F> bool SingleInstructionTarget(u64 address, F f) noexcept {
    return address == reinterpret_cast<u64>(f);
}
}  // namespace detail

namespace {
enum class Operation { ExtendedMultiply, ReciprocalStep, ReciprocalSquareRootStep };

bool DecodeExtendedFP(u32 word, bool fp16, u32& bits, Operation& operation) {
    const bool scalar = word & NEONScalar;
    const u32 mask = fp16 ? (scalar ? u32(NEONScalar3SameFP16Mask) : u32(NEON3SameFP16Mask))
                          : (scalar ? u32(NEONScalar3SameFPMask) : u32(NEON3SameFPMask));
    const u32 scalar_flags = scalar ? u32(NEON_Q | NEONScalar) : 0;
    const u32 op = word & mask;
    if (op == ((fp16 ? u32(NEON_FMULX_H) : u32(NEON_FMULX)) | scalar_flags))
        operation = Operation::ExtendedMultiply;
    else if (op == ((fp16 ? u32(NEON_FRECPS_H) : u32(NEON_FRECPS)) | scalar_flags))
        operation = Operation::ReciprocalStep;
    else if (op == ((fp16 ? u32(NEON_FRSQRTS_H) : u32(NEON_FRSQRTS)) | scalar_flags))
        operation = Operation::ReciprocalSquareRootStep;
    else
        return false;
    bits = fp16 ? 16 : (word & (1u << 22)) ? 64 : 32;
    return true;
}

u64 ExecuteExtendedFP(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<GuestState*>(context_address);
    const u32 word = u32(instruction);
    const bool scalar = word & NEONScalar;
    const bool fp16 = scalar ? (word & NEONScalar3SameFP16FMask) == NEONScalar3SameFP16Fixed
                             : (word & NEON3SameFP16FMask) == NEON3SameFP16Fixed;
    u32 bits;
    Operation operation;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeExtendedFP(word, fp16, bits, operation)) {
        ctx.pc = guest_pc;
        ctx.helper_status = 1;
        return 0;
    }
    const u32 rn = (word >> 5) & 31, rm = (word >> 16) & 31, rd = word & 31;
    u8 lhs[16], rhs[16], destination[16]{};
    std::memcpy(lhs, &ctx.q[rn], sizeof(lhs));
    std::memcpy(rhs, &ctx.q[rm], sizeof(rhs));
    const u32 active = scalar ? 1 : (word & NEON_Q ? 128 : 64) / bits;
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        u64 left = 0, right = 0;
        std::memcpy(&left, lhs + lane * bits / 8, bits / 8);
        std::memcpy(&right, rhs + lane * bits / 8, bits / 8);
        const auto result =
                operation == Operation::ExtendedMultiply
                        ? EvaluateExtendedMultiplyFP(left, right, bits, ctx.fpcr)
                        : EvaluateReciprocalStepFP(left,
                                                   right,
                                                   bits,
                                                   operation == Operation::ReciprocalSquareRootStep,
                                                   ctx.fpcr);
        std::memcpy(destination + lane * bits / 8, &result.value, bits / 8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.q[rd], destination, sizeof(destination));
    return 0;
}
}  // namespace

bool detail::AdmitExtendedFP(u64 target, u32 word) noexcept {
    const bool scalar = word & NEONScalar;
    const bool fp16 = scalar ? (word & NEONScalar3SameFP16FMask) == NEONScalar3SameFP16Fixed
                             : (word & NEON3SameFP16FMask) == NEON3SameFP16Fixed;
    u32 bits;
    Operation operation;
    return SingleInstructionTarget(target, &ExecuteExtendedFP) &&
           DecodeExtendedFP(word, fp16, bits, operation) &&
           (scalar || bits != 64 || (word & NEON_Q));
}
}  // namespace family_reciprocal_step

u64 NumericExtended(u64 state, u64 word, u64 pc) {
    return family_reciprocal_step::ExecuteExtendedFP(state, word, pc);
}

bool SupportsExtended(u32 word) {
    return family_reciprocal_step::detail::AdmitExtendedFP(
            reinterpret_cast<u64>(&family_reciprocal_step::ExecuteExtendedFP), word);
}

namespace family_indexed_fp {
namespace detail {
bool AdmitIndexedFP(u64, u32) noexcept;

template <class F> bool SingleInstructionTarget(u64 address, F f) noexcept {
    return address == reinterpret_cast<u64>(f);
}
}  // namespace detail

namespace {
enum class IndexedOperation { Multiply, ExtendedMultiply, MultiplyAdd, MultiplySubtract };

struct IndexedFP {
    u32 bits, rd, rn, rm, index, active;
    IndexedOperation operation;
    bool allocated;
};

bool DecodeIndexedFP(u32 word, IndexedFP& decoded) {
    const bool scalar = word & NEONScalar;
    const u32 mask = scalar ? u32(NEONScalarByIndexedElementMask) : u32(NEONByIndexedElementMask);
    const u32 flags = scalar ? u32(NEON_Q | NEONScalar) : 0;
    const u32 op = word & mask;
    if (op == (u32(NEON_FMUL_H_byelement) | flags))
        decoded.operation = IndexedOperation::Multiply;
    else if (op == (u32(NEON_FMULX_H_byelement) | flags))
        decoded.operation = IndexedOperation::ExtendedMultiply;
    else if (op == (u32(NEON_FMLA_H_byelement) | flags))
        decoded.operation = IndexedOperation::MultiplyAdd;
    else if (op == (u32(NEON_FMLS_H_byelement) | flags))
        decoded.operation = IndexedOperation::MultiplySubtract;
    else
        return false;
    const u32 size = (word >> 22) & 3, h = (word >> 11) & 1, l = (word >> 21) & 1,
              m = (word >> 20) & 1;
    decoded.bits = size == 0 ? 16 : size == 2 ? 32 : 64;
    decoded.rd = word & 31;
    decoded.rn = (word >> 5) & 31;
    // H uses H:L:M as its index and only four Rm bits. S/D use M as Rm[4].
    decoded.rm = (word >> 16) & (decoded.bits == 16 ? 15 : 31);
    decoded.index = decoded.bits == 16   ? (h << 2) | (l << 1) | m
                    : decoded.bits == 32 ? (h << 1) | l
                                         : h;
    decoded.active = scalar ? 1 : (word & NEON_Q ? 128 : 64) / decoded.bits;
    decoded.allocated = size != 1 && (decoded.bits != 64 || (!l && (scalar || (word & NEON_Q))));
    return true;
}

u64 ExecuteIndexedFP(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<GuestState*>(context_address);
    IndexedFP decoded;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeIndexedFP(u32(instruction), decoded) ||
        !decoded.allocated) {
        ctx.pc = guest_pc;
        ctx.helper_status = 1;
        return 0;
    }
    u8 lhs[16], indexed[16], accumulator[16], destination[16]{};
    std::memcpy(lhs, &ctx.q[decoded.rn], sizeof(lhs));
    std::memcpy(indexed, &ctx.q[decoded.rm], sizeof(indexed));
    std::memcpy(accumulator, &ctx.q[decoded.rd], sizeof(accumulator));
    u64 rhs = 0;
    std::memcpy(&rhs, indexed + decoded.index * decoded.bits / 8, decoded.bits / 8);
    u32 exceptions = 0;
    for (u32 lane = 0; lane < decoded.active; ++lane) {
        u64 left = 0, addend = 0;
        std::memcpy(&left, lhs + lane * decoded.bits / 8, decoded.bits / 8);
        std::memcpy(&addend, accumulator + lane * decoded.bits / 8, decoded.bits / 8);
        FPBinaryResult result;
        switch (decoded.operation) {
            case IndexedOperation::Multiply:
                result = EvaluateBinaryFP(left, rhs, decoded.bits, 0, ctx.fpcr);
                break;
            case IndexedOperation::ExtendedMultiply:
                result = EvaluateExtendedMultiplyFP(left, rhs, decoded.bits, ctx.fpcr);
                break;
            default:
                result = EvaluateFusedFP(
                        left,
                        rhs,
                        addend,
                        decoded.bits,
                        decoded.operation == IndexedOperation::MultiplySubtract ? 1 : 0,
                        ctx.fpcr);
                break;
        }
        std::memcpy(destination + lane * decoded.bits / 8, &result.value, decoded.bits / 8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.q[decoded.rd], destination, sizeof(destination));
    return 0;
}
}  // namespace

bool detail::AdmitIndexedFP(u64 target, u32 word) noexcept {
    IndexedFP decoded;
    return SingleInstructionTarget(target, &ExecuteIndexedFP) && DecodeIndexedFP(word, decoded) &&
           decoded.allocated;
}
}  // namespace family_indexed_fp

u64 NumericIndexed(u64 state, u64 word, u64 pc) {
    return family_indexed_fp::ExecuteIndexedFP(state, word, pc);
}

bool SupportsIndexed(u32 word) {
    return family_indexed_fp::detail::AdmitIndexedFP(
            reinterpret_cast<u64>(&family_indexed_fp::ExecuteIndexedFP), word);
}
}  // namespace swift::runtime::arm64
