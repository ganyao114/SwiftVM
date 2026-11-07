#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include "aarch64/constants-aarch64.h"
#include "numeric.h"

namespace swift::runtime::arm64 {
using namespace vixl::aarch64;

namespace family_fp_integer {
namespace detail {
bool AdmitFPInteger(u64, u32) noexcept;

template <class F> bool SingleInstructionTarget(u64 address, F f) noexcept {
    return address == reinterpret_cast<u64>(f);
}
}  // namespace detail

namespace {
bool DecodeFPInteger(u32 word, bool half, u32& operation) {
    word &= ~(NEONScalar | NEON_Q);
    const u32 code = word & (half ? NEON2RegMiscFP16Mask : NEON2RegMiscFPMask);
    const u32 bases[]{NEON_SCVTF,
                      NEON_UCVTF,
                      NEON_FCVTNS,
                      NEON_FCVTNU,
                      NEON_FCVTMS,
                      NEON_FCVTMU,
                      NEON_FCVTPS,
                      NEON_FCVTPU,
                      NEON_FCVTZS,
                      NEON_FCVTZU,
                      NEON_FCVTAS,
                      NEON_FCVTAU};
    const u32 halves[]{NEON_SCVTF_H,
                       NEON_UCVTF_H,
                       NEON_FCVTNS_H,
                       NEON_FCVTNU_H,
                       NEON_FCVTMS_H,
                       NEON_FCVTMU_H,
                       NEON_FCVTPS_H,
                       NEON_FCVTPU_H,
                       NEON_FCVTZS_H,
                       NEON_FCVTZU_H,
                       NEON_FCVTAS_H,
                       NEON_FCVTAU_H};
    for (u32 index = 0; index < 12; ++index)
        if (code == (half ? halves : bases)[index]) {
            operation = index;
            return true;
        }
    return false;
}

template <bool Half> u64 ExecuteFPInteger(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<GuestState*>(context_address);
    const u32 word = u32(instruction);
    u32 operation;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeFPInteger(word, Half, operation)) {
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
        const auto result = EvaluateIntegerFP(value, bits, operation, ctx.fpcr);
        std::memcpy(destination + lane * bits / 8, &result.value, bits / 8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.q[rd], destination, sizeof(destination));
    return 0;
}
}  // namespace

bool detail::AdmitFPInteger(u64 target, u32 word) noexcept {
    const bool half = SingleInstructionTarget(target, &ExecuteFPInteger<true>);
    if (!half && !SingleInstructionTarget(target, &ExecuteFPInteger<false>)) return false;
    u32 operation;
    return DecodeFPInteger(word, half, operation) &&
           (half || !(word & (1u << 22)) || (word & NEON_Q));
}
}  // namespace family_fp_integer

u64 NumericInteger(u64 state, u64 word, u64 pc) {
    return family_fp_integer::ExecuteFPInteger<false>(state, word, pc);
}

bool SupportsInteger(u32 word) {
    return family_fp_integer::detail::AdmitFPInteger(
            reinterpret_cast<u64>(&family_fp_integer::ExecuteFPInteger<false>), word);
}

u64 NumericInteger16(u64 state, u64 word, u64 pc) {
    return family_fp_integer::ExecuteFPInteger<true>(state, word, pc);
}

bool SupportsInteger16(u32 word) {
    return family_fp_integer::detail::AdmitFPInteger(
            reinterpret_cast<u64>(&family_fp_integer::ExecuteFPInteger<true>), word);
}

namespace family_fp_fixed_simd {
namespace detail {
bool AdmitFixedSIMD(u64, u32) noexcept;

template <class F> bool SingleInstructionTarget(u64 address, F f) noexcept {
    return address == reinterpret_cast<u64>(f);
}
}  // namespace detail

namespace {
bool DecodeFixedSIMD(u32 word, u32& operation, u32& bits, u32& fbits) {
    switch ((word & ~(NEON_Q | NEONScalar)) & NEONShiftImmediateMask) {
        case NEON_SCVTF_imm:
            operation = 0;
            break;
        case NEON_UCVTF_imm:
            operation = 1;
            break;
        case NEON_FCVTZS_imm:
            operation = 8;
            break;
        case NEON_FCVTZU_imm:
            operation = 9;
            break;
        default:
            return false;
    }
    const u32 immediate = (word >> 16) & 127, immh = immediate >> 3;
    const int highest = (int(std::bit_width(u32(immh))) - 1);
    bits = highest >= 0 ? 8u << highest : 0;
    fbits = 2 * bits - immediate;
    return true;
}

u64 ExecuteFixedSIMD(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<GuestState*>(context_address);
    const u32 word = u32(instruction);
    u32 operation, bits, fbits;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeFixedSIMD(word, operation, bits, fbits)) {
        ctx.pc = guest_pc;
        ctx.helper_status = 1;
        return 0;
    }
    const u32 rn = (word >> 5) & 31, rd = word & 31;
    u8 source[16], destination[16]{};
    std::memcpy(source, &ctx.q[rn], sizeof(source));
    const u32 active = word & NEONScalar ? 1 : (word & NEON_Q ? 128 : 64) / bits;
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        u64 value = 0;
        std::memcpy(&value, source + lane * bits / 8, bits / 8);
        const auto result = EvaluateIntegerFP(value, bits, operation, ctx.fpcr, fbits);
        std::memcpy(destination + lane * bits / 8, &result.value, bits / 8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.q[rd], destination, sizeof(destination));
    return 0;
}
}  // namespace

bool detail::AdmitFixedSIMD(u64 target, u32 word) noexcept {
    u32 operation, bits, fbits;
    return SingleInstructionTarget(target, &ExecuteFixedSIMD) &&
           DecodeFixedSIMD(word, operation, bits, fbits) && bits >= 16 && bits <= 64 && fbits &&
           fbits <= bits && (bits != 64 || (word & NEON_Q));
}
}  // namespace family_fp_fixed_simd

