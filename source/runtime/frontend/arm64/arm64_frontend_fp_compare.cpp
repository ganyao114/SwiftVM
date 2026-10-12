#include "arm64_frontend_internal.h"
#include "single_instruction_admission.h"
#include "fp_helpers.h"
#include "runtime/backend/context.h"
#include <cstring>

namespace swift::arm64 {
namespace {
bool DecodeFPCompare(u32 word, bool half, bool zero, FPCompareOperation& operation) {
    // Scalar forms share the vector opcode after removing their class bits.
    word &= ~(NEONScalar | NEON_Q);
    const u32 mask = half ? (zero ? NEON2RegMiscFP16Mask : NEON3SameFP16Mask)
                         : (zero ? NEON2RegMiscFPMask : NEON3SameFPMask);
    const u32 code = word & mask;
    if (zero) {
        if (code == u32(half ? NEON_FCMEQ_H_zero : NEON_FCMEQ_zero)) operation = FPCompareOperation::Eq;
        else if (code == u32(half ? NEON_FCMGE_H_zero : NEON_FCMGE_zero)) operation = FPCompareOperation::Ge;
        else if (code == u32(half ? NEON_FCMGT_H_zero : NEON_FCMGT_zero)) operation = FPCompareOperation::Gt;
        else if (code == u32(half ? NEON_FCMLE_H_zero : NEON_FCMLE_zero)) operation = FPCompareOperation::Le;
        else if (code == u32(half ? NEON_FCMLT_H_zero : NEON_FCMLT_zero)) operation = FPCompareOperation::Lt;
        else return false;
    } else {
        if (code == u32(half ? NEON_FCMEQ_H : NEON_FCMEQ)) operation = FPCompareOperation::Eq;
        else if (code == u32(half ? NEON_FCMGE_H : NEON_FCMGE)) operation = FPCompareOperation::Ge;
        else if (code == u32(half ? NEON_FCMGT_H : NEON_FCMGT)) operation = FPCompareOperation::Gt;
        else if (code == u32(half ? NEON_FACGE_H : NEON_FACGE)) operation = FPCompareOperation::AbsGe;
        else if (code == u32(half ? NEON_FACGT_H : NEON_FACGT)) operation = FPCompareOperation::AbsGt;
        else return false;
    }
    return true;
}

template<bool Half, bool Zero>
u64 ExecuteFPCompare(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context_address);
    const u32 word = u32(instruction);
    FPCompareOperation operation;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeFPCompare(word, Half, Zero, operation)) {
        auto& state = *reinterpret_cast<runtime::backend::State*>(
            context_address - runtime::backend::state_offset_uniform_buffer);
        ctx.pc = guest_pc;
        ctx.padding = u32(InterruptReason::FALLBACK);
        state.current_loc = runtime::ir::Location{guest_pc};
        state.halt_reason = runtime::HaltReason::CallHost;
        return 0;
    }
    const u32 bits = Half ? 16 : word & (1u << 22) ? 64 : 32;
    const u32 rn = (word >> 5) & 31, rm = (word >> 16) & 31, rd = word & 31;
    u8 lhs[16], rhs[16]{}, destination[16]{};
    std::memcpy(lhs, &ctx.v[rn], sizeof(lhs));
    if (!Zero) std::memcpy(rhs, &ctx.v[rm], sizeof(rhs));
    const u32 active = word & NEONScalar ? 1 : (word & NEON_Q ? 128 : 64) / bits;
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        u64 a = 0, b = 0;
        std::memcpy(&a, lhs + lane * bits / 8, bits / 8);
        std::memcpy(&b, rhs + lane * bits / 8, bits / 8);
        const auto result = EvaluateCompareFP(a, b, bits, operation, ctx.fpcr);
        std::memcpy(destination + lane * bits / 8, &result.value, bits / 8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.v[rd], destination, sizeof(destination));
    return 0;
}
}

bool A64Decoder::TryNEONFPCompare(const Instruction* instr, bool half, bool zero) {
    const u32 word = instr->GetInstructionBits();
    FPCompareOperation operation;
    if (!DecodeFPCompare(word, half, zero, operation)) return false;
    if (!half && (word & (1u << 22)) && !(word & NEON_Q)) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    auto context = assembler->GetUniformAddress(ir::Imm{u32{0}}).SetType(ir::ValueType::U64);
    auto function = half ? (zero ? &ExecuteFPCompare<true, true> : &ExecuteFPCompare<true, false>)
                         : (zero ? &ExecuteFPCompare<false, true> : &ExecuteFPCompare<false, false>);
    assembler->CallHostFPFree(function, context,
        ImmValue(word, ir::ValueType::U64), ImmValue(current_pc, ir::ValueType::U64)).SetType(ir::ValueType::U64);
    return true;
}
bool detail::AdmitFPCompare(u64 target, u32 word) noexcept {
    if (SingleInstructionTarget(target, &ExecuteFPCompare<true, true>)) {
        FPCompareOperation operation;
        return DecodeFPCompare(word, true, true, operation);
    }
    if (SingleInstructionTarget(target, &ExecuteFPCompare<true, false>)) {
        FPCompareOperation operation;
        return DecodeFPCompare(word, true, false, operation);
    }
    const bool zero = SingleInstructionTarget(target, &ExecuteFPCompare<false, true>);
    if (!zero && !SingleInstructionTarget(target, &ExecuteFPCompare<false, false>)) return false;
    FPCompareOperation operation;
    return DecodeFPCompare(word, false, zero, operation) &&
           (!(word & (1u << 22)) || (word & NEON_Q));
}

}  // namespace swift::arm64
