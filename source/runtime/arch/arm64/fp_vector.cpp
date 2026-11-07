#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include "aarch64/constants-aarch64.h"
#include "numeric.h"

namespace swift::runtime::arm64 {
using namespace vixl::aarch64;

namespace family_fp16_unary {
namespace detail {
bool AdmitVectorFP16Unary(u64, u32) noexcept;

template <class F> bool SingleInstructionTarget(u64 address, F f) noexcept {
    return address == reinterpret_cast<u64>(f);
}
}  // namespace detail

namespace {
bool DecodeUnary(u32 word, u32& operation) {
    switch (word & NEON2RegMiscFP16Mask) {
        case NEON_FSQRT_H:
            operation = FSQRT_s;
            break;
        case NEON_FRINTN_H:
            operation = FRINTN_s;
            break;
        case NEON_FRINTM_H:
            operation = FRINTM_s;
            break;
        case NEON_FRINTP_H:
            operation = FRINTP_s;
            break;
        case NEON_FRINTZ_H:
            operation = FRINTZ_s;
            break;
        case NEON_FRINTA_H:
            operation = FRINTA_s;
            break;
        case NEON_FRINTX_H:
            operation = FRINTX_s;
            break;
        case NEON_FRINTI_H:
            operation = FRINTI_s;
            break;
        default:
            return false;
    }
    return true;
}
}  // namespace

u64 ExecuteVectorFP16Unary(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<GuestState*>(context_address);
    u32 operation;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeUnary(u32(instruction), operation)) {
        ctx.pc = guest_pc;
        ctx.helper_status = 1;
        return 0;
    }
    const u32 rn = (instruction >> 5) & 31, rd = instruction & 31;
    u16 source[8], destination[8]{};
    std::memcpy(source, &ctx.q[rn], sizeof(source));
    const u32 active = instruction & NEON_Q ? 8 : 4;
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        const auto result = EvaluateUnaryFP(source[lane], 16, operation, ctx.fpcr);
        destination[lane] = u16(result.value);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.q[rd], destination, sizeof(destination));
    return 0;
}

bool detail::AdmitVectorFP16Unary(u64 target, u32 word) noexcept {
    u32 operation;
    return SingleInstructionTarget(target, &ExecuteVectorFP16Unary) && DecodeUnary(word, operation);
}
}  // namespace family_fp16_unary

u64 NumericUnary16(u64 state, u64 word, u64 pc) {
    return family_fp16_unary::ExecuteVectorFP16Unary(state, word, pc);
}

bool SupportsUnary16(u32 word) {
    return family_fp16_unary::detail::AdmitVectorFP16Unary(
            reinterpret_cast<u64>(&family_fp16_unary::ExecuteVectorFP16Unary), word);
}

namespace family_fp16_binary {
namespace detail {
bool AdmitFP16Binary(u64, u32) noexcept;

template <class F> bool SingleInstructionTarget(u64 address, F f) noexcept {
    return address == reinterpret_cast<u64>(f);
}
}  // namespace detail

namespace {
bool DecodeBinary(u32 word, u32& operation, bool& absolute) {
    const bool scalar = word & NEONScalar;
    const u32 op = word & (scalar ? NEONScalar3SameFP16Mask : NEON3SameFP16Mask);
    absolute = op == (scalar ? NEON_FABD_H_scalar : NEON_FABD_H);
    if (absolute) {
        operation = 3;
        return true;
    }
    if (scalar) return false;
    switch (op) {
        case NEON_FADD_H:
            operation = 2;
            break;
        case NEON_FSUB_H:
            operation = 3;
            break;
        case NEON_FMUL_H:
            operation = 0;
            break;
        case NEON_FDIV_H:
            operation = 1;
            break;
        case NEON_FMAX_H:
            operation = 4;
            break;
        case NEON_FMIN_H:
            operation = 5;
            break;
        case NEON_FMAXNM_H:
            operation = 6;
            break;
        case NEON_FMINNM_H:
            operation = 7;
            break;
        default:
            return false;
    }
    return true;
}

u64 ExecuteFP16Binary(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<GuestState*>(context_address);
    u32 operation;
    bool absolute;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeBinary(u32(instruction), operation, absolute)) {
        ctx.pc = guest_pc;
        ctx.helper_status = 1;
        return 0;
    }
    const u32 rn = (instruction >> 5) & 31, rm = (instruction >> 16) & 31, rd = instruction & 31;
    u16 lhs[8], rhs[8], destination[8]{};
    std::memcpy(lhs, &ctx.q[rn], sizeof(lhs));
    std::memcpy(rhs, &ctx.q[rm], sizeof(rhs));
    const u32 active = instruction & NEONScalar ? 1 : instruction & NEON_Q ? 8 : 4;
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        const auto result = EvaluateBinaryFP(lhs[lane], rhs[lane], 16, operation, ctx.fpcr);
        destination[lane] = u16(absolute ? result.value & 0x7FFF : result.value);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.q[rd], destination, sizeof(destination));
    return 0;
}
}  // namespace

