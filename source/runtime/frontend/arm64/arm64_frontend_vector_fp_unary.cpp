#include "arm64_frontend_internal.h"
#include "single_instruction_admission.h"
#include "fp_helpers.h"
#include "runtime/backend/context.h"
#include <cstring>

namespace swift::arm64 {
namespace {
bool DecodeVectorUnary(u32 word, u32& operation) {
    switch (word & NEON2RegMiscFPMask) {
        case NEON_FSQRT: operation = FSQRT_s; break;
        case NEON_FRINTN: operation = FRINTN_s; break;
        case NEON_FRINTM: operation = FRINTM_s; break;
        case NEON_FRINTP: operation = FRINTP_s; break;
        case NEON_FRINTZ: operation = FRINTZ_s; break;
        case NEON_FRINTA: operation = FRINTA_s; break;
        case NEON_FRINTX: operation = FRINTX_s; break;
        case NEON_FRINTI: operation = FRINTI_s; break;
        default: return false;
    }
    return true;
}

u64 ExecuteVectorUnary(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context_address);
    const u32 word = u32(instruction);
    u32 operation;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeVectorUnary(word, operation)) {
        auto& state = *reinterpret_cast<runtime::backend::State*>(
            context_address - runtime::backend::state_offset_uniform_buffer);
        ctx.pc = guest_pc;
        ctx.padding = u32(InterruptReason::FALLBACK);
        state.current_loc = runtime::ir::Location{guest_pc};
        state.halt_reason = runtime::HaltReason::CallHost;
        return 0;
    }
    const u32 bits = word & (1u << 22) ? 64 : 32, rn = (word >> 5) & 31, rd = word & 31;
    u8 source[16], destination[16]{};
    std::memcpy(source, &ctx.v[rn], sizeof(source));
    const u32 active = (word & NEON_Q ? 128 : 64) / bits;
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        u64 value = 0;
        std::memcpy(&value, source + lane * bits / 8, bits / 8);
        const auto result = EvaluateUnaryFP(value, bits, operation, ctx.fpcr);
        std::memcpy(destination + lane * bits / 8, &result.value, bits / 8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.v[rd], destination, sizeof(destination));
    return 0;
}
}

bool A64Decoder::TryNEONVectorFPUnary(const Instruction* instr) {
    u32 operation;
    if (!DecodeVectorUnary(instr->GetInstructionBits(), operation)) return false;
    if ((instr->GetInstructionBits() & (1u << 22)) && !instr->GetNEONQ()) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    auto context = assembler->GetUniformAddress(ir::Imm{u32{0}}).SetType(ir::ValueType::U64);
    assembler->CallHostFPFree(&ExecuteVectorUnary, context,
        ImmValue(instr->GetInstructionBits(), ir::ValueType::U64),
        ImmValue(current_pc, ir::ValueType::U64)).SetType(ir::ValueType::U64);
    return true;
}
bool detail::AdmitVectorUnary(u64 target, u32 word) noexcept {
    u32 operation;
    return SingleInstructionTarget(target, &ExecuteVectorUnary) && DecodeVectorUnary(word, operation) &&
           (!(word & (1u << 22)) || (word & NEON_Q));
}

}  // namespace swift::arm64
