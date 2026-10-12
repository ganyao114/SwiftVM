#include "arm64_frontend_internal.h"
#include "single_instruction_admission.h"
#include "fp_helpers.h"
#include "runtime/backend/context.h"
#include <cstring>

namespace swift::arm64 {
namespace {
enum class Operation { Widen, Narrow, Odd };
bool DecodePrecision(u32 word, Operation& operation) {
    switch (word & NEON2RegMiscFPMask) {
        case NEON_FCVTL: operation = Operation::Widen; break;
        case NEON_FCVTN: operation = Operation::Narrow; break;
        case NEON_FCVTXN: operation = Operation::Odd; break;
        default: return false;
    }
    return true;
}

u64 ExecutePrecision(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context_address);
    const u32 word = u32(instruction);
    Operation operation;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodePrecision(word, operation)) {
        auto& state = *reinterpret_cast<runtime::backend::State*>(
            context_address - runtime::backend::state_offset_uniform_buffer);
        ctx.pc = guest_pc;
        ctx.padding = u32(InterruptReason::FALLBACK);
        state.current_loc = runtime::ir::Location{guest_pc};
        state.halt_reason = runtime::HaltReason::CallHost;
        return 0;
    }
    const u32 small = word & (1u << 22) ? 32 : 16;
    const bool widen = operation == Operation::Widen, upper = word & NEON_Q;
    const u32 source_bits = widen ? small : small*2, destination_bits = widen ? small*2 : small;
    const u32 rn = (word >> 5) & 31, rd = word & 31;
    u8 source[16], destination[16]{};
    std::memcpy(source, &ctx.v[rn], sizeof(source));
    // Narrow2 preserves the old lower half, including when Rd aliases Rn.
    if (!widen && upper) std::memcpy(destination, &ctx.v[rd], 8);
    const u32 active = 64/small, source_offset = widen && upper ? 8 : 0, destination_offset = !widen && upper ? 8 : 0;
    u32 exceptions = 0;
    for (u32 lane = 0; lane < active; ++lane) {
        u64 value = 0;
        std::memcpy(&value, source + source_offset + lane*source_bits/8, source_bits/8);
        const auto result = EvaluatePrecisionFP(value, source_bits, destination_bits, operation == Operation::Odd, ctx.fpcr);
        std::memcpy(destination + destination_offset + lane*destination_bits/8, &result.value, destination_bits/8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.v[rd], destination, sizeof(destination));
    return 0;
}
}

bool A64Decoder::TryNEONFPPrecision(const Instruction* instr) {
    const u32 word = instr->GetInstructionBits();
    Operation operation;
    if (!DecodePrecision(word, operation)) return false;
    if (operation == Operation::Odd && !(word & (1u << 22))) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    auto context = assembler->GetUniformAddress(ir::Imm{u32{0}}).SetType(ir::ValueType::U64);
    assembler->CallHostFPFree(&ExecutePrecision, context,
        ImmValue(word, ir::ValueType::U64), ImmValue(current_pc, ir::ValueType::U64)).SetType(ir::ValueType::U64);
    return true;
}
bool detail::AdmitPrecision(u64 target, u32 word) noexcept {
    Operation operation;
    return SingleInstructionTarget(target, &ExecutePrecision) && DecodePrecision(word, operation) &&
           (operation != Operation::Odd || (word & (1u << 22)));
}

}  // namespace swift::arm64
