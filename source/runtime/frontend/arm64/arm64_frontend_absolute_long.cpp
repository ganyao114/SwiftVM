#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

bool A64Decoder::TryNEONAbsoluteLong(const Instruction* instr) {
    const u32 op = instr->Mask(NEON3DifferentMask) & ~u32(NEON_Q);
    const bool sign = op == NEON_SABDL || op == NEON_SABAL;
    const bool accumulate = op == NEON_SABAL || op == NEON_UABAL;
    if (!sign && !accumulate && op != NEON_UABDL) return false;
    const u32 bits = 8u<<instr->GetNEONSize(),output_bits = bits*2;
    if (bits==64) { Interrupt(InterruptReason::ILL_CODE,current_pc);return true; }
    const bool upper = instr->GetNEONQ();
    auto zero = __ VecSharedZero().SetType(ir::ValueType::V128);
    auto widen = [&](u32 reg) {
        auto input = ReadVRegister(reg,ir::ValueType::V128);
        auto result = __ VecZip(input,zero,ir::Imm{bits},ir::Imm{upper}).SetType(ir::ValueType::V128);
        if (sign) {
            result = __ VecShiftLeftImm(result,ir::Imm{bits},ir::Imm{output_bits}).SetType(ir::ValueType::V128);
            result = __ VecShiftRightArithmeticImm(result,ir::Imm{bits},ir::Imm{output_bits}).SetType(ir::ValueType::V128);
        }
        return result;
    };
    auto left = widen(instr->GetRn()),right = widen(instr->GetRm());
    // A double-width signed lane holds every signed/unsigned source
    // difference, so abs needs no extra carry or saturation information.
    auto difference = __ VecSub(left,right,ir::Imm{output_bits}).SetType(ir::ValueType::V128);
    auto negative = __ VecShiftRightArithmeticImm(difference,ir::Imm{output_bits-1},ir::Imm{output_bits}).SetType(ir::ValueType::V128);
    auto flipped = __ VecXor(difference,negative).SetType(ir::ValueType::V128);
    auto result = __ VecSub(flipped,negative,ir::Imm{output_bits}).SetType(ir::ValueType::V128);
    if (accumulate) {
        auto old = ReadVRegister(instr->GetRd(),ir::ValueType::V128);
        result = __ VecAdd(old,result,ir::Imm{output_bits}).SetType(ir::ValueType::V128);
    }
    WriteVRegister(instr->GetRd(),result);return true;
}

#undef __
}  // namespace swift::arm64
