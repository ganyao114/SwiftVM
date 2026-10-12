#include "arm64_frontend_internal.h"
#include "single_instruction_admission.h"
#include "fp_helpers.h"
#include "runtime/backend/context.h"
#include <cstring>

namespace swift::arm64 {
namespace {
bool DecodeBinary(u32 word, u32& operation, bool& absolute) {
    absolute = false;
    switch (word & NEON3SameFPMask) {
        case NEON_FADD: operation = 2; break;
        case NEON_FSUB: operation = 3; break;
        case NEON_FMUL: operation = 0; break;
        case NEON_FDIV: operation = 1; break;
        case NEON_FMAX: operation = 4; break;
        case NEON_FMIN: operation = 5; break;
        case NEON_FMAXNM: operation = 6; break;
        case NEON_FMINNM: operation = 7; break;
        case NEON_FABD: operation = 3; absolute = true; break;
        default: return false;
    }
    return true;
}

u64 ExecuteVectorFPBinary(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context_address);
    u32 operation;
    bool absolute;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeBinary(u32(instruction), operation, absolute)) {
        auto& state = *reinterpret_cast<runtime::backend::State*>(
            context_address - runtime::backend::state_offset_uniform_buffer);
        ctx.pc = guest_pc;
        ctx.padding = u32(InterruptReason::FALLBACK);
        state.current_loc = runtime::ir::Location{guest_pc};
        state.halt_reason = runtime::HaltReason::CallHost;
        return 0;
    }
    const u32 bits = instruction & (1u << 22) ? 64 : 32;
    const u32 active = (instruction & NEON_Q ? 128 : 64) / bits;
    const u32 rn = (instruction >> 5) & 31, rm = (instruction >> 16) & 31, rd = instruction & 31;
    u8 lhs[16], rhs[16], destination[16]{};
    std::memcpy(lhs, &ctx.v[rn], sizeof(lhs));
    std::memcpy(rhs, &ctx.v[rm], sizeof(rhs));
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        u64 left = 0, right = 0;
        std::memcpy(&left, lhs + lane * bits / 8, bits / 8);
        std::memcpy(&right, rhs + lane * bits / 8, bits / 8);
        auto result = EvaluateBinaryFP(left, right, bits, operation, ctx.fpcr);
        if (absolute) result.value &= ~(u64{1} << (bits - 1));
        std::memcpy(destination + lane * bits / 8, &result.value, bits / 8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.v[rd], destination, sizeof(destination));
    return 0;
}
}

bool A64Decoder::TryNEONVectorFPBinary(const Instruction* instr) {
    u32 operation;
    bool absolute;
    if (!DecodeBinary(instr->GetInstructionBits(), operation, absolute)) return false;
    // VIXL's generic 3Same visitor also receives the unallocated 1D forms.
    if ((instr->GetInstructionBits() & (1u << 22)) && !instr->GetNEONQ()) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    auto context = assembler->GetUniformAddress(ir::Imm{u32{0}}).SetType(ir::ValueType::U64);
    assembler->CallHostFPFree(&ExecuteVectorFPBinary, context,
        ImmValue(instr->GetInstructionBits(), ir::ValueType::U64),
        ImmValue(current_pc, ir::ValueType::U64)).SetType(ir::ValueType::U64);
    return true;
}
bool detail::AdmitVectorFPBinary(u64 target, u32 word) noexcept {
    u32 operation;
    bool absolute;
    return SingleInstructionTarget(target, &ExecuteVectorFPBinary) && DecodeBinary(word, operation, absolute) &&
           (!(word & (1u << 22)) || (word & NEON_Q));
}

}  // namespace swift::arm64
