#include "translator.h"
#include <array>
#include <algorithm>
#include <stdexcept>
#include "runtime/common/unsigned_estimate.h"

namespace swift::runtime::backend::x64 {
using O = ir::OpCode;
using namespace Xbyak;

void Translator::EmitVecUnsignedEstimate(ir::Inst* inst) {
    const auto operation = ValidateUnsignedEstimate(inst);
    static_assert(sizeof(kReciprocalMidpoints) == 512 && sizeof(kReciprocalRootMidpoints) == 512);
    static_assert(sizeof(kReciprocalRootMidpoints[0]) == 256);
    // Read the complete container before any reused home is written.
    // XMM2 remains reserved for one-use vector forwarding. Build all
    // lane indices and saturation masks in parallel with SSE2, then
    // retain the mask in XMM1 across the four scalar table loads.
    ReadVector(xmm0, Source(inst, 0));
    movdqa(xmm1, xmm0);
    if (!operation) {
        psrad(xmm1, 31); pcmpeqd(xmm3, xmm3); pxor(xmm1, xmm3);
        psrld(xmm0, 23);
        mov(eax, 255); movd(xmm6, eax); pshufd(xmm6, xmm6, 0);
        pand(xmm0, xmm6);
    } else {
        psrld(xmm1, 30); pxor(xmm5, xmm5); pcmpeqd(xmm1, xmm5);
        movdqa(xmm3, xmm0); psrad(xmm3, 31);
        movdqa(xmm4, xmm0); psrld(xmm4, 23); psrld(xmm0, 24);
        mov(eax, 127); movd(xmm6, eax); pshufd(xmm6, xmm6, 0);
        pand(xmm4, xmm6); pand(xmm0, xmm6);
        mov(eax, 128); movd(xmm5, eax); pshufd(xmm5, xmm5, 0); por(xmm4, xmm5);
        // High lanes select parity0; lower lanes select parity1 in
        // the second 128-entry half. Saturated indices stay in range.
        pand(xmm0, xmm3); pandn(xmm3, xmm4); por(xmm0, xmm3);
    }
    // Both table shapes occupy exactly 512 contiguous bytes. Keep
    // one base live across all lanes; every masked index is in range
    // even for inputs whose final result saturates.
    mov(rdx, operation ? reinterpret_cast<u64>(kReciprocalRootMidpoints.data()) :
                         reinterpret_cast<u64>(kReciprocalMidpoints.data()));
    const std::array<Xmm, 4> estimates{xmm3, xmm4, xmm5, xmm6};
    for (u32 lane = 0; lane < 4; ++lane) {
        // Each packed index fits one word. Extract directly so the
        // independent loads avoid stack store-forward dependencies.
        pextrw(eax, xmm0, lane * 2);
        movzx(eax, word[rdx + rax * 2]); movd(estimates[lane], eax);
    }
    punpckldq(xmm3, xmm4); punpckldq(xmm5, xmm6); punpcklqdq(xmm3, xmm5);
    pslld(xmm3, 23); por(xmm3, xmm1);
    WriteVector(inst, xmm3); return;
}

void Translator::EmitVecDot(ir::Inst* inst) {
    const u64 sign = inst->GetArg<ir::Imm>(3).Get();
    if (sign > 1) throw std::runtime_error("x64 VecDot signed selector must be 0 or 1");
    if (inst->ReturnType() != ir::ValueType::V128)
        throw std::runtime_error("x64 VecDot requires V128 result and inputs");
    for (u32 index = 0; index < 3; ++index)
        if (Source(inst, index).Type() != ir::ValueType::V128 || Source(inst, index).Def()->ReturnType() != ir::ValueType::V128)
            throw std::runtime_error("x64 VecDot requires V128 result and inputs");
    // Widen bytes before PMADDWD: unsigned 255*255 is exact here,
    // whereas PMADDUBSW would saturate intermediate pair sums.
    ReadVector(xmm0, Source(inst, 0)); ReadVector(xmm1, Source(inst, 1)); ReadVector(xmm3, Source(inst, 2));
    movdqa(xmm4, xmm0); movdqa(xmm5, xmm1);
    pxor(xmm6, xmm6);
    if (sign) {
        pcmpgtb(xmm6, xmm0); pxor(xmm7, xmm7); pcmpgtb(xmm7, xmm1);
        punpcklbw(xmm0, xmm6); punpckhbw(xmm4, xmm6);
        punpcklbw(xmm1, xmm7); punpckhbw(xmm5, xmm7);
    } else {
        punpcklbw(xmm0, xmm6); punpckhbw(xmm4, xmm6);
        punpcklbw(xmm1, xmm6); punpckhbw(xmm5, xmm6);
    }
    pmaddwd(xmm0, xmm1); pmaddwd(xmm4, xmm5);
    // Each half now has four pair sums. Sum adjacent pairs and
    // select lanes 0,2 into the low qword for two four-byte dots.
    movdqa(xmm1, xmm0); psrldq(xmm1, 4); paddd(xmm0, xmm1); pshufd(xmm0, xmm0, 0x88);
    movdqa(xmm5, xmm4); psrldq(xmm5, 4); paddd(xmm4, xmm5); pshufd(xmm4, xmm4, 0x88);
    punpcklqdq(xmm0, xmm4); paddd(xmm0, xmm3);
    WriteVector(inst, xmm0); return;
}

void Translator::EmitVec4And(ir::Inst* inst) {
    ReadVectorOperands(inst); pand(xmm0, xmm1); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecAnd(ir::Inst* inst) { EmitVec4And(inst); }

void Translator::EmitVec4Or(ir::Inst* inst) {
    ReadVectorOperands(inst); por(xmm0, xmm1); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecOr(ir::Inst* inst) { EmitVec4Or(inst); }

void Translator::EmitVecXor(ir::Inst* inst) {
    ReadVectorOperands(inst); pxor(xmm0, xmm1); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecAndNot(ir::Inst* inst) {
    ReadVectorOperands(inst); pandn(xmm1, xmm0); movdqa(xmm0, xmm1); WriteVector(inst, xmm0); return;
}

void Translator::EmitVectorAddSub(ir::Inst* inst, ir::OpCode op) {
    const auto bits = (op == O::Vec4Add || op == O::Vec4Sub) ? 32 : VectorImmediate(inst, 2);
    ValidateLaneWidth(bits); ReadVectorOperands(inst);
    if (op == O::Vec4Add || op == O::VecAdd) {
        if (bits == 8) paddb(xmm0, xmm1); else if (bits == 16) paddw(xmm0, xmm1);
        else if (bits == 32) paddd(xmm0, xmm1); else paddq(xmm0, xmm1);
    } else {
        if (bits == 8) psubb(xmm0, xmm1); else if (bits == 16) psubw(xmm0, xmm1);
        else if (bits == 32) psubd(xmm0, xmm1); else psubq(xmm0, xmm1);
    }
    WriteVector(inst, xmm0); return;
}

void Translator::EmitVec4Add(ir::Inst* inst) { EmitVectorAddSub(inst, O::Vec4Add); }

void Translator::EmitVecAdd(ir::Inst* inst) { EmitVectorAddSub(inst, O::VecAdd); }

void Translator::EmitVec4Sub(ir::Inst* inst) { EmitVectorAddSub(inst, O::Vec4Sub); }

void Translator::EmitVecSub(ir::Inst* inst) { EmitVectorAddSub(inst, O::VecSub); }

void Translator::EmitVectorCompare(ir::Inst* inst, bool equal) {
    const auto bits = VectorImmediate(inst, 2);
    ValidateLaneWidth(bits);
    ReadVectorOperands(inst);
    if (equal) {
        if (bits == 8) pcmpeqb(xmm0, xmm1); else if (bits == 16) pcmpeqw(xmm0, xmm1); else pcmpeqd(xmm0, xmm1);
        if (bits == 64) {
            // A qword matches only if both constituent dwords
            // match. Duplicate that conjunction across the lane.
            pshufd(xmm3, xmm0, 0xb1); pand(xmm0, xmm3);
        }
    } else {
        if (bits == 8) pcmpgtb(xmm0, xmm1);
        else if (bits == 16) pcmpgtw(xmm0, xmm1);
        else if (bits == 32) pcmpgtd(xmm0, xmm1);
        else {
            // Signed qword order is signed high-dword order,
            // followed by unsigned low-dword order on equal
            // highs. SSE2 has signed dword compares; bias the
            // lows with bit31 to express unsigned order.
            movdqa(xmm3, xmm0); pcmpgtd(xmm3, xmm1); pshufd(xmm3, xmm3, 0xf5);
            movdqa(xmm4, xmm0); pcmpeqd(xmm4, xmm1); pshufd(xmm4, xmm4, 0xf5);
            pcmpeqd(xmm5, xmm5); pslld(xmm5, 31);
            pxor(xmm0, xmm5); pxor(xmm1, xmm5); pcmpgtd(xmm0, xmm1); pshufd(xmm0, xmm0, 0xa0);
            pand(xmm0, xmm4); por(xmm0, xmm3);
        }
    }
    WriteVector(inst, xmm0); return;

}

void Translator::EmitVecCmpEq(ir::Inst* inst) { EmitVectorCompare(inst, true); }
void Translator::EmitVecCmpGt(ir::Inst* inst) { EmitVectorCompare(inst, false); }

void Translator::EmitVectorMultiply(ir::Inst* inst, u32 bits) {
    ValidateLaneWidth(bits);
    if (bits == 16) {
        ReadVectorOperands(inst); pmullw(xmm0, xmm1); WriteVector(inst, xmm0); return;
    }
    if (bits == 32) {
        ReadVectorOperands(inst);
        // PMULUDQ produces the full products of lanes 0 and 2.
        // Shift each qword to select lanes 1 and 3, then interleave
        // only the low dwords: signed/unsigned multiplication has
        // the same low 32 bits.
        movdqa(xmm3, xmm0); movdqa(xmm4, xmm1);
        pmuludq(xmm0, xmm1); psrlq(xmm3, 32); psrlq(xmm4, 32); pmuludq(xmm3, xmm4);
        pshufd(xmm0, xmm0, 0x88); pshufd(xmm3, xmm3, 0x88); punpckldq(xmm0, xmm3);
        WriteVector(inst, xmm0); return;
    }
    if (bits == 8) {
        ReadVectorOperands(inst); pxor(xmm5, xmm5);
        movdqa(xmm3, xmm0); movdqa(xmm4, xmm1);
        punpcklbw(xmm0, xmm5); punpcklbw(xmm1, xmm5);
        punpckhbw(xmm3, xmm5); punpckhbw(xmm4, xmm5);
        pmullw(xmm0, xmm1); pmullw(xmm3, xmm4);
        // Clear high product bytes before unsigned packing so PACK
        // cannot saturate products whose low byte should wrap.
        pcmpeqd(xmm5, xmm5); psrlw(xmm5, 8);
        pand(xmm0, xmm5); pand(xmm3, xmm5); packuswb(xmm0, xmm3);
        WriteVector(inst, xmm0); return;
    }
    for (u32 byte = 0; byte < 16; byte += bits / 8) {
        LoadLane(rax, Home(Source(inst, 0).Def()) + byte, bits);
        LoadLane(rdx, Home(Source(inst, 1).Def()) + byte, bits);
        imul(rax, rdx);
        StoreLane(rax, Home(inst) + byte, bits);
    }
}

void Translator::EmitVec4Mul(ir::Inst* inst) { EmitVectorMultiply(inst, 32); }
void Translator::EmitVecMul(ir::Inst* inst) { EmitVectorMultiply(inst, VectorImmediate(inst, 2)); }

void Translator::EmitVectorMinMax(ir::Inst* inst, bool minimum) {
    const auto bits = VectorImmediate(inst, 2);
    ValidateLaneWidth(bits);
    const bool sign = VectorImmediate(inst, 3);
    for (u32 byte = 0; byte < 16; byte += bits / 8) {
        LoadLane(rax, Home(Source(inst, 0).Def()) + byte, bits);
        LoadLane(rdx, Home(Source(inst, 1).Def()) + byte, bits);
        if (sign) { SignExtend(rax, bits); SignExtend(rdx, bits); }
        cmp(rax, rdx);
        if (sign) {
            if (minimum) cmovg(rax, rdx); else cmovl(rax, rdx);
        } else {
            if (minimum) cmova(rax, rdx); else cmovb(rax, rdx);
        }
        StoreLane(rax, Home(inst) + byte, bits);
    }
}

void Translator::EmitVecMin(ir::Inst* inst) { EmitVectorMinMax(inst, true); }
void Translator::EmitVecMax(ir::Inst* inst) { EmitVectorMinMax(inst, false); }

void Translator::EmitVecMulHigh16(ir::Inst* inst) {
    ReadVectorOperands(inst); if (VectorImmediate(inst, 2)) pmulhw(xmm0, xmm1); else pmulhuw(xmm0, xmm1); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecMulWiden(ir::Inst* inst) {
    const auto bits = VectorImmediate(inst, 2); const bool sign = VectorImmediate(inst, 3);
    if (bits != 8 && bits != 16 && bits != 32) Unsupported(inst);
    if (bits == 32 && !sign) { ReadVectorOperands(inst); pmuludq(xmm0, xmm1); WriteVector(inst, xmm0); return; }
    for (u32 byte = 0; byte < 16; byte += bits / 4) {
        LoadLane(rax, Home(Source(inst, 0).Def()) + byte, bits);
        LoadLane(rdx, Home(Source(inst, 1).Def()) + byte, bits);
        if (sign) { SignExtend(rax, bits); SignExtend(rdx, bits); }
        imul(rax, rdx); StoreLane(rax, Home(inst) + byte, bits * 2);
    }
    return;
}

void Translator::EmitVecAvg(ir::Inst* inst) {
    ReadVectorOperands(inst); if (VectorImmediate(inst, 2) == 8) pavgb(xmm0, xmm1); else if (VectorImmediate(inst, 2) == 16) pavgw(xmm0, xmm1); else Unsupported(inst); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecAbsDiffSum8(ir::Inst* inst) {
    ReadVectorOperands(inst); psadbw(xmm0, xmm1); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecMadd16(ir::Inst* inst) {
    ReadVectorOperands(inst); pmaddwd(xmm0, xmm1); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecPack(ir::Inst* inst) {
    ReadVectorOperands(inst);
    if (VectorImmediate(inst, 2) == 16) { if (VectorImmediate(inst, 3)) packuswb(xmm0, xmm1); else packsswb(xmm0, xmm1); }
    else if (VectorImmediate(inst, 2) == 32) {
        if (!VectorImmediate(inst, 3)) packssdw(xmm0, xmm1);
        else {
            // Clamp negatives, bias into the signed-16 range, then
            // undo that bias after PACKSSDW's upper saturation.
            pxor(xmm4, xmm4);
            movdqa(xmm3, xmm0); pcmpgtd(xmm3, xmm4); pand(xmm0, xmm3);
            movdqa(xmm3, xmm1); pcmpgtd(xmm3, xmm4); pand(xmm1, xmm3);
            mov(eax, 0x8000); movd(xmm4, eax); pshufd(xmm4, xmm4, 0);
            psubd(xmm0, xmm4); psubd(xmm1, xmm4); packssdw(xmm0, xmm1);
            pcmpeqd(xmm4, xmm4); psllw(xmm4, 15); pxor(xmm0, xmm4);
        }
    }
    else Unsupported(inst);
    WriteVector(inst, xmm0); return;
}

void Translator::EmitVectorShift(ir::Inst* inst, ir::OpCode op) {
    const auto bits = VectorImmediate(inst, 2); ValidateLaneWidth(bits);
    const bool immediate = op == O::VecShiftLeftImm || op == O::VecShiftRightImm || op == O::VecShiftRightArithmeticImm;
    const bool arithmetic = op == O::VecShiftRightArithmetic || op == O::VecShiftRightArithmeticImm;
    const bool left = op == O::VecShiftLeft || op == O::VecShiftLeftImm;
    const u64 immediate_count = immediate ? inst->GetArg<ir::Imm>(1).Get() : 0;
    if (bits != 8 && (!arithmetic || bits != 64)) {
        ReadVector(xmm0, Source(inst, 0));
        const auto shift = [&](const auto& count) {
            if (left) {
                if (bits == 16) psllw(xmm0, count); else if (bits == 32) pslld(xmm0, count); else psllq(xmm0, count);
            } else if (arithmetic) {
                if (bits == 16) psraw(xmm0, count); else psrad(xmm0, count);
            } else {
                if (bits == 16) psrlw(xmm0, count); else if (bits == 32) psrld(xmm0, count); else psrlq(xmm0, count);
            }
        };
        if (immediate) shift(static_cast<u32>(std::min<u64>(immediate_count, bits)));
        else { Read(rcx, Source(inst, 1)); movq(xmm1, rcx); shift(xmm1); }
        WriteVector(inst, xmm0); return;
    }
    for (u32 byte = 0; byte < 16; byte += bits / 8) {
        LoadLane(rax, Home(Source(inst, 0).Def()) + byte, bits);
        if (immediate) mov(rcx, immediate_count); else Read(rcx, Source(inst, 1));
        if (arithmetic) {
            SignExtend(rax, bits); mov(edx, bits - 1); cmp(rcx, rdx); cmova(rcx, rdx); sar(rax, cl);
        } else {
            Label zero, done; cmp(rcx, bits); jae(zero);
            if (left) shl(rax, cl); else shr(rax, cl);
            jmp(done); L(zero); xor_(eax, eax); L(done);
        }
        StoreLane(rax, Home(inst) + byte, bits);
    }
    return;
}

void Translator::EmitVecShiftLeft(ir::Inst* inst) { EmitVectorShift(inst, O::VecShiftLeft); }

void Translator::EmitVecShiftRight(ir::Inst* inst) { EmitVectorShift(inst, O::VecShiftRight); }

void Translator::EmitVecShiftRightArithmetic(ir::Inst* inst) { EmitVectorShift(inst, O::VecShiftRightArithmetic); }

void Translator::EmitVecShiftLeftImm(ir::Inst* inst) { EmitVectorShift(inst, O::VecShiftLeftImm); }

void Translator::EmitVecShiftRightImm(ir::Inst* inst) { EmitVectorShift(inst, O::VecShiftRightImm); }

void Translator::EmitVecShiftRightArithmeticImm(ir::Inst* inst) { EmitVectorShift(inst, O::VecShiftRightArithmeticImm); }

}  // namespace swift::runtime::backend::x64
