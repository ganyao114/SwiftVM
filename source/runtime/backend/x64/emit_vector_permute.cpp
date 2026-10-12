#include "translator.h"

namespace swift::runtime::backend::x64 {
using O = ir::OpCode;
using namespace Xbyak;

void Translator::EmitVecLoadConst(ir::Inst* inst) {
    mov(rax, inst->GetArg<ir::Imm>(0).Get()); mov(qword[r13 + Home(inst)], rax);
    mov(rax, inst->GetArg<ir::Imm>(1).Get()); mov(qword[r13 + Home(inst) + 8], rax); return;
}

void Translator::EmitVecByteShift(ir::Inst* inst) {
    ReadVector(xmm0, Source(inst, 0)); if (VectorImmediate(inst, 3)) pslldq(xmm0, VectorImmediate(inst, 2)); else psrldq(xmm0, VectorImmediate(inst, 2)); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecShuffle32(ir::Inst* inst) {
    ReadVector(xmm0, Source(inst, 0)); pshufd(xmm0, xmm0, VectorImmediate(inst, 1)); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecShuffle32TwoSrc(ir::Inst* inst) {
    ReadVectorOperands(inst); shufps(xmm0, xmm1, VectorImmediate(inst, 2)); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecShuffle16(ir::Inst* inst) {
    ReadVector(xmm0, Source(inst, 0)); if (VectorImmediate(inst, 2)) pshufhw(xmm0, xmm0, VectorImmediate(inst, 1)); else pshuflw(xmm0, xmm0, VectorImmediate(inst, 1)); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecDupPairs32(ir::Inst* inst) {
    ReadVector(xmm0, Source(inst, 0)); pshufd(xmm0, xmm0, VectorImmediate(inst, 1) ? 0xf5 : 0xa0); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecDup64(ir::Inst* inst) {
    Read(rax, Source(inst, 0)); movq(xmm0, rax); punpcklqdq(xmm0, xmm0); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecExtract64(ir::Inst* inst) {
    mov(rax, qword[r13 + Home(Source(inst, 0).Def()) + (VectorImmediate(inst, 1) ? 8 : 0)]); Write(inst, rax); return;
}

void Translator::EmitVecExtract16(ir::Inst* inst) {
    movzx(eax, word[r13 + Home(Source(inst, 0).Def()) + (VectorImmediate(inst, 1) & 7) * 2]); Write(inst, rax); return;
}

void Translator::EmitVecInsert16(ir::Inst* inst) {
    ReadVector(xmm0, Source(inst, 0)); Read(rax, Source(inst, 1)); pinsrw(xmm0, eax, VectorImmediate(inst, 2) & 7); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecExtractBytes(ir::Inst* inst) {
    ReadVectorOperands(inst); psrldq(xmm0, VectorImmediate(inst, 2)); pslldq(xmm1, 16 - VectorImmediate(inst, 2)); por(xmm0, xmm1); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecZip(ir::Inst* inst) {
    ReadVectorOperands(inst); const auto bits = VectorImmediate(inst, 2); ValidateLaneWidth(bits);
    if (VectorImmediate(inst, 3)) {
        if (bits == 8) punpckhbw(xmm0, xmm1); else if (bits == 16) punpckhwd(xmm0, xmm1);
        else if (bits == 32) punpckhdq(xmm0, xmm1); else punpckhqdq(xmm0, xmm1);
    } else {
        if (bits == 8) punpcklbw(xmm0, xmm1); else if (bits == 16) punpcklwd(xmm0, xmm1);
        else if (bits == 32) punpckldq(xmm0, xmm1); else punpcklqdq(xmm0, xmm1);
    }
    WriteVector(inst, xmm0); return;
}

void Translator::EmitVecUnzip(ir::Inst* inst) {
    const auto bits = VectorImmediate(inst, 2), bytes = bits / 8; ValidateLaneWidth(bits);
    for (u32 index = 0; index < 16 / bytes; ++index) {
        const auto input = index < 8 / bytes ? Source(inst, 0) : Source(inst, 1);
        const auto lane = (index % (8 / bytes)) * 2 + !!VectorImmediate(inst, 3);
        LoadLane(rax, Home(input.Def()) + lane * bytes, bits);
        StoreLane(rax, Home(inst) + index * bytes, bits);
    }
    return;
}

void Translator::EmitVectorTableLookup(ir::Inst* inst, ir::OpCode op) {
    for (u32 index = 0; index < 16; ++index) {
        movzx(eax, byte[r13 + Home(Source(inst, 1).Def()) + index]);
        if (op == O::VecTableLookup8) and_(eax, 0x8f);
        Label zero, done; cmp(eax, 16); jae(zero);
        movzx(eax, byte[r13 + rax + Home(Source(inst, 0).Def())]); jmp(done);
        L(zero); xor_(eax, eax); L(done); mov(byte[r13 + Home(inst) + index], al);
    }
    return;
}

void Translator::EmitVecShuffle32Indexed(ir::Inst* inst) { EmitVectorTableLookup(inst, O::VecShuffle32Indexed); }

void Translator::EmitVecTableLookup8(ir::Inst* inst) { EmitVectorTableLookup(inst, O::VecTableLookup8); }

void Translator::EmitVecMovMask(ir::Inst* inst) {
    ReadVector(xmm0, Source(inst, 0));
    if (VectorImmediate(inst, 1) == 8) pmovmskb(eax, xmm0); else if (VectorImmediate(inst, 1) == 32) movmskps(eax, xmm0);
    else if (VectorImmediate(inst, 1) == 64) movmskpd(eax, xmm0); else Unsupported(inst);
    Write(inst, rax); return;
}

}  // namespace swift::runtime::backend::x64
