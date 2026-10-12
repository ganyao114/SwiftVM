#include "arm64_frontend_internal.h"
#include "runtime/common/saturating_shift.h"

namespace swift::arm64 {
#define __ assembler->

bool A64Decoder::TryNEONVectorSaturatingShift(const Instruction* instr) {
    const auto op = instr->Mask(NEONShiftImmediateMask);
    runtime::SaturatingShiftMode mode;
    switch (op) {
        case NEON_SQSHL_imm: mode = runtime::SaturatingShiftMode::SQSHL; break;
        case NEON_UQSHL_imm: mode = runtime::SaturatingShiftMode::UQSHL; break;
        case NEON_SQSHLU: mode = runtime::SaturatingShiftMode::SQSHLU; break;
        case NEON_SQSHRN: mode = runtime::SaturatingShiftMode::SQSHRN; break;
        case NEON_UQSHRN: mode = runtime::SaturatingShiftMode::UQSHRN; break;
        case NEON_SQSHRUN: mode = runtime::SaturatingShiftMode::SQSHRUN; break;
        case NEON_SQRSHRN: mode = runtime::SaturatingShiftMode::SQRSHRN; break;
        case NEON_UQRSHRN: mode = runtime::SaturatingShiftMode::UQRSHRN; break;
        case NEON_SQRSHRUN: mode = runtime::SaturatingShiftMode::SQRSHRUN; break;
        default: return false;
    }
    const bool narrow = u32(mode) >= 3, q = instr->GetNEONQ();
    const int highest = vixl::HighestSetBitPosition(instr->GetImmNEONImmh());
    if (highest < 0 || highest > (narrow ? 2 : 3) || (!narrow && highest == 3 && !q)) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    const u32 bits = 8u << highest, encoded = instr->GetImmNEONImmhImmb();
    const u32 shift = narrow ? 2 * bits - encoded : encoded - bits;
    const u32 lanes = (narrow ? 64u : q ? 128u : 64u) / bits;
    auto input = ReadVRegister(instr->GetRn(), ir::ValueType::V128);
    auto result = __ VecSatShiftImm(input, ir::Imm{bits}, ir::Imm{shift},
        ir::Imm{runtime::SaturatingShiftControl(mode, lanes)}).SetType(ir::ValueType::V128);
    auto saturated = __ VecSatShiftQC(result).SetType(ir::ValueType::U8);
    ir::Uniform status{u32(offsetof(ThreadContext64, fpsr)), ir::ValueType::U32};
    auto old = __ LoadUniform<ir::U32>(status);
    auto qc = __ Select(saturated, ImmValue(1u << 27, ir::ValueType::U32),
                       ImmValue(0, ir::ValueType::U32)).SetType(ir::ValueType::U32);
    __ StoreUniform(status, __ Or<ir::U32>(old, SingleOperand(qc)).SetType(ir::ValueType::U32));
    if (narrow && q) {
        // Preserve the original low destination half, including Rd == Rn.
        auto low = ReadVHalf(instr->GetRd(), false);
        auto high = __ VecExtract64(result, ir::Imm{u32{0}}).SetType(ir::ValueType::U64);
        WriteVHalves(instr->GetRd(), low, high);
    } else WriteVRegister(instr->GetRd(), result);
    return true;
}

#undef __
}  // namespace swift::arm64
