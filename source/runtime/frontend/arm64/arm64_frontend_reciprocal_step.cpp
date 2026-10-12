#include "arm64_frontend_internal.h"
#include "single_instruction_admission.h"
#include "fp_helpers.h"
#include "runtime/backend/context.h"
#include <cstring>

namespace swift::arm64 {
namespace {
enum class Operation { ExtendedMultiply, ReciprocalStep, ReciprocalSquareRootStep };

bool DecodeExtendedFP(u32 word, bool fp16, u32& bits, Operation& operation) {
    const bool scalar = word & NEONScalar;
    const u32 mask = fp16 ? (scalar ? u32(NEONScalar3SameFP16Mask) : u32(NEON3SameFP16Mask)) :
                           (scalar ? u32(NEONScalar3SameFPMask) : u32(NEON3SameFPMask));
    const u32 scalar_flags = scalar ? u32(NEON_Q | NEONScalar) : 0;
    const u32 op = word & mask;
    if (op == ((fp16 ? u32(NEON_FMULX_H) : u32(NEON_FMULX)) | scalar_flags))
        operation = Operation::ExtendedMultiply;
    else if (op == ((fp16 ? u32(NEON_FRECPS_H) : u32(NEON_FRECPS)) | scalar_flags))
        operation = Operation::ReciprocalStep;
    else if (op == ((fp16 ? u32(NEON_FRSQRTS_H) : u32(NEON_FRSQRTS)) | scalar_flags))
        operation = Operation::ReciprocalSquareRootStep;
    else return false;
    bits = fp16 ? 16 : (word & (1u << 22)) ? 64 : 32;
    return true;
}

u64 ExecuteExtendedFP(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context_address);
    const u32 word = u32(instruction);
    const bool scalar = word & NEONScalar;
    const bool fp16 = scalar ? (word & NEONScalar3SameFP16FMask) == NEONScalar3SameFP16Fixed :
                              (word & NEON3SameFP16FMask) == NEON3SameFP16Fixed;
    u32 bits;
    Operation operation;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeExtendedFP(word, fp16, bits, operation)) {
        auto& state = *reinterpret_cast<runtime::backend::State*>(
            context_address - runtime::backend::state_offset_uniform_buffer);
        ctx.pc = guest_pc;
        ctx.padding = u32(InterruptReason::FALLBACK);
        state.current_loc = runtime::ir::Location{guest_pc};
        state.halt_reason = runtime::HaltReason::CallHost;
        return 0;
    }
    const u32 rn = (word >> 5) & 31, rm = (word >> 16) & 31, rd = word & 31;
    u8 lhs[16], rhs[16], destination[16]{};
    std::memcpy(lhs, &ctx.v[rn], sizeof(lhs));
    std::memcpy(rhs, &ctx.v[rm], sizeof(rhs));
    const u32 active = scalar ? 1 : (word & NEON_Q ? 128 : 64) / bits;
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        u64 left = 0, right = 0;
        std::memcpy(&left, lhs + lane * bits / 8, bits / 8);
        std::memcpy(&right, rhs + lane * bits / 8, bits / 8);
        const auto result = operation == Operation::ExtendedMultiply ?
            EvaluateExtendedMultiplyFP(left, right, bits, ctx.fpcr) :
            EvaluateReciprocalStepFP(left, right, bits, operation == Operation::ReciprocalSquareRootStep, ctx.fpcr);
        std::memcpy(destination + lane * bits / 8, &result.value, bits / 8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.v[rd], destination, sizeof(destination));
    return 0;
}
}

bool A64Decoder::TryNEONExtendedFP(const Instruction* instr, bool fp16) {
    u32 bits;
    Operation operation;
    if (!DecodeExtendedFP(instr->GetInstructionBits(), fp16, bits, operation)) return false;
    if (!(instr->GetInstructionBits() & NEONScalar) && bits == 64 && !instr->GetNEONQ()) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    auto context = assembler->GetUniformAddress(ir::Imm{u32{0}}).SetType(ir::ValueType::U64);
    assembler->CallHostFPFree(&ExecuteExtendedFP, context,
        ImmValue(instr->GetInstructionBits(), ir::ValueType::U64),
        ImmValue(current_pc, ir::ValueType::U64)).SetType(ir::ValueType::U64);
    return true;
}
bool detail::AdmitExtendedFP(u64 target, u32 word) noexcept {
    const bool scalar = word & NEONScalar;
    const bool fp16 = scalar ? (word & NEONScalar3SameFP16FMask) == NEONScalar3SameFP16Fixed :
                              (word & NEON3SameFP16FMask) == NEON3SameFP16Fixed;
    u32 bits;
    Operation operation;
    return SingleInstructionTarget(target, &ExecuteExtendedFP) && DecodeExtendedFP(word, fp16, bits, operation) &&
           (scalar || bits != 64 || (word & NEON_Q));
}

}  // namespace swift::arm64
