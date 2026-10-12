#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

bool A64Decoder::TryNEONHighNarrow(const Instruction* instr) {
    const u32 op = instr->Mask(NEON3DifferentMask) & ~u32(NEON_Q);
    const bool add = op == NEON_ADDHN || op == NEON_RADDHN;
    const bool subtract = op == NEON_SUBHN || op == NEON_RSUBHN;
    if (!add && !subtract) return false;
    const u32 size = instr->GetNEONSize();
    if (size == 3) { Interrupt(InterruptReason::ILL_CODE,current_pc);return true; }
    const u32 output_bits = 8u << size, source_bits = 2 * output_bits;
    const bool upper = instr->GetNEONQ();
    // Snapshot Rd before any write: HN2 preserves its low half even when aliased.
    auto old_low = upper ? ReadVHalf(instr->GetRd(),false) : ImmValue(0,ir::ValueType::U64);
    auto left = ReadVRegister(instr->GetRn(),ir::ValueType::V128);
    auto right = ReadVRegister(instr->GetRm(),ir::ValueType::V128);
    auto result = (add ? __ VecAdd(left,right,ir::Imm{source_bits})
                       : __ VecSub(left,right,ir::Imm{source_bits})).SetType(ir::ValueType::V128);
    if (op == NEON_RADDHN || op == NEON_RSUBHN) {
        u64 rounding = 0;
        for (u32 lane = 0;lane < 64/source_bits;++lane)
            rounding |= (u64{1} << (output_bits-1)) << (lane*source_bits);
        auto increment = __ VecLoadConst(ir::Imm{rounding},ir::Imm{rounding}).SetType(ir::ValueType::V128);
        result = __ VecAdd(result,increment,ir::Imm{source_bits}).SetType(ir::ValueType::V128);
    }
    // Odd half-sized elements are the high bits of each full-width sum.
    result = __ VecUnzip(result,result,ir::Imm{output_bits},ir::Imm{true}).SetType(ir::ValueType::V128);
    auto packed = __ BitCast(result).SetType(ir::ValueType::U64);
    WriteVHalves(instr->GetRd(),upper ? old_low : packed,
                 upper ? packed : ImmValue(0,ir::ValueType::U64));
    return true;
}

#undef __
}  // namespace swift::arm64
