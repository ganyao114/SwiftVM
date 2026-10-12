#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

bool A64Decoder::TryNEONDotProduct(const Instruction* instr, bool indexed) {
    const auto op = instr->Mask(indexed ? NEONByIndexedElementMask : NEON3SameExtraMask);
    const bool signed_bytes = op == (indexed ? NEON_SDOT_byelement : NEON_SDOT);
    if (!signed_bytes && op != (indexed ? NEON_UDOT_byelement : NEON_UDOT)) return false;
    if (instr->GetNEONSize() != 2) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    const u32 index = (instr->GetNEONH() << 1) | instr->GetNEONL();
    // Take all source and accumulator snapshots before the destination write.
    // Indexed Rm uses all five register bits: M is not an index bit here.
    auto lhs = ReadVRegister(instr->GetRn(), ir::ValueType::V128);
    ir::Value rhs;
    if (indexed) {
        auto group = ReadVHalf(instr->GetRm(), index >= 2);
        group = __ LsrImm<ir::U64>(group, ir::Imm{(index % 2) * 32}).SetType(ir::ValueType::U64);
        group = __ And<ir::U64>(group, SingleOperand(ir::Imm{u64{0xFFFFFFFF}})).SetType(ir::ValueType::U64);
        auto upper = __ LslImm<ir::U64>(group, ir::Imm{u32{32}}).SetType(ir::ValueType::U64);
        group = __ Or<ir::U64>(group, SingleOperand(upper)).SetType(ir::ValueType::U64);
        rhs = __ VecDup64(group).SetType(ir::ValueType::V128);
    } else {
        rhs = ReadVRegister(instr->GetRm(), ir::ValueType::V128);
    }
    auto old = ReadVRegister(instr->GetRd(), ir::ValueType::V128);
    auto result = __ VecDot(lhs, rhs, old, ir::Imm{u32(signed_bytes)}).SetType(ir::ValueType::V128);
    if (!instr->GetNEONQ()) result = __ BitCast(result).SetType(ir::ValueType::V64);
    WriteVRegister(instr->GetRd(), result);
    return true;
}

void A64Decoder::VisitNEON3SameExtra(const Instruction* instr) {
    if (TryNEONDotProduct(instr, false)) return;
    if (TryNEONSaturatingMultiplyHigh(instr, false, false, true)) return;
    Interrupt(InterruptReason::FALLBACK, current_pc);
}

#undef __
}  // namespace swift::arm64
