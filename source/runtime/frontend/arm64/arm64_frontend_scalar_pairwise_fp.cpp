#include "arm64_frontend_internal.h"
#include "single_instruction_admission.h"
#include "fp_helpers.h"
#include "runtime/backend/context.h"
#include <cstring>

namespace swift::arm64 {
namespace {
bool DecodePairwise(u32 word, u32& bits, u32& operation) {
    const u32 op = word & NEONScalarPairwiseMask;
    bool half = false;
    switch (op) {
        case NEON_FADDP_h_scalar: half = true; [[fallthrough]];
        case NEON_FADDP_scalar: operation = 2; break;
        case NEON_FMAXP_h_scalar: half = true; [[fallthrough]];
        case NEON_FMAXP_scalar: operation = 4; break;
        case NEON_FMINP_h_scalar: half = true; [[fallthrough]];
        case NEON_FMINP_scalar: operation = 5; break;
        case NEON_FMAXNMP_h_scalar: half = true; [[fallthrough]];
        case NEON_FMAXNMP_scalar: operation = 6; break;
        case NEON_FMINNMP_h_scalar: half = true; [[fallthrough]];
        case NEON_FMINNMP_scalar: operation = 7; break;
        default: return false;
    }
    bits = half ? 16 : (word & (1u << 22)) ? 64 : 32;
    return true;
}
}

u64 ExecuteScalarPairwiseFP(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context_address);
    u32 bits, operation;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodePairwise(u32(instruction), bits, operation)) {
        auto& state = *reinterpret_cast<runtime::backend::State*>(
            context_address - runtime::backend::state_offset_uniform_buffer);
        ctx.pc = guest_pc;
        ctx.padding = u32(InterruptReason::FALLBACK);
        state.current_loc = runtime::ir::Location{guest_pc};
        state.halt_reason = runtime::HaltReason::CallHost;
        return 0;
    }
    const u32 rn = (instruction >> 5) & 31, rd = instruction & 31;
    u64 source[2];
    std::memcpy(source, &ctx.v[rn], sizeof(source));
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    const u64 lhs = source[0] & mask;
    const u64 rhs = bits == 64 ? source[1] : (source[0] >> bits) & mask;
    const auto result = EvaluateBinaryFP(lhs, rhs, bits, operation, ctx.fpcr);
    ctx.fpsr |= result.exceptions;
    const u64 destination[] = {result.value, 0};
    std::memcpy(&ctx.v[rd], destination, sizeof(destination));
    return 0;
}

bool A64Decoder::TryScalarPairwiseFP(const Instruction* instr) {
    u32 bits, operation;
    if (!DecodePairwise(instr->GetInstructionBits(), bits, operation)) return false;
    auto context = assembler->GetUniformAddress(ir::Imm{u32{0}}).SetType(ir::ValueType::U64);
    assembler->CallHostFPFree(&ExecuteScalarPairwiseFP, context,
        ImmValue(instr->GetInstructionBits(), ir::ValueType::U64),
        ImmValue(current_pc, ir::ValueType::U64)).SetType(ir::ValueType::U64);
    return true;
}
bool detail::AdmitScalarPairwiseFP(u64 target, u32 word) noexcept {
    u32 bits, operation;
    return SingleInstructionTarget(target, &ExecuteScalarPairwiseFP) && DecodePairwise(word, bits, operation);
}

}  // namespace swift::arm64
