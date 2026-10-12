#include "arm64_frontend_internal.h"
#include "single_instruction_admission.h"
#include "fp_helpers.h"
#include "runtime/backend/context.h"
#include <cstring>

namespace swift::arm64 {
namespace {
bool DecodeBinary(u32 word, u32& operation, bool& absolute) {
    const bool scalar = word & NEONScalar;
    const u32 op = word & (scalar ? NEONScalar3SameFP16Mask : NEON3SameFP16Mask);
    absolute = op == (scalar ? NEON_FABD_H_scalar : NEON_FABD_H);
    if (absolute) { operation = 3; return true; }
    if (scalar) return false;
    switch (op) {
        case NEON_FADD_H: operation = 2; break;
        case NEON_FSUB_H: operation = 3; break;
        case NEON_FMUL_H: operation = 0; break;
        case NEON_FDIV_H: operation = 1; break;
        case NEON_FMAX_H: operation = 4; break;
        case NEON_FMIN_H: operation = 5; break;
        case NEON_FMAXNM_H: operation = 6; break;
        case NEON_FMINNM_H: operation = 7; break;
        default: return false;
    }
    return true;
}

u64 ExecuteFP16Binary(u64 context_address, u64 instruction, u64 guest_pc) {
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
    const u32 rn = (instruction >> 5) & 31, rm = (instruction >> 16) & 31, rd = instruction & 31;
    u16 lhs[8], rhs[8], destination[8]{};
    std::memcpy(lhs, &ctx.v[rn], sizeof(lhs));
    std::memcpy(rhs, &ctx.v[rm], sizeof(rhs));
    const u32 active = instruction & NEONScalar ? 1 : instruction & NEON_Q ? 8 : 4;
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        const auto result = EvaluateBinaryFP(lhs[lane], rhs[lane], 16, operation, ctx.fpcr);
        destination[lane] = u16(absolute ? result.value & 0x7FFF : result.value);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.v[rd], destination, sizeof(destination));
    return 0;
}
}

void A64Decoder::VisitNEON3SameFP16(const Instruction* instr) {
    if (TryNEONFPReduction(instr, true, false)) return;
    if (TryNEONFPCompare(instr, true, false)) return;
    if (TryNEONExtendedFP(instr, true)) return;
    if (TryNEONVectorFMA(instr, true)) return;
    u32 operation;
    bool absolute;
    if (!DecodeBinary(instr->GetInstructionBits(), operation, absolute)) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    auto context = assembler->GetUniformAddress(ir::Imm{u32{0}}).SetType(ir::ValueType::U64);
    assembler->CallHostFPFree(&ExecuteFP16Binary, context,
        ImmValue(instr->GetInstructionBits(), ir::ValueType::U64),
        ImmValue(current_pc, ir::ValueType::U64)).SetType(ir::ValueType::U64);
}

void A64Decoder::VisitNEONScalar3SameFP16(const Instruction* instr) {
    VisitNEON3SameFP16(instr);
}
bool detail::AdmitFP16Binary(u64 target, u32 word) noexcept {
    u32 operation;
    bool absolute;
    return SingleInstructionTarget(target, &ExecuteFP16Binary) && DecodeBinary(word, operation, absolute);
}

}  // namespace swift::arm64
