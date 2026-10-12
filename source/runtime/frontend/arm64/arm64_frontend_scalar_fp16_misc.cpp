#include "arm64_frontend_internal.h"
#include "single_instruction_admission.h"
#include "fp_helpers.h"
#include "runtime/backend/context.h"
#include <cstring>

namespace swift::arm64 {
namespace {
enum class Operation { Eq, Gt, Ge, Lt, Le, RecipExponent };
bool DecodeScalarFP16Misc(u32 word, Operation& operation) {
    switch (word & NEONScalar2RegMiscFP16Mask) {
        case NEON_FCMEQ_H_zero_scalar: operation = Operation::Eq; break;
        case NEON_FCMGT_H_zero_scalar: operation = Operation::Gt; break;
        case NEON_FCMGE_H_zero_scalar: operation = Operation::Ge; break;
        case NEON_FCMLT_H_zero_scalar: operation = Operation::Lt; break;
        case NEON_FCMLE_H_zero_scalar: operation = Operation::Le; break;
        case NEON_FRECPX_H_scalar: operation = Operation::RecipExponent; break;
        default: return false;
    }
    return true;
}

u64 ExecuteScalarFP16Misc(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context_address);
    Operation operation;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeScalarFP16Misc(u32(instruction), operation)) {
        auto& state = *reinterpret_cast<runtime::backend::State*>(
            context_address - runtime::backend::state_offset_uniform_buffer);
        ctx.pc = guest_pc;
        ctx.padding = u32(InterruptReason::FALLBACK);
        state.current_loc = runtime::ir::Location{guest_pc};
        state.halt_reason = runtime::HaltReason::CallHost;
        return 0;
    }
    const u32 rn = (instruction >> 5) & 31, rd = instruction & 31;
    u16 source;
    std::memcpy(&source, &ctx.v[rn], sizeof(source));
    const u16 sign = source & 0x8000;
    const u32 exponent = (source >> 10) & 31, fraction = source & 1023;
    u16 result;
    u32 exceptions = 0;
    if (exponent == 31 && fraction) {
        const bool signaling = !(fraction & 512);
        if (operation == Operation::RecipExponent) {
            result = ctx.fpcr & (1u << 25) ? 0x7E00 : source | 512;
            exceptions = signaling ? 1 : 0;
        } else {
            result = 0;
            exceptions = signaling || operation != Operation::Eq ? 1 : 0;
        }
    } else if (operation == Operation::RecipExponent) {
        // FRECPX complements the biased exponent; zero/subnormal exponent
        // uses max finite exponent. Fraction bits do not affect the result.
        result = sign | ((exponent == 0 ? 30 : 31 - exponent) << 10);
    } else {
        if (!exponent && fraction && (ctx.fpcr & (1u << 19))) source = sign;
        const bool zero = !(source & 0x7FFF), negative = sign != 0;
        bool match;
        switch (operation) {
            case Operation::Eq: match = zero; break;
            case Operation::Gt: match = !zero && !negative; break;
            case Operation::Ge: match = zero || !negative; break;
            case Operation::Lt: match = !zero && negative; break;
            default: match = zero || negative; break;
        }
        result = match ? 0xFFFF : 0;
    }
    ctx.fpsr |= exceptions;
    const u64 destination[] = {result, 0};
    std::memcpy(&ctx.v[rd], destination, sizeof(destination));
    return 0;
}
}

void A64Decoder::VisitNEONScalar2RegMiscFP16(const Instruction* instr) {
    if (TryNEONFPEstimate(instr, true)) return;
    if (TryNEONFPInteger(instr, true)) return;
    Operation operation;
    if (!DecodeScalarFP16Misc(instr->GetInstructionBits(), operation)) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    auto context = assembler->GetUniformAddress(ir::Imm{u32{0}}).SetType(ir::ValueType::U64);
    assembler->CallHostFPFree(&ExecuteScalarFP16Misc, context,
        ImmValue(instr->GetInstructionBits(), ir::ValueType::U64),
        ImmValue(current_pc, ir::ValueType::U64)).SetType(ir::ValueType::U64);
}
bool detail::AdmitScalarFP16Misc(u64 target, u32 word) noexcept {
    Operation operation;
    return SingleInstructionTarget(target, &ExecuteScalarFP16Misc) && DecodeScalarFP16Misc(word, operation);
}

}  // namespace swift::arm64
