#include "arm64_frontend_internal.h"
#include "single_instruction_admission.h"
#include "fp_helpers.h"
#include "runtime/backend/context.h"
#include <cstring>

namespace swift::arm64 {
namespace {
bool DecodeFixedSIMD(u32 word,u32& operation,u32& bits,u32& fbits) {
    switch ((word & ~(NEON_Q|NEONScalar)) & NEONShiftImmediateMask) {
        case NEON_SCVTF_imm: operation = 0;break;
        case NEON_UCVTF_imm: operation = 1;break;
        case NEON_FCVTZS_imm: operation = 8;break;
        case NEON_FCVTZU_imm: operation = 9;break;
        default: return false;
    }
    const u32 immediate = (word>>16)&127,immh = immediate>>3;
    const int highest = vixl::HighestSetBitPosition(immh);
    bits = highest >= 0 ? 8u << highest : 0;
    fbits = 2*bits-immediate;
    return true;
}

u64 ExecuteFixedSIMD(u64 context_address,u64 instruction,u64 guest_pc) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context_address);
    const u32 word = u32(instruction);
    u32 operation,bits,fbits;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeFixedSIMD(word,operation,bits,fbits)) {
        auto& state = *reinterpret_cast<runtime::backend::State*>(context_address-runtime::backend::state_offset_uniform_buffer);
        ctx.pc = guest_pc;ctx.padding = u32(InterruptReason::FALLBACK);
        state.current_loc = runtime::ir::Location{guest_pc};state.halt_reason = runtime::HaltReason::CallHost;
        return 0;
    }
    const u32 rn = (word>>5)&31,rd = word&31;
    u8 source[16],destination[16]{};std::memcpy(source,&ctx.v[rn],sizeof(source));
    const u32 active = word&NEONScalar ? 1 : (word&NEON_Q ? 128 : 64)/bits;
    u32 exceptions = 0;
    for (u32 lane=0;lane<active;++lane) {
        u64 value = 0;std::memcpy(&value,source+lane*bits/8,bits/8);
        const auto result = EvaluateIntegerFP(value,bits,operation,ctx.fpcr,fbits);
        std::memcpy(destination+lane*bits/8,&result.value,bits/8);exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;std::memcpy(&ctx.v[rd],destination,sizeof(destination));return 0;
}
}

bool A64Decoder::TryNEONFPFixedSIMD(const Instruction* instr) {
    const u32 word = instr->GetInstructionBits();
    u32 operation,bits,fbits;
    if (!DecodeFixedSIMD(word,operation,bits,fbits)) return false;
    if (bits < 16 || bits > 64 || !fbits || fbits > bits ||
        (bits == 64 && !(word&NEON_Q))) {
        Interrupt(InterruptReason::ILL_CODE,current_pc);return true;
    }
    auto context = assembler->GetUniformAddress(ir::Imm{u32{0}}).SetType(ir::ValueType::U64);
    assembler->CallHostFPFree(&ExecuteFixedSIMD,context,ImmValue(word,ir::ValueType::U64),ImmValue(current_pc,ir::ValueType::U64)).SetType(ir::ValueType::U64);
    return true;
}
bool detail::AdmitFixedSIMD(u64 target, u32 word) noexcept {
    u32 operation, bits, fbits;
    return SingleInstructionTarget(target, &ExecuteFixedSIMD) && DecodeFixedSIMD(word, operation, bits, fbits) &&
           bits >= 16 && bits <= 64 && fbits && fbits <= bits && (bits != 64 || (word & NEON_Q));
}

}  // namespace swift::arm64
