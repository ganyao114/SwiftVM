#include "translator.h"
#include "helpers.h"

namespace swift::runtime::backend::x64 {
using O = ir::OpCode;
using namespace Xbyak;

void Translator::EmitVecFAddScalar32(ir::Inst* inst) {
    ReadVectorOperands(inst); addss(xmm0, xmm1); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecFSubScalar32(ir::Inst* inst) {
    ReadVectorOperands(inst); subss(xmm0, xmm1); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecFMulScalar32(ir::Inst* inst) {
    ReadVectorOperands(inst); mulss(xmm0, xmm1); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecFDivScalar32(ir::Inst* inst) {
    ReadVectorOperands(inst); divss(xmm0, xmm1); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecFAddScalar64(ir::Inst* inst) {
    ReadVectorOperands(inst); addsd(xmm0, xmm1); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecFSubScalar64(ir::Inst* inst) {
    ReadVectorOperands(inst); subsd(xmm0, xmm1); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecFMulScalar64(ir::Inst* inst) {
    ReadVectorOperands(inst); mulsd(xmm0, xmm1); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecFDivScalar64(ir::Inst* inst) {
    ReadVectorOperands(inst); divsd(xmm0, xmm1); WriteVector(inst, xmm0); return;
}

void Translator::EmitVectorFloatBinary(ir::Inst* inst, ir::OpCode op) {
    ReadVectorOperands(inst); if (VectorImmediate(inst, 2) != 32 && VectorImmediate(inst, 2) != 64) Unsupported(inst);
    if (op == O::VecFAdd) { if (VectorImmediate(inst, 2) == 32) addps(xmm0, xmm1); else addpd(xmm0, xmm1); }
    else if (op == O::VecFSub) { if (VectorImmediate(inst, 2) == 32) subps(xmm0, xmm1); else subpd(xmm0, xmm1); }
    else if (op == O::VecFMul) { if (VectorImmediate(inst, 2) == 32) mulps(xmm0, xmm1); else mulpd(xmm0, xmm1); }
    else { if (VectorImmediate(inst, 2) == 32) divps(xmm0, xmm1); else divpd(xmm0, xmm1); }
    WriteVector(inst, xmm0); return;
}

void Translator::EmitVecFAdd(ir::Inst* inst) { EmitVectorFloatBinary(inst, O::VecFAdd); }

void Translator::EmitVecFSub(ir::Inst* inst) { EmitVectorFloatBinary(inst, O::VecFSub); }

void Translator::EmitVecFMul(ir::Inst* inst) { EmitVectorFloatBinary(inst, O::VecFMul); }

void Translator::EmitVecFDiv(ir::Inst* inst) { EmitVectorFloatBinary(inst, O::VecFDiv); }

void Translator::EmitVecFRoundInt(ir::Inst* inst) {
    const auto bits = VectorImmediate(inst, 2), mode = VectorImmediate(inst, 3); const bool scalar = VectorImmediate(inst, 4);
    if ((bits != 32 && bits != 64) || mode > 3) Unsupported(inst);
    ReadVector(xmm0, Source(inst, 0));
    if (features.sse41) {
        if (scalar) {
            ReadVector(xmm1, Source(inst, 1));
            if (bits == 32) roundss(xmm1, xmm0, mode | 8); else roundsd(xmm1, xmm0, mode | 8);
            movdqa(xmm0, xmm1);
        } else if (bits == 32) roundps(xmm0, xmm0, mode | 8); else roundpd(xmm0, xmm0, mode | 8);
        WriteVector(inst, xmm0); return;
    }
    if (scalar) ReadVector(xmm0, Source(inst, 1)); else pxor(xmm0, xmm0);
    WriteVector(inst, xmm0);
    for (u32 lane = 0; lane < (scalar ? 1u : 128 / bits); ++lane) {
        LoadLane(rdi, Home(Source(inst, 0).Def()) + lane * bits / 8, bits);
        mov(esi, bits); mov(edx, mode); Call(reinterpret_cast<u64>(&RoundIntegral));
        StoreLane(rax, Home(inst) + lane * bits / 8, bits);
    }
    return;
}

void Translator::EmitVecFMulAdd(ir::Inst* inst) {
    const auto bits = VectorImmediate(inst, 3), flags = VectorImmediate(inst, 4) & 3;
    if (bits != 32 && bits != 64) Unsupported(inst);
    if (!features.fma) {
        for (u32 lane = 0; lane < 128 / bits; ++lane) {
            LoadLane(rdi, Home(Source(inst, 0).Def()) + lane * bits / 8, bits);
            LoadLane(rsi, Home(Source(inst, 1).Def()) + lane * bits / 8, bits);
            LoadLane(rdx, Home(Source(inst, 2).Def()) + lane * bits / 8, bits);
            mov(ecx, bits); mov(r8d, flags); Call(reinterpret_cast<u64>(&FusedMultiplyAdd));
            StoreLane(rax, Home(inst) + lane * bits / 8, bits);
        }
        return;
    }
    ReadVectorOperands(inst); ReadVector(xmm3, Source(inst, 2));
    if (bits == 32) {
        if (flags == 0) vfmadd213ps(xmm0, xmm1, xmm3);
        else if (flags == 1) vfnmadd213ps(xmm0, xmm1, xmm3);
        else if (flags == 2) vfmsub213ps(xmm0, xmm1, xmm3);
        else vfnmsub213ps(xmm0, xmm1, xmm3);
    } else {
        if (flags == 0) vfmadd213pd(xmm0, xmm1, xmm3);
        else if (flags == 1) vfnmadd213pd(xmm0, xmm1, xmm3);
        else if (flags == 2) vfmsub213pd(xmm0, xmm1, xmm3);
        else vfnmsub213pd(xmm0, xmm1, xmm3);
    }
    WriteVector(inst, xmm0);
    // The IR has a fixed NaN priority a,b,c, preserving original signs
    // despite sign-mode negation. FMA3's physical-operand preference
    // cannot substitute for that policy.
    const u64 quiet = bits == 32 ? 0x00400000 : 0x0008000000000000ULL;
    const u64 indefinite = bits == 32 ? 0xffc00000 : 0xfff8000000000000ULL;
    for (u32 lane = 0; lane < 128 / bits; ++lane) {
        Label done;
        const auto compare_self = [&](u32 offset) {
            if (bits == 32) { movss(xmm4, ptr[r13 + offset]); ucomiss(xmm4, xmm4); }
            else { movsd(xmm4, ptr[r13 + offset]); ucomisd(xmm4, xmm4); }
        };
        const u32 output = Home(inst) + lane * bits / 8;
        for (u32 index = 0; index < 3; ++index) {
            Label next; const auto input = Home(Source(inst, index).Def()) + lane * bits / 8;
            compare_self(input); jnp(next);
            LoadLane(rax, input, bits); mov(rdx, quiet); or_(rax, rdx);
            StoreLane(rax, output, bits); jmp(done); L(next);
        }
        compare_self(output); jnp(done);
        mov(rax, indefinite); StoreLane(rax, output, bits); L(done);
    }
    vzeroupper(); return;
}

void Translator::EmitVecFMinMax(ir::Inst* inst) {
    ReadVectorOperands(inst);
    if (VectorImmediate(inst, 2) == 32 && VectorImmediate(inst, 4)) { if (VectorImmediate(inst, 3)) maxss(xmm0, xmm1); else minss(xmm0, xmm1); }
    else if (VectorImmediate(inst, 2) == 32) { if (VectorImmediate(inst, 3)) maxps(xmm0, xmm1); else minps(xmm0, xmm1); }
    else if (VectorImmediate(inst, 2) == 64 && VectorImmediate(inst, 4)) { if (VectorImmediate(inst, 3)) maxsd(xmm0, xmm1); else minsd(xmm0, xmm1); }
    else if (VectorImmediate(inst, 2) == 64) { if (VectorImmediate(inst, 3)) maxpd(xmm0, xmm1); else minpd(xmm0, xmm1); }
    else Unsupported(inst);
    WriteVector(inst, xmm0); return;
}

void Translator::EmitVecFUnary(ir::Inst* inst) {
    if (VectorImmediate(inst, 3) != 0) Unsupported(inst);
    ReadVector(xmm1, Source(inst, 0));
    if (VectorImmediate(inst, 4)) ReadVector(xmm0, Source(inst, 1)); else pxor(xmm0, xmm0);
    if (VectorImmediate(inst, 2) == 32 && VectorImmediate(inst, 4)) sqrtss(xmm0, xmm1);
    else if (VectorImmediate(inst, 2) == 32) sqrtps(xmm0, xmm1);
    else if (VectorImmediate(inst, 2) == 64 && VectorImmediate(inst, 4)) sqrtsd(xmm0, xmm1);
    else if (VectorImmediate(inst, 2) == 64) sqrtpd(xmm0, xmm1); else Unsupported(inst);
    WriteVector(inst, xmm0); return;
}

void Translator::EmitVecFCmp(ir::Inst* inst) {
    ReadVectorOperands(inst); if (VectorImmediate(inst, 2) == 32) ucomiss(xmm0, xmm1); else if (VectorImmediate(inst, 2) == 64) ucomisd(xmm0, xmm1); else Unsupported(inst);
    setb(al); setp(dl); sete(cl);
    movzx(eax, al); movzx(edx, dl); movzx(ecx, cl); shl(edx, 1); shl(ecx, 2); or_(eax, edx); or_(eax, ecx);
    Write(inst, rax); return;
}

void Translator::EmitVecFCmpMask(ir::Inst* inst) {
    const auto bits = VectorImmediate(inst, 2), relations = VectorImmediate(inst, 3) & 15;
    if (bits != 32 && bits != 64) Unsupported(inst);
    ReadVector(xmm0, Source(inst, 0)); WriteVector(inst, xmm0);
    const auto lanes = VectorImmediate(inst, 4) ? 1 : 128 / bits;
    for (u32 lane = 0; lane < lanes; ++lane) {
        if (bits == 32) { movss(xmm0, ptr[r13 + Home(Source(inst, 0).Def()) + lane * 4]); movss(xmm1, ptr[r13 + Home(Source(inst, 1).Def()) + lane * 4]); ucomiss(xmm0, xmm1); }
        else { movsd(xmm0, ptr[r13 + Home(Source(inst, 0).Def()) + lane * 8]); movsd(xmm1, ptr[r13 + Home(Source(inst, 1).Def()) + lane * 8]); ucomisd(xmm0, xmm1); }
        Label unordered, less, equal, done;
        jp(unordered); jb(less); je(equal); mov(eax, (relations >> 2) & 1); jmp(done);
        L(unordered); mov(eax, (relations >> 3) & 1); jmp(done);
        L(less); mov(eax, relations & 1); jmp(done);
        L(equal); mov(eax, (relations >> 1) & 1); L(done); neg(rax);
        StoreLane(rax, Home(inst) + lane * bits / 8, bits);
    }
    return;
}

void Translator::EmitVecFCvtPacked(ir::Inst* inst) {
    ReadVector(xmm0, Source(inst, 0));
    switch (VectorImmediate(inst, 1)) {
        case 0: cvtdq2ps(xmm0, xmm0); break; case 1: cvtdq2pd(xmm0, xmm0); break;
        case 2: cvtps2dq(xmm0, xmm0); break; case 3: cvttps2dq(xmm0, xmm0); break;
        case 4: cvtpd2dq(xmm0, xmm0); break; case 5: cvttpd2dq(xmm0, xmm0); break;
        case 6: cvtps2pd(xmm0, xmm0); break; case 7: cvtpd2ps(xmm0, xmm0); break;
        default: Unsupported(inst);
    } WriteVector(inst, xmm0); return;
}

void Translator::EmitVecFCvtScalar(ir::Inst* inst) {
    ReadVector(xmm0, Source(inst, 0)); if (VectorImmediate(inst, 1) == 32) cvtss2sd(xmm0, xmm0); else if (VectorImmediate(inst, 1) == 64) cvtsd2ss(xmm0, xmm0); else Unsupported(inst);
    movq(rax, xmm0); if (VectorImmediate(inst, 1) == 64) mov(eax, eax); Write(inst, rax); return;
}

void Translator::EmitVecFCvtIntToFloat(ir::Inst* inst) {
    Read(rax, Source(inst, 0)); SignExtend(rax, VectorImmediate(inst, 1)); pxor(xmm0, xmm0);
    if (VectorImmediate(inst, 2) == 32) cvtsi2ss(xmm0, rax); else if (VectorImmediate(inst, 2) == 64) cvtsi2sd(xmm0, rax); else Unsupported(inst);
    movq(rax, xmm0); Write(inst, rax); return;
}

void Translator::EmitVecFCvtFloatToInt(ir::Inst* inst) {
    ReadVector(xmm0, Source(inst, 0));
    if (VectorImmediate(inst, 1) == 32) { if (VectorImmediate(inst, 3)) cvtss2si(rax, xmm0); else cvttss2si(rax, xmm0); }
    else if (VectorImmediate(inst, 1) == 64) { if (VectorImmediate(inst, 3)) cvtsd2si(rax, xmm0); else cvttsd2si(rax, xmm0); }
    else Unsupported(inst);
    if (VectorImmediate(inst, 2) == 32) {
        // SSE's 64-bit conversion differs on finite values outside
        // the requested signed-32 range; choose the width at emission.
        if (VectorImmediate(inst, 1) == 32) { if (VectorImmediate(inst, 3)) cvtss2si(eax, xmm0); else cvttss2si(eax, xmm0); }
        else { if (VectorImmediate(inst, 3)) cvtsd2si(eax, xmm0); else cvttsd2si(eax, xmm0); }
    }
    Write(inst, rax); return;
}

}  // namespace swift::runtime::backend::x64