bool detail::AdmitFP16Binary(u64 target, u32 word) noexcept {
    u32 operation;
    bool absolute;
    return SingleInstructionTarget(target, &ExecuteFP16Binary) &&
           DecodeBinary(word, operation, absolute);
}
}  // namespace family_fp16_binary

u64 NumericBinary16(u64 state, u64 word, u64 pc) {
    return family_fp16_binary::ExecuteFP16Binary(state, word, pc);
}

bool SupportsBinary16(u32 word) {
    return family_fp16_binary::detail::AdmitFP16Binary(
            reinterpret_cast<u64>(&family_fp16_binary::ExecuteFP16Binary), word);
}

namespace family_vector_fp_binary {
namespace detail {
bool AdmitVectorFPBinary(u64, u32) noexcept;

template <class F> bool SingleInstructionTarget(u64 address, F f) noexcept {
    return address == reinterpret_cast<u64>(f);
}
}  // namespace detail

namespace {
bool DecodeBinary(u32 word, u32& operation, bool& absolute) {
    absolute = false;
    switch (word & NEON3SameFPMask) {
        case NEON_FADD:
            operation = 2;
            break;
        case NEON_FSUB:
            operation = 3;
            break;
        case NEON_FMUL:
            operation = 0;
            break;
        case NEON_FDIV:
            operation = 1;
            break;
        case NEON_FMAX:
            operation = 4;
            break;
        case NEON_FMIN:
            operation = 5;
            break;
        case NEON_FMAXNM:
            operation = 6;
            break;
        case NEON_FMINNM:
            operation = 7;
            break;
        case NEON_FABD:
            operation = 3;
            absolute = true;
            break;
        default:
            return false;
    }
    return true;
}

u64 ExecuteVectorFPBinary(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<GuestState*>(context_address);
    u32 operation;
    bool absolute;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeBinary(u32(instruction), operation, absolute)) {
        ctx.pc = guest_pc;
        ctx.helper_status = 1;
        return 0;
    }
    const u32 bits = instruction & (1u << 22) ? 64 : 32;
    const u32 active = (instruction & NEON_Q ? 128 : 64) / bits;
    const u32 rn = (instruction >> 5) & 31, rm = (instruction >> 16) & 31, rd = instruction & 31;
    u8 lhs[16], rhs[16], destination[16]{};
    std::memcpy(lhs, &ctx.q[rn], sizeof(lhs));
    std::memcpy(rhs, &ctx.q[rm], sizeof(rhs));
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        u64 left = 0, right = 0;
        std::memcpy(&left, lhs + lane * bits / 8, bits / 8);
        std::memcpy(&right, rhs + lane * bits / 8, bits / 8);
        auto result = EvaluateBinaryFP(left, right, bits, operation, ctx.fpcr);
        if (absolute) result.value &= ~(u64{1} << (bits - 1));
        std::memcpy(destination + lane * bits / 8, &result.value, bits / 8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.q[rd], destination, sizeof(destination));
    return 0;
}
}  // namespace

