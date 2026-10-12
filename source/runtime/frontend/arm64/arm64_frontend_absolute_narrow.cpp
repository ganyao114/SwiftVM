#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

bool A64Decoder::TryNEONAbsoluteNarrow(const Instruction* instr) {
    const u32 op = instr->Mask(NEON3SameMask);
    const bool sign = op == NEON_SABD || op == NEON_SABA;
    const bool accumulate = op == NEON_SABA || op == NEON_UABA;
    if (!sign && !accumulate && op != NEON_UABD) return false;
    const u32 bits = 8u<<instr->GetNEONSize();const bool q = instr->GetNEONQ();
    if (bits==64) { Interrupt(InterruptReason::ILL_CODE,current_pc);return true; }
    auto left = ReadVRegister(instr->GetRn(),ir::ValueType::V128);
    auto right = ReadVRegister(instr->GetRm(),ir::ValueType::V128);
    auto compare_left = left,compare_right = right;
    if (!sign) {
        u64 sign_bits = 0;
        for (u32 position=0;position<64;position+=bits) sign_bits |= u64{1}<<(position+bits-1);
        auto bias = __ VecLoadConst(ir::Imm{sign_bits},ir::Imm{sign_bits}).SetType(ir::ValueType::V128);
        compare_left = __ VecXor(left,bias).SetType(ir::ValueType::V128);
        compare_right = __ VecXor(right,bias).SetType(ir::ValueType::V128);
    }
    auto greater = __ VecCmpGt(compare_left,compare_right,ir::Imm{bits}).SetType(ir::ValueType::V128);
    // Both differences wrap at lane width. Choosing the subtraction from
    // larger to smaller yields the full unsigned magnitude, including the
    // signed MIN/MAX difference that does not fit a signed narrow lane.
    auto forward = __ VecSub(left,right,ir::Imm{bits}).SetType(ir::ValueType::V128);
    auto reverse = __ VecSub(right,left,ir::Imm{bits}).SetType(ir::ValueType::V128);
    forward = __ VecAnd(forward,greater).SetType(ir::ValueType::V128);
    reverse = __ VecAndNot(reverse,greater).SetType(ir::ValueType::V128);
    auto result = __ VecOr(forward,reverse).SetType(ir::ValueType::V128);
    if (accumulate) {
        auto old = ReadVRegister(instr->GetRd(),ir::ValueType::V128);
        result = __ VecAdd(old,result,ir::Imm{bits}).SetType(ir::ValueType::V128);
    }
    if (!q) result = __ BitCast(result).SetType(ir::ValueType::V64);
    WriteVRegister(instr->GetRd(),result);return true;
}

#undef __
}  // namespace swift::arm64
