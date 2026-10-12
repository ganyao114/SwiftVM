#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

bool A64Decoder::TryNEONHalving(const Instruction* instr) {
    const u32 op = instr->Mask(NEON3SameMask);
    const bool add = op == NEON_SHADD || op == NEON_UHADD;
    const bool rounded = op == NEON_SRHADD || op == NEON_URHADD;
    const bool subtract = op == NEON_SHSUB || op == NEON_UHSUB;
    if (!add && !rounded && !subtract) return false;
    const u32 bits = 8u << instr->GetNEONSize();
    if (bits == 64) { Interrupt(InterruptReason::ILL_CODE,current_pc);return true; }
    const bool sign = op == NEON_SHADD || op == NEON_SRHADD || op == NEON_SHSUB;
    const auto left = ReadVRegister(instr->GetRn(),ir::ValueType::V128);
    const auto right = ReadVRegister(instr->GetRm(),ir::ValueType::V128);
    const auto different = __ VecXor(left,right).SetType(ir::ValueType::V128);
    auto half = [&](ir::Value value) {
        return (sign ? __ VecShiftRightArithmeticImm(value,ir::Imm{1u},ir::Imm{bits})
                     : __ VecShiftRightImm(value,ir::Imm{1u},ir::Imm{bits})).SetType(ir::ValueType::V128);
    };
    ir::Value result;
    if (subtract) {
        // floor((a-b)/2) = floor(a/2)-floor(b/2)-(!a[0] && b[0]).
        // The unsigned difference may be negative: retain its lane-sized bits.
        u64 ones = 0;for (u32 bit=0;bit<64;bit+=bits) ones |= u64{1}<<bit;
        auto low_bits = __ VecLoadConst(ir::Imm{ones},ir::Imm{ones}).SetType(ir::ValueType::V128);
        auto borrow = __ VecAnd(different,right).SetType(ir::ValueType::V128);
        borrow = __ VecAnd(borrow,low_bits).SetType(ir::ValueType::V128);
        result = __ VecSub(half(left),half(right),ir::Imm{bits}).SetType(ir::ValueType::V128);
        result = __ VecSub(result,borrow,ir::Imm{bits}).SetType(ir::ValueType::V128);
    } else if (rounded) {
        // ceil((a+b)/2) = (a|b)-floor((a^b)/2), including signed lanes.
        auto common = __ VecOr(left,right).SetType(ir::ValueType::V128);
        result = __ VecSub(common,half(different),ir::Imm{bits}).SetType(ir::ValueType::V128);
    } else {
        // floor((a+b)/2) = (a&b)+floor((a^b)/2), without a lost carry.
        auto common = __ VecAnd(left,right).SetType(ir::ValueType::V128);
        result = __ VecAdd(common,half(different),ir::Imm{bits}).SetType(ir::ValueType::V128);
    }
    if (!instr->GetNEONQ()) result = __ BitCast(result).SetType(ir::ValueType::V64);
    WriteVRegister(instr->GetRd(),result);return true;
}

#undef __
}  // namespace swift::arm64