u64 NumericFixed(u64 state, u64 word, u64 pc) {
    return family_fp_fixed_simd::ExecuteFixedSIMD(state, word, pc);
}

bool SupportsFixed(u32 word) {
    return family_fp_fixed_simd::detail::AdmitFixedSIMD(
            reinterpret_cast<u64>(&family_fp_fixed_simd::ExecuteFixedSIMD), word);
}

namespace family_frint_range {
namespace detail {
bool AdmitRangeRound(u64, u32) noexcept;

template <class F> bool SingleInstructionTarget(u64 address, F f) noexcept {
    return address == reinterpret_cast<u64>(f);
}
}  // namespace detail

namespace {
bool DecodeRangeRound(u32 word, u32& operation, u32& integer_bits, bool& scalar) {
    scalar = word & (1u << 28);
    if (scalar) {
        switch (word & (FPDataProcessing1SourceMask & ~u32(FP64))) {
            case FRINT32Z_s:
                operation = FRINTZ_s;
                integer_bits = 32;
                break;
            case FRINT32X_s:
                operation = FRINTX_s;
                integer_bits = 32;
                break;
            case FRINT64Z_s:
                operation = FRINTZ_s;
                integer_bits = 64;
                break;
            case FRINT64X_s:
                operation = FRINTX_s;
                integer_bits = 64;
                break;
            default:
                return false;
        }
    } else {
        switch (word & NEON2RegMiscFPMask) {
            case NEON_FRINT32Z:
                operation = FRINTZ_s;
                integer_bits = 32;
                break;
            case NEON_FRINT32X:
                operation = FRINTX_s;
                integer_bits = 32;
                break;
            case NEON_FRINT64Z:
                operation = FRINTZ_s;
                integer_bits = 64;
                break;
            case NEON_FRINT64X:
                operation = FRINTX_s;
                integer_bits = 64;
                break;
            default:
                return false;
        }
    }
    return true;
}

u64 ExecuteRangeRound(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<GuestState*>(context_address);
    const u32 word = u32(instruction);
    u32 operation, integer_bits;
    bool scalar;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeRangeRound(word, operation, integer_bits, scalar)) {
        ctx.pc = guest_pc;
        ctx.helper_status = 1;
        return 0;
    }
    const u32 bits = word & (1u << 22) ? 64 : 32, rn = (word >> 5) & 31, rd = word & 31;
    u8 source[16], destination[16]{};
    std::memcpy(source, &ctx.q[rn], sizeof(source));
    const u32 active = scalar ? 1 : (word & NEON_Q ? 128 : 64) / bits;
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        u64 value = 0;
        std::memcpy(&value, source + lane * bits / 8, bits / 8);
        const auto result = EvaluateUnaryFP(value, bits, operation, ctx.fpcr, integer_bits);
        std::memcpy(destination + lane * bits / 8, &result.value, bits / 8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.q[rd], destination, sizeof(destination));
    return 0;
}
}  // namespace

