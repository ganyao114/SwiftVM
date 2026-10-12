#include "arm64_frontend_internal.h"
#include "single_instruction_admission.h"
#include "fp_helpers.h"
#include "runtime/backend/context.h"
#include <cstring>

namespace swift::arm64 {
namespace {
bool DecodeFPInteger(u32 word, bool half, u32& operation) {
    word &= ~(NEONScalar | NEON_Q);
    const u32 code = word & (half ? NEON2RegMiscFP16Mask : NEON2RegMiscFPMask);
    const u32 bases[]{NEON_SCVTF,NEON_UCVTF,NEON_FCVTNS,NEON_FCVTNU,NEON_FCVTMS,NEON_FCVTMU,
                     NEON_FCVTPS,NEON_FCVTPU,NEON_FCVTZS,NEON_FCVTZU,NEON_FCVTAS,NEON_FCVTAU};
    const u32 halves[]{NEON_SCVTF_H,NEON_UCVTF_H,NEON_FCVTNS_H,NEON_FCVTNU_H,NEON_FCVTMS_H,NEON_FCVTMU_H,
                      NEON_FCVTPS_H,NEON_FCVTPU_H,NEON_FCVTZS_H,NEON_FCVTZU_H,NEON_FCVTAS_H,NEON_FCVTAU_H};
    for (u32 index = 0; index < 12; ++index)
        if (code == (half ? halves : bases)[index]) { operation = index; return true; }
    return false;
}

template<bool Half>
u64 ExecuteFPInteger(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context_address);
    const u32 word = u32(instruction);
    u32 operation;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeFPInteger(word, Half, operation)) {
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
    const u32 active = word & NEONScalar ? 1 : (word & NEON_Q ? 128 : 64) / bits;
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        u64 value = 0;
        std::memcpy(&value, source + lane * bits / 8, bits / 8);
        const auto result = EvaluateIntegerFP(value, bits, operation, ctx.fpcr);
        std::memcpy(destination + lane * bits / 8, &result.value, bits / 8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.v[rd], destination, sizeof(destination));
    return 0;
}
}

bool A64Decoder::TryNEONFPInteger(const Instruction* instr, bool half) {
    const u32 word = instr->GetInstructionBits();
    u32 operation;
    if (!DecodeFPInteger(word, half, operation)) return false;
    if (!half && (word & (1u << 22)) && !(word & NEON_Q)) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    auto context = assembler->GetUniformAddress(ir::Imm{u32{0}}).SetType(ir::ValueType::U64);
    assembler->CallHostFPFree(half ? &ExecuteFPInteger<true> : &ExecuteFPInteger<false>, context,
        ImmValue(word, ir::ValueType::U64), ImmValue(current_pc, ir::ValueType::U64)).SetType(ir::ValueType::U64);
    return true;
}
bool detail::AdmitFPInteger(u64 target, u32 word) noexcept {
    const bool half = SingleInstructionTarget(target, &ExecuteFPInteger<true>);
    if (!half && !SingleInstructionTarget(target, &ExecuteFPInteger<false>)) return false;
    u32 operation;
    return DecodeFPInteger(word, half, operation) &&
           (half || !(word & (1u << 22)) || (word & NEON_Q));
}

}  // namespace swift::arm64
