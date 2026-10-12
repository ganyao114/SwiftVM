#include "arm64_frontend_internal.h"
#include "single_instruction_admission.h"
#include "fp_helpers.h"
#include "runtime/backend/context.h"
#include <cstring>

namespace swift::arm64 {
namespace {
bool DecodeReduction(u32 word,bool half,bool across,u32& operation) {
    const u32 mask = across ? (half ? NEONAcrossLanesFP16Mask : NEONAcrossLanesFPMask)
                            : (half ? NEON3SameFP16Mask : NEON3SameFPMask);
    const u32 code = word&mask;
    if (across) {
        if (code == u32(half ? NEON_FMAXV_H : NEON_FMAXV)) operation = 4;
        else if (code == u32(half ? NEON_FMINV_H : NEON_FMINV)) operation = 5;
        else if (code == u32(half ? NEON_FMAXNMV_H : NEON_FMAXNMV)) operation = 6;
        else if (code == u32(half ? NEON_FMINNMV_H : NEON_FMINNMV)) operation = 7;
        else return false;
    } else {
        if (code == u32(half ? NEON_FADDP_H : NEON_FADDP)) operation = 2;
        else if (code == u32(half ? NEON_FMAXP_H : NEON_FMAXP)) operation = 4;
        else if (code == u32(half ? NEON_FMINP_H : NEON_FMINP)) operation = 5;
        else if (code == u32(half ? NEON_FMAXNMP_H : NEON_FMAXNMP)) operation = 6;
        else if (code == u32(half ? NEON_FMINNMP_H : NEON_FMINNMP)) operation = 7;
        else return false;
    }
    return true;
}

template<bool Half,bool Across>
u64 ExecuteReduction(u64 context_address,u64 instruction,u64 guest_pc) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context_address);
    const u32 word = u32(instruction);
    u32 operation;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeReduction(word,Half,Across,operation)) {
        auto& state = *reinterpret_cast<runtime::backend::State*>(context_address-runtime::backend::state_offset_uniform_buffer);
        ctx.pc = guest_pc;ctx.padding = u32(InterruptReason::FALLBACK);
        state.current_loc = runtime::ir::Location{guest_pc};state.halt_reason = runtime::HaltReason::CallHost;
        return 0;
    }
    const u32 bits = Half ? 16 : word&(1u<<22) ? 64 : 32;
    const u32 active = (word&NEON_Q ? 128 : 64)/bits;
    const u32 rn = (word>>5)&31,rm = (word>>16)&31,rd = word&31;
    u8 lhs[16],rhs[16]{},destination[16]{};
    std::memcpy(lhs,&ctx.v[rn],sizeof(lhs));
    if (!Across) std::memcpy(rhs,&ctx.v[rm],sizeof(rhs));
    auto element = [&](const u8* source,u32 lane) { u64 value = 0;std::memcpy(&value,source+lane*bits/8,bits/8);return value; };
    u32 exceptions = 0;
    auto combine = [&](u64 a,u64 b) {
        const auto result = EvaluateBinaryFP(a,b,bits,operation,ctx.fpcr);
        exceptions |= result.exceptions;return result.value;
    };
    if (Across) {
        u64 values[8]{};
        for (u32 lane=0;lane<active;++lane) values[lane] = element(lhs,lane);
        // Recursively combine adjacent halves. With 8H this is
        // ((01,23),(45,67)); intermediate NaN quieting is observable.
        for (u32 remaining=active;remaining>1;remaining/=2)
            for (u32 lane=0;lane<remaining/2;++lane) values[lane] = combine(values[lane*2],values[lane*2+1]);
        std::memcpy(destination,&values[0],bits/8);
    } else {
        for (u32 lane=0;lane<active;++lane) {
            const u8* source = lane<active/2 ? lhs : rhs;
            const u32 index = (lane%(active/2))*2;
            const u64 value = combine(element(source,index),element(source,index+1));
            std::memcpy(destination+lane*bits/8,&value,bits/8);
        }
    }
    ctx.fpsr |= exceptions;std::memcpy(&ctx.v[rd],destination,sizeof(destination));
    return 0;
}
}

bool A64Decoder::TryNEONFPReduction(const Instruction* instr,bool half,bool across) {
    const u32 word = instr->GetInstructionBits();
    u32 operation;
    if (!DecodeReduction(word,half,across,operation)) return false;
    if ((!across && (word&NEONScalar)) || (!half &&
        (across ? (!(word&NEON_Q) || (word&(1u<<22))) : ((word&(1u<<22)) && !(word&NEON_Q))))) {
        Interrupt(InterruptReason::ILL_CODE,current_pc);return true;
    }
    auto context = assembler->GetUniformAddress(ir::Imm{u32{0}}).SetType(ir::ValueType::U64);
    auto function = half ? (across ? &ExecuteReduction<true,true> : &ExecuteReduction<true,false>)
                         : (across ? &ExecuteReduction<false,true> : &ExecuteReduction<false,false>);
    assembler->CallHostFPFree(function,context,ImmValue(word,ir::ValueType::U64),ImmValue(current_pc,ir::ValueType::U64)).SetType(ir::ValueType::U64);
    return true;
}
bool detail::AdmitReduction(u64 target, u32 word) noexcept {
    const bool half = SingleInstructionTarget(target, &ExecuteReduction<true, true>) ||
                      SingleInstructionTarget(target, &ExecuteReduction<true, false>);
    const bool across = SingleInstructionTarget(target, &ExecuteReduction<true, true>) ||
                        SingleInstructionTarget(target, &ExecuteReduction<false, true>);
    if (!half && !across && !SingleInstructionTarget(target, &ExecuteReduction<false, false>)) return false;
    u32 operation;
    return DecodeReduction(word, half, across, operation) &&
           (across || !(word & NEONScalar)) &&
           (half || (across ? (word & NEON_Q) && !(word & (1u << 22))
                           : !(word & (1u << 22)) || (word & NEON_Q)));
}

}  // namespace swift::arm64
