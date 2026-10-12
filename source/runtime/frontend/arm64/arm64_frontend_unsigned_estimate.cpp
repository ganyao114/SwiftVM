#include "arm64_frontend_internal.h"

namespace swift::arm64 {

bool A64Decoder::TryNEONUnsignedEstimate(const Instruction* instr) {
    // Bit 23 distinguishes integer estimates from FCVTAS/FCVTAU; only bit
    // 22 is a reserved size selector for these unsigned estimates.
    const u32 opcode = instr->Mask(NEON2RegMiscFPMask);
    if (opcode != NEON_URECPE && opcode != NEON_URSQRTE) return false;
    if (instr->GetNEONSize() != 2) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    // The shared IR computes four independent lanes without FP side effects.
    // Q=0 publishes only the low pair and explicitly clears the upper half.
    const auto input = ReadVRegister(instr->GetRn(), ir::ValueType::V128);
    auto result = assembler->VecUnsignedEstimate(input, ir::Imm{u32(opcode == NEON_URSQRTE)})
        .SetType(ir::ValueType::V128);
    if (!instr->GetNEONQ()) result = assembler->BitCast(result).SetType(ir::ValueType::V64);
    WriteVRegister(instr->GetRd(), result);
    return true;
}

}  // namespace swift::arm64