bool detail::AdmitRangeRound(u64 target, u32 word) noexcept {
    u32 operation, integer_bits;
    bool scalar;
    return SingleInstructionTarget(target, &ExecuteRangeRound) &&
           DecodeRangeRound(word, operation, integer_bits, scalar) &&
           (scalar || !(word & (1u << 22)) || (word & NEON_Q));
}
}  // namespace family_frint_range

u64 NumericRangeRound(u64 state, u64 word, u64 pc) {
    return family_frint_range::ExecuteRangeRound(state, word, pc);
}

bool SupportsRangeRound(u32 word) {
    return family_frint_range::detail::AdmitRangeRound(
            reinterpret_cast<u64>(&family_frint_range::ExecuteRangeRound), word);
}

namespace family_fp_precision {
namespace detail {
bool AdmitPrecision(u64, u32) noexcept;

template <class F> bool SingleInstructionTarget(u64 address, F f) noexcept {
    return address == reinterpret_cast<u64>(f);
}
}  // namespace detail

namespace {
enum class Operation { Widen, Narrow, Odd };

bool DecodePrecision(u32 word, Operation& operation) {
    switch (word & NEON2RegMiscFPMask) {
        case NEON_FCVTL:
            operation = Operation::Widen;
            break;
        case NEON_FCVTN:
            operation = Operation::Narrow;
            break;
        case NEON_FCVTXN:
            operation = Operation::Odd;
            break;
        default:
            return false;
    }
    return true;
}

u64 ExecutePrecision(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<GuestState*>(context_address);
    const u32 word = u32(instruction);
    Operation operation;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodePrecision(word, operation)) {
        ctx.pc = guest_pc;
        ctx.helper_status = 1;
        return 0;
    }
    const u32 small = word & (1u << 22) ? 32 : 16;
    const bool widen = operation == Operation::Widen, upper = word & NEON_Q;
    const u32 source_bits = widen ? small : small * 2, destination_bits = widen ? small * 2 : small;
    const u32 rn = (word >> 5) & 31, rd = word & 31;
    u8 source[16], destination[16]{};
    std::memcpy(source, &ctx.q[rn], sizeof(source));
    // Narrow2 preserves the old lower half, including when Rd aliases Rn.
    if (!widen && upper) std::memcpy(destination, &ctx.q[rd], 8);
    const u32 active = 64 / small, source_offset = widen && upper ? 8 : 0,
              destination_offset = !widen && upper ? 8 : 0;
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        u64 value = 0;
        std::memcpy(&value, source + source_offset + lane * source_bits / 8, source_bits / 8);
        const auto result = EvaluatePrecisionFP(
                value, source_bits, destination_bits, operation == Operation::Odd, ctx.fpcr);
        std::memcpy(destination + destination_offset + lane * destination_bits / 8,
                    &result.value,
                    destination_bits / 8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.q[rd], destination, sizeof(destination));
    return 0;
}
}  // namespace

bool detail::AdmitPrecision(u64 target, u32 word) noexcept {
    Operation operation;
    return SingleInstructionTarget(target, &ExecutePrecision) && DecodePrecision(word, operation) &&
           (operation != Operation::Odd || (word & (1u << 22)));
}
}  // namespace family_fp_precision

u64 NumericPrecision(u64 state, u64 word, u64 pc) {
    return family_fp_precision::ExecutePrecision(state, word, pc);
}

bool SupportsPrecision(u32 word) {
    return family_fp_precision::detail::AdmitPrecision(
            reinterpret_cast<u64>(&family_fp_precision::ExecutePrecision), word);
}
}  // namespace swift::runtime::arm64