bool detail::AdmitVectorFPBinary(u64 target, u32 word) noexcept {
    u32 operation;
    bool absolute;
    return SingleInstructionTarget(target, &ExecuteVectorFPBinary) &&
           DecodeBinary(word, operation, absolute) && (!(word & (1u << 22)) || (word & NEON_Q));
}
}  // namespace family_vector_fp_binary

u64 NumericVectorBinary(u64 state, u64 word, u64 pc) {
    return family_vector_fp_binary::ExecuteVectorFPBinary(state, word, pc);
}

bool SupportsVectorBinary(u32 word) {
    return family_vector_fp_binary::detail::AdmitVectorFPBinary(
            reinterpret_cast<u64>(&family_vector_fp_binary::ExecuteVectorFPBinary), word);
}

namespace family_vector_fp_unary {
namespace detail {
bool AdmitVectorUnary(u64, u32) noexcept;

template <class F> bool SingleInstructionTarget(u64 address, F f) noexcept {
    return address == reinterpret_cast<u64>(f);
}
}  // namespace detail

namespace {
bool DecodeVectorUnary(u32 word, u32& operation) {
    switch (word & NEON2RegMiscFPMask) {
        case NEON_FSQRT:
            operation = FSQRT_s;
            break;
        case NEON_FRINTN:
            operation = FRINTN_s;
            break;
        case NEON_FRINTM:
            operation = FRINTM_s;
            break;
        case NEON_FRINTP:
            operation = FRINTP_s;
            break;
        case NEON_FRINTZ:
            operation = FRINTZ_s;
            break;
        case NEON_FRINTA:
            operation = FRINTA_s;
            break;
        case NEON_FRINTX:
            operation = FRINTX_s;
            break;
        case NEON_FRINTI:
            operation = FRINTI_s;
            break;
        default:
            return false;
    }
    return true;
}

u64 ExecuteVectorUnary(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<GuestState*>(context_address);
    const u32 word = u32(instruction);
    u32 operation;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeVectorUnary(word, operation)) {
        ctx.pc = guest_pc;
        ctx.helper_status = 1;
        return 0;
    }
    const u32 bits = word & (1u << 22) ? 64 : 32, rn = (word >> 5) & 31, rd = word & 31;
    u8 source[16], destination[16]{};
    std::memcpy(source, &ctx.q[rn], sizeof(source));
    const u32 active = (word & NEON_Q ? 128 : 64) / bits;
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        u64 value = 0;
        std::memcpy(&value, source + lane * bits / 8, bits / 8);
        const auto result = EvaluateUnaryFP(value, bits, operation, ctx.fpcr);
        std::memcpy(destination + lane * bits / 8, &result.value, bits / 8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.q[rd], destination, sizeof(destination));
    return 0;
}
}  // namespace

bool detail::AdmitVectorUnary(u64 target, u32 word) noexcept {
    u32 operation;
    return SingleInstructionTarget(target, &ExecuteVectorUnary) &&
           DecodeVectorUnary(word, operation) && (!(word & (1u << 22)) || (word & NEON_Q));
}
}  // namespace family_vector_fp_unary

u64 NumericVectorUnary(u64 state, u64 word, u64 pc) {
    return family_vector_fp_unary::ExecuteVectorUnary(state, word, pc);
}

bool SupportsVectorUnary(u32 word) {
    return family_vector_fp_unary::detail::AdmitVectorUnary(
            reinterpret_cast<u64>(&family_vector_fp_unary::ExecuteVectorUnary), word);
}

