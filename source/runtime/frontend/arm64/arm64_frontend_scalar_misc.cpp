#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

void A64Decoder::VisitNEONScalar2RegMisc(const Instruction* instr) {
    if (TryNEONMixedSaturating(instr,true)) return;
    if (TryNEONFPEstimate(instr, false)) return;
    if (TryNEONFPInteger(instr, false)) return;
    if (TryNEONFPCompare(instr, false, true)) return;
    const auto op = instr->Mask(NEONScalar2RegMiscMask);
    const bool narrow = op == NEON_SQXTN_scalar || op == NEON_UQXTN_scalar || op == NEON_SQXTUN_scalar;
    const bool absolute = op == NEON_ABS_scalar || op == NEON_SQABS_scalar;
    const bool negate = op == NEON_NEG_scalar || op == NEON_SQNEG_scalar;
    const bool saturate_unary = op == NEON_SQABS_scalar || op == NEON_SQNEG_scalar;
    const bool compare = op == NEON_CMGT_zero_scalar || op == NEON_CMGE_zero_scalar ||
                         op == NEON_CMEQ_zero_scalar || op == NEON_CMLT_zero_scalar || op == NEON_CMLE_zero_scalar;
    if (!narrow && !absolute && !negate && !compare) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    const u32 bits = 8u << instr->GetNEONSize();
    if ((narrow && bits == 64) || (!narrow && !saturate_unary && bits != 64)) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const u32 source_bits = narrow ? bits * 2 : bits;
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    const u64 source_mask = source_bits == 64 ? ~u64{0} : (u64{1} << source_bits) - 1;
    auto source = __ And<ir::U64>(ReadVHalf(instr->GetRn(), false), SingleOperand(ir::Imm{source_mask})).SetType(ir::ValueType::U64);
    auto zero = ImmValue(0, ir::ValueType::U64);
    ir::Value result, saturated;
    if (compare) {
        auto negative = __ TestBit(source, ir::Imm{63u}).SetType(ir::ValueType::U8);
        auto is_zero = __ TestZero(source).SetType(ir::ValueType::U8);
        ir::Value condition;
        if (op == NEON_CMEQ_zero_scalar) condition = is_zero;
        else if (op == NEON_CMLT_zero_scalar) condition = negative;
        else if (op == NEON_CMGE_zero_scalar)
            condition = __ Xor<ir::U8>(negative, SingleOperand(ir::Imm{u8{1}})).SetType(ir::ValueType::U8);
        else {
            condition = __ Or<ir::U8>(negative, SingleOperand(is_zero)).SetType(ir::ValueType::U8);
            if (op == NEON_CMGT_zero_scalar)
                condition = __ Xor<ir::U8>(condition, SingleOperand(ir::Imm{u8{1}})).SetType(ir::ValueType::U8);
        }
        result = __ Select(condition, ImmValue(~u64{0}, ir::ValueType::U64), zero).SetType(ir::ValueType::U64);
    } else if (narrow) {
        const bool signed_source = op != NEON_UQXTN_scalar;
        if (signed_source) source = SignExtendValue(source, source_bits);
        auto trimmed = __ And<ir::U64>(source, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
        if (op == NEON_SQXTN_scalar) trimmed = SignExtendValue(trimmed, bits);
        auto changed = __ Xor<ir::U64>(source, SingleOperand(trimmed)).SetType(ir::ValueType::U64);
        saturated = __ TestNotZero(changed).SetType(ir::ValueType::U8);
        auto limit = ImmValue(mask, ir::ValueType::U64);
        if (signed_source) {
            auto negative = __ TestBit(source, ir::Imm{63u}).SetType(ir::ValueType::U8);
            limit = __ Select(negative,
                ImmValue(op == NEON_SQXTN_scalar ? u64{1} << (bits - 1) : 0, ir::ValueType::U64),
                ImmValue(op == NEON_SQXTN_scalar ? (u64{1} << (bits - 1)) - 1 : mask, ir::ValueType::U64)).SetType(ir::ValueType::U64);
        }
        result = __ Select(saturated, limit, source).SetType(ir::ValueType::U64);
    } else {
        auto negative = __ TestBit(source, ir::Imm{bits - 1}).SetType(ir::ValueType::U8);
        auto opposite = __ Sub<ir::U64>(zero, SingleOperand(source)).SetType(ir::ValueType::U64);
        result = absolute ? __ Select(negative, opposite, source).SetType(ir::ValueType::U64) : opposite;
        if (saturate_unary) {
            auto changed = __ Xor<ir::U64>(source, SingleOperand(ir::Imm{u64{1} << (bits - 1)})).SetType(ir::ValueType::U64);
            saturated = __ TestZero(changed).SetType(ir::ValueType::U8);
            result = __ Select(saturated, ImmValue((u64{1} << (bits - 1)) - 1, ir::ValueType::U64), result).SetType(ir::ValueType::U64);
        }
    }
    if (narrow || saturate_unary) {
        ir::Uniform status{u32(offsetof(ThreadContext64, fpsr)), ir::ValueType::U32};
        auto old = __ LoadUniform<ir::U32>(status);
        auto qc = __ Select(saturated, ImmValue(1u << 27, ir::ValueType::U32), ImmValue(0, ir::ValueType::U32)).SetType(ir::ValueType::U32);
        __ StoreUniform(status, __ Or<ir::U32>(old, SingleOperand(qc)).SetType(ir::ValueType::U32));
    }
    result = __ And<ir::U64>(result, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
    WriteVHalves(instr->GetRd(), result, zero);
}

void A64Decoder::VisitNEONScalarPairwise(const Instruction* instr) {
    if (TryScalarPairwiseFP(instr)) return;
    // The generic VIXL visitor admits reserved ADDP size fields, including
    // size=01 whose bit 23 differs from the allocated ADDP opcode constant.
    const auto op = instr->GetInstructionBits() & ~(3u << 22);
    const bool integer_add = (op & 0xFFFFFC00u) == (u32(NEON_ADDP_scalar) & ~(3u << 22));
    if (!integer_add) {
        // FP pairwise operations require guest FPCR/FPSR and ARM NaN handling.
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    if (instr->GetNEONSize() != 3) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    auto low = ReadVHalf(instr->GetRn(), false), high = ReadVHalf(instr->GetRn(), true);
    auto result = __ Add<ir::U64>(low, SingleOperand(high)).SetType(ir::ValueType::U64);
    WriteVHalves(instr->GetRd(), result, ImmValue(0, ir::ValueType::U64));
}

#undef __
}  // namespace swift::arm64
