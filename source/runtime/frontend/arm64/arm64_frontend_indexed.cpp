#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

void A64Decoder::VisitNEONByIndexedElement(const Instruction* instr) {
    if (TryNEONVectorDoublingLong(instr, true)) return;
    if (TryNEONIndexedFP(instr)) return;
    if (TryNEONDotProduct(instr, true)) return;
    if (TryNEONSaturatingMultiplyHigh(instr, false, true, false)) return;
    const auto op = instr->Mask(NEONByIndexedElementMask);
    const bool narrow = op == NEON_MUL_byelement || op == NEON_MLA_byelement ||
                        op == NEON_MLS_byelement;
    const bool widen = op == NEON_SMULL_byelement || op == NEON_UMULL_byelement ||
                       op == NEON_SMLAL_byelement || op == NEON_UMLAL_byelement ||
                       op == NEON_SMLSL_byelement || op == NEON_UMLSL_byelement;
    if (!narrow && !widen) { Interrupt(InterruptReason::FALLBACK, current_pc); return; }
    const u32 bits = 8u << instr->GetNEONSize();
    if (bits != 16 && bits != 32) { Interrupt(InterruptReason::ILL_CODE, current_pc); return; }
    const bool upper = instr->GetNEONQ();
    const u32 rm = bits == 16 ? instr->GetRmLow16() : instr->GetRm();
    const u32 index = bits == 16 ? (instr->GetNEONH() << 2) | (instr->GetNEONL() << 1) | instr->GetNEONM()
                                : (instr->GetNEONH() << 1) | instr->GetNEONL();
    const u64 mask = (u64{1} << bits) - 1;
    auto element = ReadVHalf(rm, index * bits >= 64);
    element = __ LsrImm(element, ir::Imm{index * bits % 64}).SetType(ir::ValueType::U64);
    element = __ And(element, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
    if (narrow) {
        auto broadcast = element;
        for (u32 shift = bits; shift < 64; shift *= 2) {
            auto copy = __ LslImm(broadcast, ir::Imm{shift}).SetType(ir::ValueType::U64);
            broadcast = __ Or(broadcast, SingleOperand(copy)).SetType(ir::ValueType::U64);
        }
        auto rhs = __ VecDup64(broadcast).SetType(ir::ValueType::V128);
        auto lhs = ReadVRegister(instr->GetRn(), ir::ValueType::V128);
        auto result = __ VecMul(lhs, rhs, ir::Imm{bits}).SetType(ir::ValueType::V128);
        if (op != NEON_MUL_byelement) {
            auto old = ReadVRegister(instr->GetRd(), ir::ValueType::V128);
            result = (op == NEON_MLA_byelement ? __ VecAdd(old, result, ir::Imm{bits})
                                               : __ VecSub(old, result, ir::Imm{bits})).SetType(ir::ValueType::V128);
        }
        if (!upper) result = __ BitCast(result).SetType(ir::ValueType::V64);
        WriteVRegister(instr->GetRd(), result);
        return;
    }
    const bool sign = op == NEON_SMULL_byelement || op == NEON_SMLAL_byelement || op == NEON_SMLSL_byelement;
    const bool accumulate = op != NEON_SMULL_byelement && op != NEON_UMULL_byelement;
    const bool subtract = op == NEON_SMLSL_byelement || op == NEON_UMLSL_byelement;
    const u32 out_bits = bits * 2;
    const u64 out_mask = out_bits == 64 ? ~u64{0} : (u64{1} << out_bits) - 1;
    auto source = ReadVHalf(instr->GetRn(), upper);
    // Snapshot every input, including the accumulator, before writing an aliased Rd.
    auto old_low = accumulate ? ReadVHalf(instr->GetRd(), false) : ImmValue(0, ir::ValueType::U64);
    auto old_high = accumulate ? ReadVHalf(instr->GetRd(), true) : old_low;
    if (sign) element = SignExtendValue(element, bits);
    auto low = ImmValue(0, ir::ValueType::U64), high = low;
    for (u32 lane = 0; lane < 64 / bits; ++lane) {
        auto a = __ LsrImm(source, ir::Imm{lane * bits}).SetType(ir::ValueType::U64);
        a = __ And(a, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
        if (sign) a = SignExtendValue(a, bits);
        auto result = __ Mul(a, SingleOperand(element)).SetType(ir::ValueType::U64);
        const u32 position = lane * out_bits;
        if (accumulate) {
            auto old = __ LsrImm(position < 64 ? old_low : old_high, ir::Imm{position % 64})
                              .SetType(ir::ValueType::U64);
            result = (subtract ? __ Sub(old, SingleOperand(result))
                               : __ Add(old, SingleOperand(result))).SetType(ir::ValueType::U64);
        }
        result = __ And(result, SingleOperand(ir::Imm{out_mask})).SetType(ir::ValueType::U64);
        result = __ LslImm(result, ir::Imm{position % 64}).SetType(ir::ValueType::U64);
        auto& half = position < 64 ? low : high;
        half = __ Or(half, SingleOperand(result)).SetType(ir::ValueType::U64);
    }
    WriteVHalves(instr->GetRd(), low, high);
}
}  // namespace swift::arm64