namespace family_fp_compare {
namespace detail {
bool AdmitFPCompare(u64, u32) noexcept;

template <class F> bool SingleInstructionTarget(u64 address, F f) noexcept {
    return address == reinterpret_cast<u64>(f);
}
}  // namespace detail

namespace {
bool DecodeFPCompare(u32 word, bool half, bool zero, FPCompareOperation& operation) {
    // Scalar forms share the vector opcode after removing their class bits.
    word &= ~(NEONScalar | NEON_Q);
    const u32 mask = half ? (zero ? NEON2RegMiscFP16Mask : NEON3SameFP16Mask)
                          : (zero ? NEON2RegMiscFPMask : NEON3SameFPMask);
    const u32 code = word & mask;
    if (zero) {
        if (code == u32(half ? NEON_FCMEQ_H_zero : NEON_FCMEQ_zero))
            operation = FPCompareOperation::Eq;
        else if (code == u32(half ? NEON_FCMGE_H_zero : NEON_FCMGE_zero))
            operation = FPCompareOperation::Ge;
        else if (code == u32(half ? NEON_FCMGT_H_zero : NEON_FCMGT_zero))
            operation = FPCompareOperation::Gt;
        else if (code == u32(half ? NEON_FCMLE_H_zero : NEON_FCMLE_zero))
            operation = FPCompareOperation::Le;
        else if (code == u32(half ? NEON_FCMLT_H_zero : NEON_FCMLT_zero))
            operation = FPCompareOperation::Lt;
        else
            return false;
    } else {
        if (code == u32(half ? NEON_FCMEQ_H : NEON_FCMEQ))
            operation = FPCompareOperation::Eq;
        else if (code == u32(half ? NEON_FCMGE_H : NEON_FCMGE))
            operation = FPCompareOperation::Ge;
        else if (code == u32(half ? NEON_FCMGT_H : NEON_FCMGT))
            operation = FPCompareOperation::Gt;
        else if (code == u32(half ? NEON_FACGE_H : NEON_FACGE))
            operation = FPCompareOperation::AbsGe;
        else if (code == u32(half ? NEON_FACGT_H : NEON_FACGT))
            operation = FPCompareOperation::AbsGt;
        else
            return false;
    }
    return true;
}

template <bool Half, bool Zero>
u64 ExecuteFPCompare(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<GuestState*>(context_address);
    const u32 word = u32(instruction);
    FPCompareOperation operation;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeFPCompare(word, Half, Zero, operation)) {
        ctx.pc = guest_pc;
        ctx.helper_status = 1;
        return 0;
    }
    const u32 bits = Half ? 16 : word & (1u << 22) ? 64 : 32;
    const u32 rn = (word >> 5) & 31, rm = (word >> 16) & 31, rd = word & 31;
    u8 lhs[16], rhs[16]{}, destination[16]{};
    std::memcpy(lhs, &ctx.q[rn], sizeof(lhs));
    if (!Zero) std::memcpy(rhs, &ctx.q[rm], sizeof(rhs));
    const u32 active = word & NEONScalar ? 1 : (word & NEON_Q ? 128 : 64) / bits;
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        u64 a = 0, b = 0;
        std::memcpy(&a, lhs + lane * bits / 8, bits / 8);
        std::memcpy(&b, rhs + lane * bits / 8, bits / 8);
        const auto result = EvaluateCompareFP(a, b, bits, operation, ctx.fpcr);
        std::memcpy(destination + lane * bits / 8, &result.value, bits / 8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.q[rd], destination, sizeof(destination));
    return 0;
}
}  // namespace

bool detail::AdmitFPCompare(u64 target, u32 word) noexcept {
    if (SingleInstructionTarget(target, &ExecuteFPCompare<true, true>)) {
        FPCompareOperation operation;
        return DecodeFPCompare(word, true, true, operation);
    }
    if (SingleInstructionTarget(target, &ExecuteFPCompare<true, false>)) {
        FPCompareOperation operation;
        return DecodeFPCompare(word, true, false, operation);
    }
    const bool zero = SingleInstructionTarget(target, &ExecuteFPCompare<false, true>);
    if (!zero && !SingleInstructionTarget(target, &ExecuteFPCompare<false, false>)) return false;
    FPCompareOperation operation;
    return DecodeFPCompare(word, false, zero, operation) &&
           (!(word & (1u << 22)) || (word & NEON_Q));
}
}  // namespace family_fp_compare

u64 NumericCompare00(u64 state, u64 word, u64 pc) {
    return family_fp_compare::ExecuteFPCompare<false, false>(state, word, pc);
}

bool SupportsCompare00(u32 word) {
    return family_fp_compare::detail::AdmitFPCompare(
            reinterpret_cast<u64>(&family_fp_compare::ExecuteFPCompare<false, false>), word);
}

