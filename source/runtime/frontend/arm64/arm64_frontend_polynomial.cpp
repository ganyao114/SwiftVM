#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

bool A64Decoder::TryNEONPolynomial(const Instruction* instr,bool widening) {
    const u32 word = instr->GetInstructionBits();
    const u32 op = instr->Mask(widening ? NEON3DifferentMask : NEON3SameMask) & ~u32(NEON_Q);
    if (op != u32(widening ? NEON_PMULL : NEON_PMUL)) return false;
    const u32 size = instr->GetNEONSize();const bool q = instr->GetNEONQ();
    if (widening ? size != 0 && size != 3 : size != 0) {
        Interrupt(InterruptReason::ILL_CODE,current_pc);return true;
    }
    auto left = ReadVRegister(instr->GetRn(),ir::ValueType::V128);
    auto right = ReadVRegister(instr->GetRm(),ir::ValueType::V128);
    ir::Value result;
    if (widening && size == 3) {
        // PMULL2 selects the high 64-bit lane of both operands.
        result = __ VecPclMul(left,right,ir::Imm{q ? 0x11u : 0u}).SetType(ir::ValueType::V128);
    } else {
        auto constant = [&](u64 half) { return __ VecLoadConst(ir::Imm{half},ir::Imm{half}).SetType(ir::ValueType::V128); };
        auto zero = __ VecSharedZero().SetType(ir::ValueType::V128);
        if (widening) {
            left = __ VecZip(left,zero,ir::Imm{8u},ir::Imm{q}).SetType(ir::ValueType::V128);
            right = __ VecZip(right,zero,ir::Imm{8u},ir::Imm{q}).SetType(ir::ValueType::V128);
        }
        result = zero;
        const auto ones = constant(widening ? 0x0001000100010001ULL : 0x0101010101010101ULL);
        for (u32 bit=0;bit<8;++bit) {
            auto coefficient = __ VecShiftRightImm(right,ir::Imm{bit},ir::Imm{16u}).SetType(ir::ValueType::V128);
            coefficient = __ VecAnd(coefficient,ones).SetType(ir::ValueType::V128);
            auto selected = __ VecCmpEq(coefficient,ones,ir::Imm{widening ? 16u : 8u}).SetType(ir::ValueType::V128);
            auto term = __ VecShiftLeftImm(left,ir::Imm{bit},ir::Imm{16u}).SetType(ir::ValueType::V128);
            if (!widening) {
                // H shifts implement byte shifts after masking the low bits
                // introduced from the adjacent byte, avoiding scalar B shifts.
                const u64 mask = u64((0xFFu<<bit)&0xFFu)*0x0101010101010101ULL;
                term = __ VecAnd(term,constant(mask)).SetType(ir::ValueType::V128);
            }
            term = __ VecAnd(term,selected).SetType(ir::ValueType::V128);
            result = __ VecXor(result,term).SetType(ir::ValueType::V128);
        }
        if (!widening && !q) result = __ BitCast(result).SetType(ir::ValueType::V64);
    }
    WriteVRegister(instr->GetRd(),result);return true;
}

#undef __
}  // namespace swift::arm64
