#include "arm64_frontend_internal.h"
#include "single_instruction_admission.h"
#include "fp_helpers.h"
#include "runtime/backend/context.h"
#include <cstring>

namespace swift::arm64 {
namespace {
bool DecodeUnary(u32 word, u32& operation) {
    switch (word & NEON2RegMiscFP16Mask) {
        case NEON_FSQRT_H: operation = FSQRT_s; break;
        case NEON_FRINTN_H: operation = FRINTN_s; break;
        case NEON_FRINTM_H: operation = FRINTM_s; break;
        case NEON_FRINTP_H: operation = FRINTP_s; break;
        case NEON_FRINTZ_H: operation = FRINTZ_s; break;
        case NEON_FRINTA_H: operation = FRINTA_s; break;
        case NEON_FRINTX_H: operation = FRINTX_s; break;
        case NEON_FRINTI_H: operation = FRINTI_s; break;
        default: return false;
    }
    return true;
}
}

u64 ExecuteVectorFP16Unary(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context_address);
    u32 operation;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeUnary(u32(instruction), operation)) {
        auto& state = *reinterpret_cast<runtime::backend::State*>(
            context_address - runtime::backend::state_offset_uniform_buffer);
        ctx.pc = guest_pc;
        ctx.padding = u32(InterruptReason::FALLBACK);
        state.current_loc = runtime::ir::Location{guest_pc};
        state.halt_reason = runtime::HaltReason::CallHost;
        return 0;
    }
    const u32 rn = (instruction >> 5) & 31, rd = instruction & 31;
    u16 source[8], destination[8]{};
    std::memcpy(source, &ctx.v[rn], sizeof(source));
    const u32 active = instruction & NEON_Q ? 8 : 4;
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        const auto result = EvaluateUnaryFP(source[lane], 16, operation, ctx.fpcr);
        destination[lane] = u16(result.value);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.v[rd], destination, sizeof(destination));
    return 0;
}

bool A64Decoder::TryNEONFP16Unary(const Instruction* instr) {
    u32 operation;
    if (!DecodeUnary(instr->GetInstructionBits(), operation)) return false;
    auto context = assembler->GetUniformAddress(ir::Imm{u32{0}}).SetType(ir::ValueType::U64);
    assembler->CallHostFPFree(&ExecuteVectorFP16Unary, context,
        ImmValue(instr->GetInstructionBits(), ir::ValueType::U64),
        ImmValue(current_pc, ir::ValueType::U64)).SetType(ir::ValueType::U64);
    return true;
}
bool detail::AdmitVectorFP16Unary(u64 target, u32 word) noexcept {
    u32 operation;
    return SingleInstructionTarget(target, &ExecuteVectorFP16Unary) && DecodeUnary(word, operation);
}

}  // namespace swift::arm64