u64 NumericCompare01(u64 state, u64 word, u64 pc) {
    return family_fp_compare::ExecuteFPCompare<false, true>(state, word, pc);
}

bool SupportsCompare01(u32 word) {
    return family_fp_compare::detail::AdmitFPCompare(
            reinterpret_cast<u64>(&family_fp_compare::ExecuteFPCompare<false, true>), word);
}

u64 NumericCompare10(u64 state, u64 word, u64 pc) {
    return family_fp_compare::ExecuteFPCompare<true, false>(state, word, pc);
}

bool SupportsCompare10(u32 word) {
    return family_fp_compare::detail::AdmitFPCompare(
            reinterpret_cast<u64>(&family_fp_compare::ExecuteFPCompare<true, false>), word);
}

u64 NumericCompare11(u64 state, u64 word, u64 pc) {
    return family_fp_compare::ExecuteFPCompare<true, true>(state, word, pc);
}

bool SupportsCompare11(u32 word) {
    return family_fp_compare::detail::AdmitFPCompare(
            reinterpret_cast<u64>(&family_fp_compare::ExecuteFPCompare<true, true>), word);
}

namespace family_scalar_fp16_misc {
namespace detail {
bool AdmitScalarFP16Misc(u64, u32) noexcept;

template <class F> bool SingleInstructionTarget(u64 address, F f) noexcept {
    return address == reinterpret_cast<u64>(f);
}
}  // namespace detail

namespace {
enum class Operation { Eq, Gt, Ge, Lt, Le, RecipExponent };

bool DecodeScalarFP16Misc(u32 word, Operation& operation) {
    switch (word & NEONScalar2RegMiscFP16Mask) {
        case NEON_FCMEQ_H_zero_scalar:
            operation = Operation::Eq;
            break;
        case NEON_FCMGT_H_zero_scalar:
            operation = Operation::Gt;
            break;
        case NEON_FCMGE_H_zero_scalar:
            operation = Operation::Ge;
            break;
        case NEON_FCMLT_H_zero_scalar:
            operation = Operation::Lt;
            break;
        case NEON_FCMLE_H_zero_scalar:
            operation = Operation::Le;
            break;
        case NEON_FRECPX_H_scalar:
            operation = Operation::RecipExponent;
            break;
        default:
            return false;
    }
    return true;
}

u64 ExecuteScalarFP16Misc(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<GuestState*>(context_address);
    Operation operation;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeScalarFP16Misc(u32(instruction), operation)) {
        ctx.pc = guest_pc;
        ctx.helper_status = 1;
        return 0;
    }
    const u32 rn = (instruction >> 5) & 31, rd = instruction & 31;
    u16 source;
    std::memcpy(&source, &ctx.q[rn], sizeof(source));
    const u16 sign = source & 0x8000;
    const u32 exponent = (source >> 10) & 31, fraction = source & 1023;
    u16 result;
    u32 exceptions = 0;
    if (exponent == 31 && fraction) {
        const bool signaling = !(fraction & 512);
        if (operation == Operation::RecipExponent) {
            result = ctx.fpcr & (1u << 25) ? 0x7E00 : source | 512;
            exceptions = signaling ? 1 : 0;
        } else {
            result = 0;
            exceptions = signaling || operation != Operation::Eq ? 1 : 0;
        }
    } else if (operation == Operation::RecipExponent) {
        // FRECPX complements the biased exponent; zero/subnormal exponent
        // uses max finite exponent. Fraction bits do not affect the result.
        result = sign | ((exponent == 0 ? 30 : 31 - exponent) << 10);
    } else {
        if (!exponent && fraction && (ctx.fpcr & (1u << 19))) source = sign;
        const bool zero = !(source & 0x7FFF), negative = sign != 0;
        bool match;
        switch (operation) {
            case Operation::Eq:
                match = zero;
                break;
            case Operation::Gt:
                match = !zero && !negative;
                break;
            case Operation::Ge:
                match = zero || !negative;
                break;
            case Operation::Lt:
                match = !zero && negative;
                break;
            default:
                match = zero || negative;
                break;
        }
        result = match ? 0xFFFF : 0;
    }
    ctx.fpsr |= exceptions;
    const u64 destination[] = {result, 0};
    std::memcpy(&ctx.q[rd], destination, sizeof(destination));
    return 0;
}
}  // namespace

