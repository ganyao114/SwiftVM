#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

void A64Decoder::VisitNEONScalarShiftImmediate(const Instruction* instr) {
    if (TryNEONFPFixedSIMD(instr)) return;
    const auto op = instr->Mask(NEONScalarShiftImmediateMask);
    const bool narrow = op == NEON_UQSHRN_scalar || op == NEON_UQRSHRN_scalar ||
        op == NEON_SQSHRN_scalar || op == NEON_SQRSHRN_scalar ||
        op == NEON_SQSHRUN_scalar || op == NEON_SQRSHRUN_scalar;
    const bool saturating_left = op == NEON_SQSHL_imm_scalar || op == NEON_UQSHL_imm_scalar || op == NEON_SQSHLU_scalar;
    const bool ordinary_left = op == NEON_SHL_scalar || op == NEON_SLI_scalar;
    const bool insert = op == NEON_SLI_scalar || op == NEON_SRI_scalar;
    const bool signed_right = op == NEON_SSHR_scalar || op == NEON_SRSHR_scalar ||
        op == NEON_SSRA_scalar || op == NEON_SRSRA_scalar ||
        op == NEON_SQSHRN_scalar || op == NEON_SQRSHRN_scalar ||
        op == NEON_SQSHRUN_scalar || op == NEON_SQRSHRUN_scalar;
    const bool rounded = op == NEON_SRSHR_scalar || op == NEON_URSHR_scalar ||
        op == NEON_SRSRA_scalar || op == NEON_URSRA_scalar ||
        op == NEON_UQRSHRN_scalar || op == NEON_SQRSHRN_scalar || op == NEON_SQRSHRUN_scalar;
    const bool accumulate = op == NEON_SSRA_scalar || op == NEON_USRA_scalar ||
        op == NEON_SRSRA_scalar || op == NEON_URSRA_scalar;
    if (!narrow && !saturating_left && !ordinary_left && !insert && !signed_right &&
        !rounded && !accumulate && op != NEON_USHR_scalar) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    const int highest = vixl::HighestSetBitPosition(instr->GetImmNEONImmh());
    if (highest < 0 || highest > 3 || (narrow && highest > 2) ||
        (!narrow && !saturating_left && highest != 3)) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const u32 bits = 8u << highest, source_bits = narrow ? bits * 2 : bits;
    const bool left = saturating_left || ordinary_left;
    const u32 encoded = instr->GetImmNEONImmhImmb();
    const u32 shift = left ? encoded - bits : 2 * bits - encoded;
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    const u64 source_mask = source_bits == 64 ? ~u64{0} : (u64{1} << source_bits) - 1;
    auto input = __ And<ir::U64>(ReadVHalf(instr->GetRn(), false), SingleOperand(ir::Imm{source_mask})).SetType(ir::ValueType::U64);
    auto old = (insert || accumulate) ? ReadVHalf(instr->GetRd(), false) : ImmValue(0, ir::ValueType::U64);
    auto zero = ImmValue(0, ir::ValueType::U64);
    ir::Value result, saturated;
    if (left) {
        result = __ LslImm<ir::U64>(input, ir::Imm{shift}).SetType(ir::ValueType::U64);
        result = __ And<ir::U64>(result, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
        if (saturating_left) {
            const bool signed_result = op == NEON_SQSHL_imm_scalar;
            auto original = signed_result ? SignExtendValue(input, bits) : input;
            auto shifted = signed_result ? SignExtendValue(result, bits) : result;
            auto recovered = signed_result
                ? __ AsrImm<ir::U64>(shifted, ir::Imm{shift}).SetType(ir::ValueType::U64)
                : __ LsrImm<ir::U64>(shifted, ir::Imm{shift}).SetType(ir::ValueType::U64);
            auto changed = __ Xor<ir::U64>(original, SingleOperand(recovered)).SetType(ir::ValueType::U64);
            saturated = __ TestNotZero(changed).SetType(ir::ValueType::U8);
            auto negative = __ TestBit(input, ir::Imm{bits - 1}).SetType(ir::ValueType::U8);
            ir::Value limit = ImmValue(mask, ir::ValueType::U64);
            if (signed_result) {
                limit = __ Select(negative, ImmValue(u64{1} << (bits - 1), ir::ValueType::U64),
                    ImmValue((u64{1} << (bits - 1)) - 1, ir::ValueType::U64)).SetType(ir::ValueType::U64);
            } else if (op == NEON_SQSHLU_scalar) {
                // Negative signed inputs saturate to zero, including shift=0.
                saturated = __ Or<ir::U8>(saturated, SingleOperand(negative)).SetType(ir::ValueType::U8);
                limit = __ Select(negative, zero, limit).SetType(ir::ValueType::U64);
            }
            result = __ Select(saturated, limit, result).SetType(ir::ValueType::U64);
        } else if (insert) {
            const u64 keep = shift == 0 ? 0 : (u64{1} << shift) - 1;
            auto preserved = __ And<ir::U64>(old, SingleOperand(ir::Imm{keep})).SetType(ir::ValueType::U64);
            result = __ Or<ir::U64>(preserved, SingleOperand(result)).SetType(ir::ValueType::U64);
        }
    } else {
        auto extended = signed_right ? SignExtendValue(input, source_bits) : input;
        if (signed_right) {
            // IR/x64 immediates mask a shift of 64; explicitly use the sign fill.
            result = __ AsrImm<ir::U64>(extended, ir::Imm{shift == 64 ? 63u : shift}).SetType(ir::ValueType::U64);
        } else if (shift == 64) result = zero;
        else result = __ LsrImm<ir::U64>(extended, ir::Imm{shift}).SetType(ir::ValueType::U64);
        if (rounded) {
            auto round = __ LsrImm<ir::U64>(input, ir::Imm{shift - 1}).SetType(ir::ValueType::U64);
            round = __ And<ir::U64>(round, SingleOperand(ir::Imm{u64{1}})).SetType(ir::ValueType::U64);
            result = __ Add<ir::U64>(result, SingleOperand(round)).SetType(ir::ValueType::U64);
        }
        if (narrow) {
            const bool signed_result = op == NEON_SQSHRN_scalar || op == NEON_SQRSHRN_scalar;
            auto trimmed = __ And<ir::U64>(result, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
            if (signed_result) trimmed = SignExtendValue(trimmed, bits);
            auto changed = __ Xor<ir::U64>(result, SingleOperand(trimmed)).SetType(ir::ValueType::U64);
            saturated = __ TestNotZero(changed).SetType(ir::ValueType::U8);
            auto limit = ImmValue(mask, ir::ValueType::U64);
            if (signed_right) {
                auto negative = __ TestBit(result, ir::Imm{63u}).SetType(ir::ValueType::U8);
                limit = __ Select(negative,
                    ImmValue(signed_result ? u64{1} << (bits - 1) : 0, ir::ValueType::U64),
                    ImmValue(signed_result ? (u64{1} << (bits - 1)) - 1 : mask, ir::ValueType::U64)).SetType(ir::ValueType::U64);
            }
            result = __ Select(saturated, limit, result).SetType(ir::ValueType::U64);
        } else if (accumulate) {
            result = __ Add<ir::U64>(old, SingleOperand(result)).SetType(ir::ValueType::U64);
        } else if (insert) {
            const u64 keep = shift == 64 ? ~u64{0} : mask ^ (mask >> shift);
            auto preserved = __ And<ir::U64>(old, SingleOperand(ir::Imm{keep})).SetType(ir::ValueType::U64);
            result = __ Or<ir::U64>(preserved, SingleOperand(result)).SetType(ir::ValueType::U64);
        }
    }
    if (narrow || saturating_left) {
        ir::Uniform status{u32(offsetof(ThreadContext64, fpsr)), ir::ValueType::U32};
        auto previous = __ LoadUniform<ir::U32>(status);
        auto qc = __ Select(saturated, ImmValue(1u << 27, ir::ValueType::U32), ImmValue(0, ir::ValueType::U32)).SetType(ir::ValueType::U32);
        __ StoreUniform(status, __ Or<ir::U32>(previous, SingleOperand(qc)).SetType(ir::ValueType::U32));
    }
    result = __ And<ir::U64>(result, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
    WriteVHalves(instr->GetRd(), result, zero);
}

#undef __
}  // namespace swift::arm64
