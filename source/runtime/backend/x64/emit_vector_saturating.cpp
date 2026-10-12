#include "translator.h"
#include "runtime/common/saturating_multiply.h"
#include "runtime/common/saturating_shift.h"
#include "runtime/common/saturating_add_sub.h"

namespace swift::runtime::backend::x64 {
using O = ir::OpCode;
using namespace Xbyak;

void Translator::EmitVecSatShiftQC(ir::Inst* inst) {
    ValidateSaturatingShiftQC(inst); return;
}

void Translator::EmitVecSatShiftImm(ir::Inst* inst) {
    const auto shape = ValidateSaturatingShift(inst);
    const auto splat = [&](const Xmm& reg, u64 value, u32 width) {
        u64 pattern{};
        for (u32 bit = 0; bit < 64; bit += width) pattern |= value << bit;
        mov(rax, pattern); movq(reg, rax); punpcklqdq(reg, reg);
    };
    const auto sign64 = [&](const Xmm& output, const Xmm& input) {
        movdqa(output, input); psrad(output, 31); pshufd(output, output, 0xf5);
    };
    const auto right = [&](const Xmm& reg, u32 count, u32 width, bool arithmetic) {
        if (!count) return;
        if (width == 16) { if (arithmetic) psraw(reg, count); else psrlw(reg, count); }
        else if (width == 32) { if (arithmetic) psrad(reg, count); else psrld(reg, count); }
        else {
            if (arithmetic) {
                sign64(xmm6, reg); splat(xmm7, ~u64{0} << (64 - count), 64); pand(xmm6, xmm7);
            }
            psrlq(reg, count);
            if (arithmetic) por(reg, xmm6);
        }
    };
    const auto equal = [&](const Xmm& target, const Xmm& other, u32 width) {
        if (width == 16) pcmpeqw(target, other);
        else {
            pcmpeqd(target, other);
            if (width == 64) { pshufd(xmm7, target, 0xb1); pand(target, xmm7); }
        }
    };
    const auto blend = [&] {
        movdqa(xmm4, xmm3); pand(xmm0, xmm3); pandn(xmm4, xmm5); por(xmm0, xmm4);
    };
    const auto reduce = [&](u32 active_bytes) {
        const u32 active_mask = (u32{1} << active_bytes) - 1;
        pmovmskb(eax, xmm3); and_(eax, active_mask); cmp(eax, active_mask); setne(al); movzx(eax, al);
    };
    ReadVector(xmm0, Source(inst, 0));
    if (shape.Left() && shape.bits == 8) {
        // Widen first: every exact 8-bit left shift fits signed16,
        // including unsigned255 <<7. Packing provides the clamp.
        movdqa(xmm1, xmm0); pxor(xmm5, xmm5);
        if (shape.SignedSource()) pcmpgtb(xmm5, xmm0);
        punpcklbw(xmm0, xmm5); punpckhbw(xmm1, xmm5);
        psllw(xmm0, shape.shift); psllw(xmm1, shape.shift);
        movdqa(xmm3, xmm0); movdqa(xmm4, xmm1);
        if (shape.SignedResult()) packsswb(xmm0, xmm1); else packuswb(xmm0, xmm1);
        pxor(xmm5, xmm5); if (shape.SignedResult()) pcmpgtb(xmm5, xmm0);
        movdqa(xmm1, xmm0); movdqa(xmm6, xmm0);
        punpcklbw(xmm1, xmm5); punpckhbw(xmm6, xmm5);
        pcmpeqw(xmm3, xmm1); pcmpeqw(xmm4, xmm6);
        pmovmskb(eax, xmm3); pmovmskb(edx, xmm4); shl(edx, 16); or_(eax, edx);
        const u32 active_mask = shape.lanes == 16 ? UINT32_MAX : 0xffff;
        and_(eax, active_mask); cmp(eax, active_mask); setne(al); movzx(eax, al);
    } else if (shape.Left()) {
        movdqa(xmm1, xmm0);
        if (shape.bits == 16) psllw(xmm0, shape.shift);
        else if (shape.bits == 32) pslld(xmm0, shape.shift);
        else psllq(xmm0, shape.shift);
        movdqa(xmm3, xmm0); right(xmm3, shape.shift, shape.bits, shape.SignedResult());
        equal(xmm3, xmm1, shape.bits);
        if (shape.SignedSource()) {
            if (shape.bits == 16) { movdqa(xmm4, xmm1); psraw(xmm4, 15); }
            else if (shape.bits == 32) { movdqa(xmm4, xmm1); psrad(xmm4, 31); }
            else sign64(xmm4, xmm1);
        }
        const u64 maximum = shape.SignedResult() ? (u64{1} << (shape.bits - 1)) - 1 :
                            shape.bits == 64 ? ~u64{0} : (u64{1} << shape.bits) - 1;
        splat(xmm5, maximum, shape.bits);
        if (shape.SignedSource()) pxor(xmm5, xmm4);
        if (shape.mode == SaturatingShiftMode::SQSHLU) { pandn(xmm4, xmm3); movdqa(xmm3, xmm4); }
        blend(); reduce(shape.lanes * shape.bits / 8);
    } else {
        const u32 width = shape.SourceBits();
        movdqa(xmm1, xmm0); right(xmm0, shape.shift, width, shape.SignedSource());
        if (shape.Rounded()) {
            movdqa(xmm3, xmm1); right(xmm3, shape.shift - 1, width, false);
            splat(xmm4, 1, width); pand(xmm3, xmm4);
            if (width == 16) paddw(xmm0, xmm3);
            else if (width == 32) paddd(xmm0, xmm3);
            else paddq(xmm0, xmm3);
        }
        movdqa(xmm1, xmm0); // exact shifted-and-rounded lanes
        if (width == 16) {
            if (!shape.SignedSource()) {
                // Rounded U16 >>1 can be0x8000, which PACKUSWB would
                // interpret as negative. Clamp as unsigned first.
                splat(xmm3, 255, 16); movdqa(xmm4, xmm0); psubusw(xmm4, xmm3);
                pxor(xmm5, xmm5); pcmpeqw(xmm4, xmm5);
                pand(xmm0, xmm4); pandn(xmm4, xmm3); por(xmm0, xmm4);
            }
            if (shape.SignedResult()) packsswb(xmm0, xmm0); else packuswb(xmm0, xmm0);
            pxor(xmm4, xmm4); if (shape.SignedResult()) pcmpgtb(xmm4, xmm0);
            movdqa(xmm3, xmm0); punpcklbw(xmm3, xmm4); pcmpeqw(xmm3, xmm1);
        } else if (width == 32) {
            if (!shape.SignedResult()) { splat(xmm4, 0x8000, 32); psubd(xmm0, xmm4); }
            packssdw(xmm0, xmm0);
            if (!shape.SignedResult()) { splat(xmm4, 0x8000, 16); pxor(xmm0, xmm4); }
            movdqa(xmm3, xmm0);
            if (shape.SignedResult()) { punpcklwd(xmm3, xmm3); psrad(xmm3, 16); }
            else { pxor(xmm4, xmm4); punpcklwd(xmm3, xmm4); }
            pcmpeqd(xmm3, xmm1);
        } else {
            if (shape.SignedResult()) { movdqa(xmm3, xmm0); psrad(xmm3, 31); pshufd(xmm3, xmm3, 0xa0); }
            else pxor(xmm3, xmm3);
            pcmpeqd(xmm3, xmm0); pshufd(xmm3, xmm3, 0xf5);
            splat(xmm5, shape.SignedResult() ? 0x7fffffff : 0xffffffff, 64);
            if (shape.SignedSource()) {
                sign64(xmm4, xmm0);
                if (shape.SignedResult()) pxor(xmm5, xmm4);
                else { pandn(xmm4, xmm5); movdqa(xmm5, xmm4); }
            }
            blend(); pshufd(xmm0, xmm0, 0x88);
        }
        reduce(shape.lanes * width / 8);
    }
    // Left D forms and all narrowing operations zero inactive bytes.
    const u32 output_bytes = shape.lanes * shape.bits / 8;
    if (output_bytes == 8) { pslldq(xmm0, 8); psrldq(xmm0, 8); }
    mov(qword[r13 + Home(inst) + 24], rax); WriteVector(inst, xmm0);
    PublishPseudoResults(inst, O::VecSatShiftQC, 24); return;
}

void Translator::EmitVecSatMulQC(ir::Inst* inst) {
    ValidateSaturatingMultiplyQC(inst);
    return;
}

void Translator::EmitVecSatMulHigh(ir::Inst* inst) {
    const auto shape = ValidateSaturatingMultiply(inst);
    const auto width = shape.bits, active = shape.lanes, mode = shape.mode;
    if (width == 32) {
        // Exact 32x32 products fit signed64. Extract before adding
        // the accumulator so neither doubled products nor shifted
        // accumulators can overflow; only the final value saturates.
        mov(qword[r13 + Home(inst)], 0); mov(qword[r13 + Home(inst) + 8], 0); xor_(r8d, r8d);
        for (u32 lane = 0; lane < active; ++lane) {
            movsxd(rax, dword[r13 + Home(Source(inst, 0).Def()) + lane * 4]);
            movsxd(rdx, dword[r13 + Home(Source(inst, 1).Def()) + lane * 4]); imul(rax, rdx);
            if (mode == 3) neg(rax);
            if (mode) add(rax, 0x40000000);
            sar(rax, 31);
            if (mode >= 2) { movsxd(rdx, dword[r13 + Home(Source(inst, 2).Def()) + lane * 4]); add(rax, rdx); }
            mov(r10, static_cast<u64>(s64{INT32_MIN})); cmp(rax, r10); setl(dl); cmovl(rax, r10); movzx(edx, dl); or_(r8d, edx);
            mov(r10, u64{INT32_MAX}); cmp(rax, r10); setg(dl); cmovg(rax, r10); movzx(edx, dl); or_(r8d, edx);
            mov(dword[r13 + Home(inst) + lane * 4], eax);
        }
        mov(qword[r13 + Home(inst) + 24], r8);
        PublishPseudoResults(inst, O::VecSatMulQC, 24); return;
    }
    // SSE2 reconstructs exact signed32 products from their low/high
    // halves. Preserve XMM2, which holds unrelated forwarded SSA.
    ReadVector(xmm0, Source(inst, 0)); ReadVector(xmm1, Source(inst, 1)); ReadVector(xmm3, Source(inst, 2));
    movdqa(xmm4, xmm0); pmullw(xmm0, xmm1); pmulhw(xmm4, xmm1);
    movdqa(xmm5, xmm0); punpcklwd(xmm0, xmm4); punpckhwd(xmm5, xmm4); movdqa(xmm4, xmm5);
    if (mode == 3) {
        pxor(xmm5, xmm5); psubd(xmm5, xmm0); movdqa(xmm0, xmm5);
        pxor(xmm5, xmm5); psubd(xmm5, xmm4); movdqa(xmm4, xmm5);
    }
    if (mode) {
        mov(eax, 0x4000); movd(xmm5, eax); pshufd(xmm5, xmm5, 0);
        paddd(xmm0, xmm5); paddd(xmm4, xmm5);
    }
    psrad(xmm0, 15); psrad(xmm4, 15);
    if (mode >= 2) {
        movdqa(xmm5, xmm3); punpcklwd(xmm5, xmm5); psrad(xmm5, 16); paddd(xmm0, xmm5);
        punpckhwd(xmm3, xmm3); psrad(xmm3, 16); paddd(xmm4, xmm3);
    }
    // Compare unsaturated values with the sign-extended packed
    // results; reduce only active lane comparisons into the QC bit.
    movdqa(xmm6, xmm0); movdqa(xmm7, xmm4); packssdw(xmm0, xmm4);
    movdqa(xmm4, xmm0); movdqa(xmm5, xmm0);
    punpcklwd(xmm4, xmm4); psrad(xmm4, 16); punpckhwd(xmm5, xmm5); psrad(xmm5, 16);
    pcmpeqd(xmm6, xmm4); pcmpeqd(xmm7, xmm5);
    pmovmskb(eax, xmm6); pmovmskb(edx, xmm7); shl(edx, 16); or_(eax, edx);
    const u32 active_mask = active == 8 ? UINT32_MAX : (u32{1} << (active * 4)) - 1;
    and_(eax, active_mask); cmp(eax, active_mask); setne(al); movzx(eax, al);
    mov(qword[r13 + Home(inst) + 24], rax);
    if (active != 8) { pslldq(xmm0, 16 - active * 2); psrldq(xmm0, 16 - active * 2); }
    WriteVector(inst, xmm0);
    PublishPseudoResults(inst, O::VecSatMulQC, 24); return;
}

void Translator::EmitVectorSaturatingAddSub(ir::Inst* inst, ir::OpCode op) {
    const auto shape = ValidateSaturatingAddSub(inst);
    const auto bits = shape.bits;
    const bool sign = shape.sign;
    ReadVectorOperands(inst);
    if (bits <= 16) {
        if (op == O::VecSatAdd) {
            if (bits == 8 && sign) paddsb(xmm0, xmm1); else if (bits == 8) paddusb(xmm0, xmm1);
            else if (sign) paddsw(xmm0, xmm1); else paddusw(xmm0, xmm1);
        } else {
            if (bits == 8 && sign) psubsb(xmm0, xmm1); else if (bits == 8) psubusb(xmm0, xmm1);
            else if (sign) psubsw(xmm0, xmm1); else psubusw(xmm0, xmm1);
        }
    } else {
        const bool add = op == O::VecSatAdd;
        const auto sign_mask = [&](const Xmm& reg) {
            psrad(reg, 31);
            if (bits == 64) pshufd(reg, reg, 0xf5);
        };
        movdqa(xmm3, xmm0);
        if (add) { if (bits == 32) paddd(xmm3, xmm1); else paddq(xmm3, xmm1); }
        else { if (bits == 32) psubd(xmm3, xmm1); else psubq(xmm3, xmm1); }
        if (sign) {
            // Overflow = sign((a ^ wrapped) & (~(a ^ b) for add, a ^ b for sub)).
            movdqa(xmm4, xmm0); pxor(xmm4, xmm3);
            movdqa(xmm5, xmm0); pxor(xmm5, xmm1);
            if (add) { pcmpeqd(xmm6, xmm6); pxor(xmm5, xmm6); }
            pand(xmm4, xmm5); sign_mask(xmm4);
            movdqa(xmm5, xmm0); sign_mask(xmm5);
            pcmpeqd(xmm6, xmm6);
            if (bits == 32) psrld(xmm6, 1); else psrlq(xmm6, 1);
            pxor(xmm5, xmm6); // MAX xor sign(a) gives MAX or MIN.
            pand(xmm5, xmm4); pandn(xmm4, xmm3); por(xmm4, xmm5); movdqa(xmm0, xmm4);
        } else {
            // Carry/borrow MSB identities avoid qword comparison entirely.
            // add: (a & b) | ((a | b) & ~wrapped)
            // sub: (~a & b) | (~(a ^ b) & wrapped)
            movdqa(xmm4, xmm0); movdqa(xmm5, xmm0);
            if (add) {
                pand(xmm4, xmm1); por(xmm5, xmm1);
                movdqa(xmm6, xmm3); pandn(xmm6, xmm5); por(xmm4, xmm6);
            } else {
                pandn(xmm4, xmm1); pxor(xmm5, xmm1); pandn(xmm5, xmm3); por(xmm4, xmm5);
            }
            sign_mask(xmm4);
            if (add) { por(xmm3, xmm4); movdqa(xmm0, xmm3); }
            else { pandn(xmm4, xmm3); movdqa(xmm0, xmm4); }
        }
    }
    WriteVector(inst, xmm0); return;
}

void Translator::EmitVecSatAdd(ir::Inst* inst) { EmitVectorSaturatingAddSub(inst, O::VecSatAdd); }

void Translator::EmitVecSatSub(ir::Inst* inst) { EmitVectorSaturatingAddSub(inst, O::VecSatSub); }

}  // namespace swift::runtime::backend::x64
