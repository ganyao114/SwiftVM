#include "arm64_frontend_internal.h"
#include "single_instruction_admission.h"
#include "fp_helpers.h"
#include "runtime/backend/context.h"
#include <cstring>

namespace swift::arm64 {
namespace {
bool DecodeRangeRound(u32 word,u32& operation,u32& integer_bits,bool& scalar) {
    scalar = word & (1u<<28);
    if (scalar) {
        switch (word & (FPDataProcessing1SourceMask & ~u32(FP64))) {
            case FRINT32Z_s: operation = FRINTZ_s;integer_bits = 32;break;
            case FRINT32X_s: operation = FRINTX_s;integer_bits = 32;break;
            case FRINT64Z_s: operation = FRINTZ_s;integer_bits = 64;break;
            case FRINT64X_s: operation = FRINTX_s;integer_bits = 64;break;
            default: return false;
        }
    } else {
        switch (word & NEON2RegMiscFPMask) {
            case NEON_FRINT32Z: operation = FRINTZ_s;integer_bits = 32;break;
            case NEON_FRINT32X: operation = FRINTX_s;integer_bits = 32;break;
            case NEON_FRINT64Z: operation = FRINTZ_s;integer_bits = 64;break;
            case NEON_FRINT64X: operation = FRINTX_s;integer_bits = 64;break;
            default: return false;
        }
    }
    return true;
}

u64 ExecuteRangeRound(u64 context_address,u64 instruction,u64 guest_pc) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context_address);
    const u32 word = u32(instruction);u32 operation,integer_bits;bool scalar;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeRangeRound(word,operation,integer_bits,scalar)) {
        auto& state = *reinterpret_cast<runtime::backend::State*>(context_address-runtime::backend::state_offset_uniform_buffer);
        ctx.pc = guest_pc;ctx.padding = u32(InterruptReason::FALLBACK);
        state.current_loc = runtime::ir::Location{guest_pc};state.halt_reason = runtime::HaltReason::CallHost;
        return 0;
    }
    const u32 bits = word & (1u<<22) ? 64 : 32,rn = (word>>5)&31,rd = word&31;
    u8 source[16],destination[16]{};std::memcpy(source,&ctx.v[rn],sizeof(source));
    const u32 active = scalar ? 1 : (word&NEON_Q ? 128 : 64)/bits;
    u32 exceptions = 0;
    for (u32 lane=0;lane<active;++lane) {
        u64 value = 0;std::memcpy(&value,source+lane*bits/8,bits/8);
        const auto result = EvaluateUnaryFP(value,bits,operation,ctx.fpcr,integer_bits);
        std::memcpy(destination+lane*bits/8,&result.value,bits/8);exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;std::memcpy(&ctx.v[rd],destination,sizeof(destination));return 0;
}
}

bool A64Decoder::TryFRINTRange(const Instruction* instr) {
    const u32 word = instr->GetInstructionBits();u32 operation,integer_bits;bool scalar;
    if (!DecodeRangeRound(word,operation,integer_bits,scalar)) return false;
    if (!scalar && (word & (1u<<22)) && !(word&NEON_Q)) {
        Interrupt(InterruptReason::ILL_CODE,current_pc);return true;
    }
    auto context = assembler->GetUniformAddress(ir::Imm{u32{0}}).SetType(ir::ValueType::U64);
    assembler->CallHostFPFree(&ExecuteRangeRound,context,ImmValue(word,ir::ValueType::U64),ImmValue(current_pc,ir::ValueType::U64)).SetType(ir::ValueType::U64);
    return true;
}
bool detail::AdmitRangeRound(u64 target, u32 word) noexcept {
    u32 operation, integer_bits;
    bool scalar;
    return SingleInstructionTarget(target, &ExecuteRangeRound) &&
           DecodeRangeRound(word, operation, integer_bits, scalar) &&
           (scalar || !(word & (1u << 22)) || (word & NEON_Q));
}

}  // namespace swift::arm64
