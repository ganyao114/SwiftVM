#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

bool A64Decoder::TryNEONVectorDoublingLong(const Instruction* instr, bool indexed) {
    const u32 mask = indexed ? u32(NEONByIndexedElementMask) : u32(NEON3DifferentMask);
    const u32 op = instr->Mask(mask) & ~u32(NEON_Q);
    const u32 multiply = indexed ? u32(NEON_SQDMULL_byelement) : u32(NEON_SQDMULL);
    const u32 add = indexed ? u32(NEON_SQDMLAL_byelement) : u32(NEON_SQDMLAL);
    const u32 subtract = indexed ? u32(NEON_SQDMLSL_byelement) : u32(NEON_SQDMLSL);
    if (op != multiply && op != add && op != subtract) return false;
    const u32 bits = 8u << instr->GetNEONSize();
    if (bits != 16 && bits != 32) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    const bool upper = instr->GetNEONQ(), accumulate = op != multiply;
    // Q selects the source half; every form produces the complete widened
    // 128-bit destination. Snapshot it along with all sources before writes.
    auto lhs = ReadVHalf(instr->GetRn(), upper);
    ir::Value rhs;
    if (indexed) {
        const u32 rm = bits == 16 ? instr->GetRmLow16() : instr->GetRm();
        const u32 index = bits == 16 ? (instr->GetNEONH() << 2) | (instr->GetNEONL() << 1) | instr->GetNEONM() :
                                     (instr->GetNEONH() << 1) | instr->GetNEONL();
        rhs = ReadVHalf(rm, index * bits >= 64);
        if (index * bits % 64)
            rhs = __ LsrImm<ir::U64>(rhs, ir::Imm{index * bits % 64}).SetType(ir::ValueType::U64);
    } else {
        rhs = ReadVHalf(instr->GetRm(), upper);
    }
    auto accumulator_low = accumulate ? ReadVHalf(instr->GetRd(), false) : ImmValue(0, ir::ValueType::U64);
    auto accumulator_high = accumulate ? ReadVHalf(instr->GetRd(), true) : accumulator_low;
    const u32 out_bits = bits * 2;
    auto low = ImmValue(0, ir::ValueType::U64), high = low;
    for (u32 lane = 0; lane < 64 / bits; ++lane) {
        auto left = lane ? __ LsrImm<ir::U64>(lhs, ir::Imm{lane * bits}).SetType(ir::ValueType::U64) : lhs;
        auto right = !indexed && lane ? __ LsrImm<ir::U64>(rhs, ir::Imm{lane * bits}).SetType(ir::ValueType::U64) : rhs;
        const u32 position = lane * out_bits;
        auto old = position < 64 ? accumulator_low : accumulator_high;
        if (position % 64)
            old = __ LsrImm<ir::U64>(old, ir::Imm{position % 64}).SetType(ir::ValueType::U64);
        // This helper clamps 2*a*b before accumulator +/- product and ORs
        // either saturation into QC, including a later cancellation.
        auto result = ScalarDoublingMultiply(left, right, old, bits, accumulate, op == subtract);
        if (position % 64)
            result = __ LslImm<ir::U64>(result, ir::Imm{position % 64}).SetType(ir::ValueType::U64);
        auto& destination = position < 64 ? low : high;
        destination = __ Or<ir::U64>(destination, SingleOperand(result)).SetType(ir::ValueType::U64);
    }
    WriteVHalves(instr->GetRd(), low, high);
    return true;
}

#undef __
}  // namespace swift::arm64