bool detail::AdmitScalarFP16Misc(u64 target, u32 word) noexcept {
    Operation operation;
    return SingleInstructionTarget(target, &ExecuteScalarFP16Misc) &&
           DecodeScalarFP16Misc(word, operation);
}
}  // namespace family_scalar_fp16_misc

u64 NumericScalar16(u64 state, u64 word, u64 pc) {
    return family_scalar_fp16_misc::ExecuteScalarFP16Misc(state, word, pc);
}

bool SupportsScalar16(u32 word) {
    return family_scalar_fp16_misc::detail::AdmitScalarFP16Misc(
            reinterpret_cast<u64>(&family_scalar_fp16_misc::ExecuteScalarFP16Misc), word);
}

namespace family_scalar_pairwise_fp {
namespace detail {
bool AdmitScalarPairwiseFP(u64, u32) noexcept;

template <class F> bool SingleInstructionTarget(u64 address, F f) noexcept {
    return address == reinterpret_cast<u64>(f);
}
}  // namespace detail

namespace {
bool DecodePairwise(u32 word, u32& bits, u32& operation) {
    const u32 op = word & NEONScalarPairwiseMask;
    bool half = false;
    switch (op) {
        case NEON_FADDP_h_scalar:
            half = true;
            [[fallthrough]];
        case NEON_FADDP_scalar:
            operation = 2;
            break;
        case NEON_FMAXP_h_scalar:
            half = true;
            [[fallthrough]];
        case NEON_FMAXP_scalar:
            operation = 4;
            break;
        case NEON_FMINP_h_scalar:
            half = true;
            [[fallthrough]];
        case NEON_FMINP_scalar:
            operation = 5;
            break;
        case NEON_FMAXNMP_h_scalar:
            half = true;
            [[fallthrough]];
        case NEON_FMAXNMP_scalar:
            operation = 6;
            break;
        case NEON_FMINNMP_h_scalar:
            half = true;
            [[fallthrough]];
        case NEON_FMINNMP_scalar:
            operation = 7;
            break;
        default:
            return false;
    }
    bits = half ? 16 : (word & (1u << 22)) ? 64 : 32;
    return true;
}
}  // namespace

u64 ExecuteScalarPairwiseFP(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<GuestState*>(context_address);
    u32 bits, operation;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodePairwise(u32(instruction), bits, operation)) {
        ctx.pc = guest_pc;
        ctx.helper_status = 1;
        return 0;
    }
    const u32 rn = (instruction >> 5) & 31, rd = instruction & 31;
    u64 source[2];
    std::memcpy(source, &ctx.q[rn], sizeof(source));
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    const u64 lhs = source[0] & mask;
    const u64 rhs = bits == 64 ? source[1] : (source[0] >> bits) & mask;
    const auto result = EvaluateBinaryFP(lhs, rhs, bits, operation, ctx.fpcr);
    ctx.fpsr |= result.exceptions;
    const u64 destination[] = {result.value, 0};
    std::memcpy(&ctx.q[rd], destination, sizeof(destination));
    return 0;
}

bool detail::AdmitScalarPairwiseFP(u64 target, u32 word) noexcept {
    u32 bits, operation;
    return SingleInstructionTarget(target, &ExecuteScalarPairwiseFP) &&
           DecodePairwise(word, bits, operation);
}
}  // namespace family_scalar_pairwise_fp

u64 NumericPairwise(u64 state, u64 word, u64 pc) {
    return family_scalar_pairwise_fp::ExecuteScalarPairwiseFP(state, word, pc);
}

bool SupportsPairwise(u32 word) {
    return family_scalar_pairwise_fp::detail::AdmitScalarPairwiseFP(
            reinterpret_cast<u64>(&family_scalar_pairwise_fp::ExecuteScalarPairwiseFP), word);
}
}  // namespace swift::runtime::arm64
