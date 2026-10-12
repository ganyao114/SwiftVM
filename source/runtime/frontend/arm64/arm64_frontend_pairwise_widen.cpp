#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

bool A64Decoder::TryNEONPairwiseWiden(const Instruction* instr) {
    const u32 op = instr->Mask(NEON2RegMiscMask);
    const bool shift = op == NEON_SHLL;
    const bool sign = op == NEON_SADDLP || op == NEON_SADALP;
    const bool accumulate = op == NEON_SADALP || op == NEON_UADALP;
    if (!shift && !sign && !accumulate && op != NEON_UADDLP) return false;
    const u32 bits = 8u<<instr->GetNEONSize();const bool q = instr->GetNEONQ();
    if (bits==64) { Interrupt(InterruptReason::ILL_CODE,current_pc);return true; }
    const u32 output_bits = bits*2;
    if (shift) {
        // SHLL's fixed shift equals the input width; Q selects the low/high
        // input half, and the output always occupies all 128 bits.
        auto input = ReadVRegister(instr->GetRn(),ir::ValueType::V128);
        auto zero = __ VecSharedZero().SetType(ir::ValueType::V128);
        auto widened = __ VecZip(input,zero,ir::Imm{bits},ir::Imm{q}).SetType(ir::ValueType::V128);
        auto result = __ VecShiftLeftImm(widened,ir::Imm{bits},ir::Imm{output_bits}).SetType(ir::ValueType::V128);
        WriteVRegister(instr->GetRd(),result);return true;
    }
    auto input = ReadVRegister(instr->GetRn(),ir::ValueType::V128);
    auto zero = __ VecSharedZero().SetType(ir::ValueType::V128);
    auto widen = [&](bool odd) {
        // De-interleaving places all even/odd source lanes in the low half.
        // ZIP with zero then widens those contiguous lanes to the output width.
        auto selected = __ VecUnzip(input,zero,ir::Imm{bits},ir::Imm{odd}).SetType(ir::ValueType::V128);
        auto result = __ VecZip(selected,zero,ir::Imm{bits},ir::Imm{false}).SetType(ir::ValueType::V128);
        if (sign) {
            result = __ VecShiftLeftImm(result,ir::Imm{bits},ir::Imm{output_bits}).SetType(ir::ValueType::V128);
            result = __ VecShiftRightArithmeticImm(result,ir::Imm{bits},ir::Imm{output_bits}).SetType(ir::ValueType::V128);
        }
        return result;
    };
    auto even = widen(false),odd = widen(true);
    auto result = __ VecAdd(even,odd,ir::Imm{output_bits}).SetType(ir::ValueType::V128);
    if (accumulate) {
        auto old = ReadVRegister(instr->GetRd(),ir::ValueType::V128);
        result = __ VecAdd(old,result,ir::Imm{output_bits}).SetType(ir::ValueType::V128);
    }
    if (!q) result = __ BitCast(result).SetType(ir::ValueType::V64);
    WriteVRegister(instr->GetRd(),result);return true;
}

#undef __
}  // namespace swift::arm64
