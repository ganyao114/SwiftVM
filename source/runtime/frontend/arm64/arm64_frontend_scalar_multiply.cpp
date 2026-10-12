#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

ir::Value A64Decoder::ScalarDoublingMultiply(ir::Value lhs, ir::Value rhs,
                                            ir::Value accumulator, u32 bits,
                                            bool accumulate, bool subtract) {
    const u32 out_bits = bits * 2;
    const u64 source_mask = (u64{1} << bits) - 1;
    const u64 out_mask = out_bits == 64 ? ~u64{0} : (u64{1} << out_bits) - 1;
    const u64 out_sign = u64{1} << (out_bits - 1);
    lhs = __ And<ir::U64>(lhs, SingleOperand(ir::Imm{source_mask})).SetType(ir::ValueType::U64);
    rhs = __ And<ir::U64>(rhs, SingleOperand(ir::Imm{source_mask})).SetType(ir::ValueType::U64);
    auto lhs_min = __ TestZero(__ Xor<ir::U64>(lhs, SingleOperand(ir::Imm{u64{1} << (bits - 1)}))
                                  .SetType(ir::ValueType::U64)).SetType(ir::ValueType::U8);
    auto rhs_min = __ TestZero(__ Xor<ir::U64>(rhs, SingleOperand(ir::Imm{u64{1} << (bits - 1)}))
                                  .SetType(ir::ValueType::U64)).SetType(ir::ValueType::U8);
    // The only doubled product outside the signed double-width range is
    // MIN * MIN. Saturate it before adding/subtracting the accumulator: a
    // negative accumulator must not cancel this first saturation or its QC.
    auto saturated = __ And<ir::U8>(lhs_min, SingleOperand(rhs_min)).SetType(ir::ValueType::U8);
    lhs = SignExtendValue(lhs, bits);
    rhs = SignExtendValue(rhs, bits);
    auto product = __ Mul<ir::U64>(lhs, SingleOperand(rhs)).SetType(ir::ValueType::U64);
    product = __ LslImm<ir::U64>(product, ir::Imm{u32{1}}).SetType(ir::ValueType::U64);
    product = __ Select(saturated, ImmValue(out_sign - 1, ir::ValueType::U64), product)
                    .SetType(ir::ValueType::U64);
    ir::Value result = product;
    if (accumulate) {
        accumulator = SignExtendValue(accumulator, out_bits);
        product = SignExtendValue(product, out_bits);
        result = (subtract ? __ Sub<ir::U64>(accumulator, SingleOperand(product))
                           : __ Add<ir::U64>(accumulator, SingleOperand(product))).SetType(ir::ValueType::U64);
        // For addition overflow requires equal input signs and a changed
        // result sign; subtraction requires different input signs instead.
        auto input_signs = __ Xor<ir::U64>(accumulator, SingleOperand(product)).SetType(ir::ValueType::U64);
        if (!subtract)
            input_signs = __ Xor<ir::U64>(input_signs, SingleOperand(ir::Imm{~u64{0}})).SetType(ir::ValueType::U64);
        auto result_sign = __ Xor<ir::U64>(accumulator, SingleOperand(result)).SetType(ir::ValueType::U64);
        auto overflow_bits = __ And<ir::U64>(input_signs, SingleOperand(result_sign)).SetType(ir::ValueType::U64);
        auto overflow = __ TestBit(overflow_bits, ir::Imm{out_bits - 1}).SetType(ir::ValueType::U8);
        auto negative = __ TestBit(accumulator, ir::Imm{out_bits - 1}).SetType(ir::ValueType::U8);
        auto limit = __ Select(negative, ImmValue(out_sign, ir::ValueType::U64),
                              ImmValue(out_sign - 1, ir::ValueType::U64)).SetType(ir::ValueType::U64);
        result = __ Select(overflow, limit, result).SetType(ir::ValueType::U64);
        saturated = __ Or<ir::U8>(saturated, SingleOperand(overflow)).SetType(ir::ValueType::U8);
    }
    const ir::Uniform status{u32(offsetof(ThreadContext64, fpsr)), ir::ValueType::U32};
    auto previous = __ LoadUniform<ir::U32>(status);
    auto qc = __ Select(saturated, ImmValue(1u << 27, ir::ValueType::U32),
                        ImmValue(0, ir::ValueType::U32)).SetType(ir::ValueType::U32);
    __ StoreUniform(status, __ Or<ir::U32>(previous, SingleOperand(qc)).SetType(ir::ValueType::U32));
    return __ And<ir::U64>(result, SingleOperand(ir::Imm{out_mask})).SetType(ir::ValueType::U64);
}

void A64Decoder::VisitNEONScalar3Diff(const Instruction* instr) {
    const auto op = instr->Mask(NEONScalar3DiffMask);
    if (op != NEON_SQDMULL_scalar && op != NEON_SQDMLAL_scalar && op != NEON_SQDMLSL_scalar) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    const u32 bits = 8u << instr->GetNEONSize();
    if (bits != 16 && bits != 32) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const bool accumulate = op != NEON_SQDMULL_scalar;
    // Snapshot every source before writing an aliased scalar destination.
    auto lhs = ReadVHalf(instr->GetRn(), false);
    auto rhs = ReadVHalf(instr->GetRm(), false);
    auto zero = ImmValue(0, ir::ValueType::U64);
    auto old = accumulate ? ReadVHalf(instr->GetRd(), false) : zero;
    auto result = ScalarDoublingMultiply(lhs, rhs, old, bits, accumulate, op == NEON_SQDMLSL_scalar);
    WriteVHalves(instr->GetRd(), result, zero);
}

void A64Decoder::VisitNEONScalarByIndexedElement(const Instruction* instr) {
    if (TryNEONIndexedFP(instr)) return;
    if (TryNEONSaturatingMultiplyHigh(instr, true, true, false)) return;
    const auto op = instr->Mask(NEONScalarByIndexedElementMask);
    if (op != NEON_SQDMULL_byelement_scalar && op != NEON_SQDMLAL_byelement_scalar &&
        op != NEON_SQDMLSL_byelement_scalar) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    const u32 bits = 8u << instr->GetNEONSize();
    if (bits != 16 && bits != 32) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const u32 rm = bits == 16 ? instr->GetRmLow16() : instr->GetRm();
    const u32 index = bits == 16 ? (instr->GetNEONH() << 2) | (instr->GetNEONL() << 1) | instr->GetNEONM()
                                : (instr->GetNEONH() << 1) | instr->GetNEONL();
    const bool accumulate = op != NEON_SQDMULL_byelement_scalar;
    auto lhs = ReadVHalf(instr->GetRn(), false);
    auto rhs = ReadVHalf(rm, index * bits >= 64);
    rhs = __ LsrImm<ir::U64>(rhs, ir::Imm{index * bits % 64}).SetType(ir::ValueType::U64);
    auto zero = ImmValue(0, ir::ValueType::U64);
    auto old = accumulate ? ReadVHalf(instr->GetRd(), false) : zero;
    auto result = ScalarDoublingMultiply(lhs, rhs, old, bits, accumulate, op == NEON_SQDMLSL_byelement_scalar);
    WriteVHalves(instr->GetRd(), result, zero);
}

#undef __
}  // namespace swift::arm64
