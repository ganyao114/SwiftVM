#include "arm64_frontend_internal.h"
#include "single_instruction_admission.h"
#include "fp_helpers.h"
#include "fp_estimate.h"
#include "runtime/backend/context.h"
#include <cstring>

namespace swift::arm64 {
namespace {
bool DecodeEstimate(u32 word, bool half, FPEstimateOperation& operation) {
    word &= ~(NEON_Q | NEONScalar);
    const u32 code = word & (half ? NEON2RegMiscFP16Mask : NEON2RegMiscFPMask);
    if (code == u32(half ? NEON_FRECPE_H : NEON_FRECPE)) operation = FPEstimateOperation::Reciprocal;
    else if (code == u32(half ? NEON_FRSQRTE_H : NEON_FRSQRTE)) operation = FPEstimateOperation::ReciprocalSquareRoot;
    else return false;
    return true;
}
// FRECPX is allocated only in the SIMD scalar class and has its own opcode.
bool DecodeScalarExponent(u32 word, bool half) {
    return (word & (half ? NEONScalar2RegMiscFP16Mask : NEONScalar2RegMiscFPMask)) ==
           u32(half ? NEON_FRECPX_H_scalar : NEON_FRECPX_scalar);
}

template<bool Half>
u64 ExecuteEstimate(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context_address);
    const u32 word = u32(instruction);
    FPEstimateOperation operation;
    const bool decoded = DecodeScalarExponent(word, Half) ? (operation = FPEstimateOperation::ReciprocalExponent, true)
                                                        : DecodeEstimate(word, Half, operation);
    if (!IsSupportedFPCR(ctx.fpcr) || !decoded) {
        auto& state = *reinterpret_cast<runtime::backend::State*>(
            context_address - runtime::backend::state_offset_uniform_buffer);
        ctx.pc = guest_pc;
        ctx.padding = u32(InterruptReason::FALLBACK);
        state.current_loc = runtime::ir::Location{guest_pc};
        state.halt_reason = runtime::HaltReason::CallHost;
        return 0;
    }
    const u32 bits = Half ? 16 : word & (1u << 22) ? 64 : 32;
    const u32 rn = (word >> 5) & 31, rd = word & 31;
    u8 source[16], destination[16]{};
    std::memcpy(source, &ctx.v[rn], sizeof(source));
    const u32 active = word & NEONScalar ? 1 : (word & NEON_Q ? 128 : 64)/bits;
    u32 exceptions = 0;
    for (u32 lane=0; lane<active; ++lane) {
        u64 value = 0;
        std::memcpy(&value, source+lane*bits/8, bits/8);
        const auto result = EvaluateEstimateFP(value,bits,operation,ctx.fpcr);
        std::memcpy(destination+lane*bits/8,&result.value,bits/8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.v[rd],destination,sizeof(destination));
    return 0;
}
}

bool A64Decoder::TryNEONFPEstimate(const Instruction* instr, bool half) {
    const u32 word = instr->GetInstructionBits();
    FPEstimateOperation operation;
    if (!DecodeScalarExponent(word,half) && !DecodeEstimate(word,half,operation)) return false;
    if (!half && (word & (1u<<22)) && !(word & NEON_Q)) {
        Interrupt(InterruptReason::ILL_CODE,current_pc);
        return true;
    }
    auto context = assembler->GetUniformAddress(ir::Imm{u32{0}}).SetType(ir::ValueType::U64);
    assembler->CallHostFPFree(half ? &ExecuteEstimate<true> : &ExecuteEstimate<false>,context,
        ImmValue(word,ir::ValueType::U64),ImmValue(current_pc,ir::ValueType::U64)).SetType(ir::ValueType::U64);
    return true;
}
bool detail::AdmitEstimate(u64 target, u32 word) noexcept {
    const bool half = SingleInstructionTarget(target, &ExecuteEstimate<true>);
    if (!half && !SingleInstructionTarget(target, &ExecuteEstimate<false>)) return false;
    FPEstimateOperation operation;
    return (DecodeScalarExponent(word, half) || DecodeEstimate(word, half, operation)) &&
           (half || !(word & (1u << 22)) || (word & NEON_Q));
}

}  // namespace swift::arm64
