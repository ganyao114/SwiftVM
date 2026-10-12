#include "arm64_frontend_internal.h"
#include "single_instruction_admission.h"
#include "fp_helpers.h"
#include "runtime/backend/context.h"
#include <cstring>

namespace swift::arm64 {
namespace {
bool DecodeVectorFMA(u32 word, bool fp16, u32& bits, u32& control) {
    if (word & NEONScalar) return false;
    const u32 op = word & (fp16 ? NEON3SameFP16Mask : NEON3SameFPMask);
    if (op == (fp16 ? u32(NEON_FMLA_H) : u32(NEON_FMLA))) control = 0;
    else if (op == (fp16 ? u32(NEON_FMLS_H) : u32(NEON_FMLS))) control = 1;
    else return false;
    bits = fp16 ? 16 : (word & (1u << 22)) ? 64 : 32;
    return true;
}

u64 ExecuteVectorFMA(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context_address);
    const u32 word = u32(instruction);
    const bool fp16 = (word & NEON3SameFP16FMask) == NEON3SameFP16Fixed;
    u32 bits, control;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeVectorFMA(word, fp16, bits, control)) {
        auto& state = *reinterpret_cast<runtime::backend::State*>(
            context_address - runtime::backend::state_offset_uniform_buffer);
        ctx.pc = guest_pc;
        ctx.padding = u32(InterruptReason::FALLBACK);
        state.current_loc = runtime::ir::Location{guest_pc};
        state.halt_reason = runtime::HaltReason::CallHost;
        return 0;
    }
    const u32 rn = (word >> 5) & 31, rm = (word >> 16) & 31, rd = word & 31;
    u8 lhs[16], rhs[16], accumulator[16], destination[16]{};
    std::memcpy(lhs, &ctx.v[rn], sizeof(lhs));
    std::memcpy(rhs, &ctx.v[rm], sizeof(rhs));
    std::memcpy(accumulator, &ctx.v[rd], sizeof(accumulator));
    const u32 active = (word & NEON_Q ? 128 : 64) / bits;
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        u64 left = 0, right = 0, addend = 0;
        std::memcpy(&left, lhs + lane * bits / 8, bits / 8);
        std::memcpy(&right, rhs + lane * bits / 8, bits / 8);
        std::memcpy(&addend, accumulator + lane * bits / 8, bits / 8);
        const auto result = EvaluateFusedFP(left, right, addend, bits, control, ctx.fpcr);
        std::memcpy(destination + lane * bits / 8, &result.value, bits / 8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.v[rd], destination, sizeof(destination));
    return 0;
}
}

bool A64Decoder::TryNEONVectorFMA(const Instruction* instr, bool fp16) {
    u32 bits, control;
    if (!DecodeVectorFMA(instr->GetInstructionBits(), fp16, bits, control)) return false;
    if (bits == 64 && !instr->GetNEONQ()) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    auto context = assembler->GetUniformAddress(ir::Imm{u32{0}}).SetType(ir::ValueType::U64);
    assembler->CallHostFPFree(&ExecuteVectorFMA, context,
        ImmValue(instr->GetInstructionBits(), ir::ValueType::U64),
        ImmValue(current_pc, ir::ValueType::U64)).SetType(ir::ValueType::U64);
    return true;
}
bool detail::AdmitVectorFMA(u64 target, u32 word) noexcept {
    const bool fp16 = (word & NEON3SameFP16FMask) == NEON3SameFP16Fixed;
    u32 bits, control;
    return SingleInstructionTarget(target, &ExecuteVectorFMA) && DecodeVectorFMA(word, fp16, bits, control) &&
           (bits != 64 || (word & NEON_Q));
}

}  // namespace swift::arm64
