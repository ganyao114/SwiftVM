#include "emit.h"
#include "helpers.h"
#include "runtime/arch/x86/floating.h"

namespace swift::runtime::backend::x64::detail {
using O = OpCode;

void Writer::AccumulateFloating(const Reg32& raised) {
    if (options.mxcsr_offset != UINT32_MAX) or_(dword[r12 + options.mxcsr_offset], raised);
    if (!options.native_fp_environment || options.mxcsr_offset == UINT32_MAX) return;
    if (Linked()) {
        or_(dword[r12 - 40], raised);
        ldmxcsr(dword[r12 - 40]);
    } else {
        or_(dword[rsp + mxcsr_slot + 4], raised);
        ldmxcsr(dword[rsp + mxcsr_slot + 4]);
    }
}

void Writer::Float(OpContext& c) {
    const auto op = c.operation.opcode;
    const bool s32 = op == O::VecFAddScalar32 || op == O::VecFSubScalar32 ||
                     op == O::VecFMulScalar32 || op == O::VecFDivScalar32;
    const bool s64 = op == O::VecFAddScalar64 || op == O::VecFSubScalar64 ||
                     op == O::VecFMulScalar64 || op == O::VecFDivScalar64;
    const bool fused = op == O::VecFMulAdd, round = op == O::VecFRoundInt,
               unary = op == O::VecFUnary, minmax = op == O::VecFMinMax,
               mask = op == O::VecFCmpMask;
    const unsigned bits = s32 ? 32 : s64 ? 64 : c.Arg(fused ? 3 : 2).Payload();
    if (bits != 32 && bits != 64) Reject(CompileError::UnsupportedOperand);
    const unsigned flags = fused ? c.Arg(4).Payload() : 0;
    const bool scalar = s32 || s64 || ((round || unary || minmax || mask) && c.Arg(4).Payload()) ||
                        (fused && (flags & 4));
    ReadVector(c.Arg(0), xmm0);
    ReadVector(c.Arg(1), xmm1);
    if (fused || (round && !options.sse41)) {
        const unsigned tmp = allocation.call_offset + 208;
        movdqu(ptr[rsp + tmp], xmm0);
        movdqu(ptr[rsp + tmp + 16], xmm1);
        if (fused) {
            ReadVector(c.Arg(2), xmm14);
            movdqu(ptr[rsp + tmp + 32], xmm14);
        }
        if (scalar)
            movdqu(ptr[rsp + tmp + 48], xmm0);
        else {
            pxor(xmm14, xmm14);
            movdqu(ptr[rsp + tmp + 48], xmm14);
        }
        for (unsigned i = 0; i < (scalar ? 1 : 128 / bits); ++i) {
            PrepareCall();
            if (fused) {
                Load(rdi, rsp, tmp + i * bits / 8, bits);
                Load(rsi, rsp, tmp + 16 + i * bits / 8, bits);
                Load(rdx, rsp, tmp + 32 + i * bits / 8, bits);
                mov(ecx, bits);
                unsigned f = flags & 3;
                if ((flags & 8) && ((i & 1) == ((flags >> 4) & 1))) f ^= 2;
                mov(r8d, f);
                if (options.mxcsr_offset != UINT32_MAX)
                    mov(r9d, dword[r12 + options.mxcsr_offset]);
                else
                    mov(r9d, 0x1f80);
                Call(reinterpret_cast<std::uintptr_t>(&FusedMultiplyAddStatus),
                     HostSymbol::X64FusedMultiplyAdd);
            } else {
                Load(rdi, rsp, tmp + 16 + i * bits / 8, bits);
                mov(esi, bits);
                mov(edx, c.Arg(3).Payload());
                if (options.mxcsr_offset != UINT32_MAX)
                    mov(ecx, dword[r12 + options.mxcsr_offset]);
                else
                    mov(ecx, 0x1f80);
                Call(reinterpret_cast<std::uintptr_t>(&RoundIntegralStatus),
                     HostSymbol::X64RoundIntegral);
            }
            AccumulateFloating(edx);
            Store(rax, rsp, tmp + 48 + i * bits / 8, bits);
        }
        movdqu(xmm0, ptr[rsp + tmp + 48]);
    } else if (round) {
        const unsigned mode = c.Arg(3).Payload();
        if (mode & ~11u) Reject(CompileError::UnsupportedOperand);
        if (bits == 32) {
            if (scalar)
                roundss(xmm0, xmm1, mode);
            else
                roundps(xmm0, xmm1, mode);
        } else {
            if (scalar)
                roundsd(xmm0, xmm1, mode);
            else
                roundpd(xmm0, xmm1, mode);
        }
    } else if (mask) {
        const unsigned relations = c.Arg(3).Payload() & 15;
        const bool signaling = c.Arg(3).Payload() & 16;
        const bool quiet_ordered = FloatingCompareNeedsCall(relations);
        if (FloatingCompareNeedsCall(c.Arg(3).Payload())) {
            // SSE2 has no quiet ordered LT/LE predicate. Use the shared pure
            // numerical leaf only for those shapes instead of introducing IE.
            const auto tmp = allocation.call_offset + 208;
            movdqu(ptr[rsp + tmp], xmm0);
            movdqu(ptr[rsp + tmp + 16], xmm1);
            mov(dword[rsp + tmp + 64],
                static_cast<unsigned>(runtime::x86::FloatingOperation::CompareMask));
            mov(dword[rsp + tmp + 68], bits);
            if (options.mxcsr_offset != UINT32_MAX)
                mov(eax, dword[r12 + options.mxcsr_offset]);
            else
                mov(eax, 0x1f80);
            mov(dword[rsp + tmp + 76], eax);
            mov(dword[rsp + tmp + 80],
                scalar ? static_cast<unsigned>(runtime::x86::FloatingFlag::Scalar) : 0);
            mov(dword[rsp + tmp + 84], relations);
            PrepareCall();
            lea(rdi, ptr[rsp + tmp]);
            Call(reinterpret_cast<std::uintptr_t>(&runtime::x86::ExecuteFloating),
                 HostSymbol::X64FloatingCompare);
            mov(edx, dword[rsp + tmp + 88]);
            AccumulateFloating(edx);
            movdqu(xmm0, ptr[rsp + tmp + 48]);
        } else {
            const auto compare = [&](const Xmm& dst, const Xmm& rhs, unsigned predicate) {
                if (bits == 32) {
                    if (scalar)
                        cmpss(dst, rhs, predicate);
                    else
                        cmpps(dst, rhs, predicate);
                } else {
                    if (scalar)
                        cmpsd(dst, rhs, predicate);
                    else
                        cmppd(dst, rhs, predicate);
                }
            };
            movdqa(xmm15, xmm0);
            // Quiet predicates still report SNaN; signaling predicates also
            // report QNaN. Do not infer this dimension from the result mask.
            if (signaling && !quiet_ordered) {
                movdqa(xmm14, xmm0);
                compare(xmm14, xmm1, 1);
            }
            unsigned predicate = 3;
            bool swap{};
            switch (relations) {
                case 1:
                    predicate = 1;
                    break;
                case 2:
                    predicate = 0;
                    break;
                case 3:
                    predicate = 2;
                    break;
                case 4:
                    predicate = 1;
                    swap = true;
                    break;
                case 5:
                    predicate = 4;
                    break;
                case 6:
                    predicate = 2;
                    swap = true;
                    break;
                case 7:
                    predicate = 7;
                    break;
                case 8:
                    predicate = 3;
                    break;
                case 9:
                    predicate = 6;
                    swap = true;
                    break;
                case 10:
                    predicate = 0;
                    break;
                case 11:
                    predicate = 5;
                    swap = true;
                    break;
                case 12:
                    predicate = 6;
                    break;
                case 13:
                    predicate = 4;
                    break;
                case 14:
                    predicate = 5;
                    break;
                default:
                    break;
            }
            movdqa(xmm14, swap ? xmm1 : xmm0);
            compare(xmm14, swap ? xmm0 : xmm1, predicate);
            if (relations == 5 || relations == 10) {
                compare(xmm0, xmm1, relations == 5 ? 7 : 3);
                if (relations == 5)
                    pand(xmm14, xmm0);
                else
                    por(xmm14, xmm0);
            } else if (relations == 0)
                pxor(xmm14, xmm14);
            else if (relations == 15)
                pcmpeqd(xmm14, xmm14);
            movdqa(xmm0, xmm14);
            if (scalar) {
                if (bits == 32)
                    movss(xmm15, xmm0);
                else
                    movsd(xmm15, xmm0);
                movdqa(xmm0, xmm15);
            }
        }
    } else if (unary) {
        const auto kind = c.Arg(3).Payload();
        if (kind > 2) Reject(CompileError::UnsupportedOperand);
        if (bits == 32) {
            if (kind == 0) {
                if (scalar)
                    sqrtss(xmm0, xmm1);
                else
                    sqrtps(xmm0, xmm1);
            } else {
                movdqa(xmm14, xmm0);
                if (kind == 1) {
                    if (scalar)
                        rcpss(xmm0, xmm1);
                    else
                        rcpps(xmm0, xmm1);
                } else {
                    if (scalar)
                        rsqrtss(xmm0, xmm1);
                    else
                        rsqrtps(xmm0, xmm1);
                }
                if (scalar) {
                    movss(xmm14, xmm0);
                    movdqa(xmm0, xmm14);
                }
            }
        } else if (kind == 0) {
            if (scalar)
                sqrtsd(xmm0, xmm1);
            else
                sqrtpd(xmm0, xmm1);
        } else {
            movdqa(xmm14, xmm0);
            if (kind == 2) sqrtpd(xmm1, xmm1);
            mov(rax, UINT64_C(0x3ff0000000000000));
            movq(xmm0, rax);
            punpcklqdq(xmm0, xmm0);
            divpd(xmm0, xmm1);
            if (scalar) {
                movsd(xmm14, xmm0);
                movdqa(xmm0, xmm14);
            }
        }
    } else if (minmax) {
        // Select the original operand bits: MIN/MAX may flush a selected
        // subnormal under FTZ, while this IR operation only applies DAZ.
        movdqa(xmm15, xmm0);
        movdqa(xmm14, c.Arg(3).Payload() ? xmm1 : xmm0);
        const auto& rhs = c.Arg(3).Payload() ? xmm0 : xmm1;
        if (bits == 32) {
            if (scalar)
                cmpss(xmm14, rhs, 1);
            else
                cmpps(xmm14, rhs, 1);
        } else {
            if (scalar)
                cmpsd(xmm14, rhs, 1);
            else
                cmppd(xmm14, rhs, 1);
        }
        pand(xmm0, xmm14);
        pandn(xmm14, xmm1);
        por(xmm0, xmm14);
        if (scalar) {
            if (bits == 32)
                movss(xmm15, xmm0);
            else
                movsd(xmm15, xmm0);
            movdqa(xmm0, xmm15);
        }
        if (options.mxcsr_offset != UINT32_MAX) {
            Label done;
            test(dword[r12 + options.mxcsr_offset], 64);
            jz(done, T_NEAR);
            mov(rax, bits == 32 ? UINT64_C(0x7f8000007f800000) : UINT64_C(0x7ff0000000000000));
            movq(xmm1, rax);
            punpcklqdq(xmm1, xmm1);
            movdqa(xmm14, xmm0);
            pand(xmm14, xmm1);
            pxor(xmm1, xmm1);
            pcmpeqd(xmm14, xmm1);
            if (bits == 64) {
                pshufd(xmm1, xmm14, 0xb1);
                pand(xmm14, xmm1);
            }
            if (scalar) {
                mov(rax, bits == 32 ? UINT64_C(0xffffffff) : UINT64_MAX);
                movq(xmm1, rax);
                pand(xmm14, xmm1);
            }
            mov(rax, bits == 32 ? UINT64_C(0x8000000080000000) : UINT64_C(0x8000000000000000));
            movq(xmm1, rax);
            punpcklqdq(xmm1, xmm1);
            movdqa(xmm15, xmm0);
            pand(xmm15, xmm1);
            pand(xmm15, xmm14);
            pandn(xmm14, xmm0);
            por(xmm14, xmm15);
            movdqa(xmm0, xmm14);
            L(done);
        }
    } else {
        const bool addop = op == O::VecFAdd || op == O::VecFAddScalar32 || op == O::VecFAddScalar64,
                   subop = op == O::VecFSub || op == O::VecFSubScalar32 || op == O::VecFSubScalar64,
                   mulop = op == O::VecFMul || op == O::VecFMulScalar32 || op == O::VecFMulScalar64;
        if (bits == 32) {
            if (scalar) {
                if (addop)
                    addss(xmm0, xmm1);
                else if (subop)
                    subss(xmm0, xmm1);
                else if (mulop)
                    mulss(xmm0, xmm1);
                else
                    divss(xmm0, xmm1);
            } else {
                if (addop)
                    addps(xmm0, xmm1);
                else if (subop)
                    subps(xmm0, xmm1);
                else if (mulop)
                    mulps(xmm0, xmm1);
                else
                    divps(xmm0, xmm1);
            }
        } else {
            if (scalar) {
                if (addop)
                    addsd(xmm0, xmm1);
                else if (subop)
                    subsd(xmm0, xmm1);
                else if (mulop)
                    mulsd(xmm0, xmm1);
                else
                    divsd(xmm0, xmm1);
            } else {
                if (addop)
                    addpd(xmm0, xmm1);
                else if (subop)
                    subpd(xmm0, xmm1);
                else if (mulop)
                    mulpd(xmm0, xmm1);
                else
                    divpd(xmm0, xmm1);
            }
        }
    }
    if (!c.vector_result) {
        movq(rax, xmm0);
        Mask(rax, bits);
    }
}

#define FP(name)                                                                                   \
    void Writer::Handle(Op<O::name>, OpContext& c) { Float(c); }
// clang-format off
FP(VecFAddScalar32)
FP(VecFSubScalar32)
FP(VecFMulScalar32)
FP(VecFDivScalar32)
FP(VecFAddScalar64)
FP(VecFSubScalar64)
FP(VecFMulScalar64)
FP(VecFDivScalar64)
FP(VecFAdd)
FP(VecFSub)
FP(VecFMul)
FP(VecFDiv)
FP(VecFMinMax)
FP(VecFUnary)
FP(VecFCmpMask)
FP(VecFMulAdd)
FP(VecFRoundInt)
// clang-format on
#undef FP

void Writer::Handle(Op<O::ArmScalarFP>, OpContext&) {
    Reject(CompileError::UnsupportedOpcode);
}

void Writer::Handle(Op<O::ArmScalarFMA>, OpContext&) {
    Reject(CompileError::UnsupportedOpcode);
}

void Writer::Handle(Op<O::VecFCmp>, OpContext& c) {
    ReadVector(c.Arg(0), xmm0);
    ReadVector(c.Arg(1), xmm1);
    const unsigned mode = c.Arg(2).Payload(), bits = mode & 127;
    if (bits == 32) {
        if (mode & 128)
            comiss(xmm0, xmm1);
        else
            ucomiss(xmm0, xmm1);
    } else if (bits == 64) {
        if (mode & 128)
            comisd(xmm0, xmm1);
        else
            ucomisd(xmm0, xmm1);
    } else
        Reject(CompileError::UnsupportedOperand);
    setb(al);
    setp(dl);
    sete(cl);
    movzx(eax, al);
    movzx(edx, dl);
    movzx(ecx, cl);
    shl(edx, 1);
    shl(ecx, 2);
    or_(eax, edx);
    or_(eax, ecx);
}

void Writer::Handle(Op<O::VecFCvtIntToFloat>, OpContext& c) {
    unsigned from = c.Arg(1).Payload(), to = c.Arg(2).Payload();
    if ((from != 32 && from != 64) || (to != 32 && to != 64))
        Reject(CompileError::UnsupportedOperand);
    Read(c.Arg(0), rax, true);
    if (from == 32) movsxd(rax, eax);
    pxor(xmm0, xmm0);
    if (to == 32)
        cvtsi2ss(xmm0, rax);
    else
        cvtsi2sd(xmm0, rax);
    movq(rax, xmm0);
    Mask(rax, to);
}

void Writer::Handle(Op<O::VecFCvtFloatToInt>, OpContext& c) {
    unsigned from = c.Arg(1).Payload(), to = c.Arg(2).Payload();
    const bool trunc = c.Arg(3).Payload();
    if ((from != 32 && from != 64) || (to != 32 && to != 64))
        Reject(CompileError::UnsupportedOperand);
    ReadVector(c.Arg(0), xmm0);
    const auto dst = R(rax, to);
    if (from == 32) {
        if (trunc)
            cvttss2si(dst, xmm0);
        else
            cvtss2si(dst, xmm0);
    } else {
        if (trunc)
            cvttsd2si(dst, xmm0);
        else
            cvtsd2si(dst, xmm0);
    }
}

void Writer::Handle(Op<O::VecFCvtScalar>, OpContext& c) {
    ReadVector(c.Arg(0), xmm1);
    if (c.Arg(1).Payload() == 32) {
        cvtss2sd(xmm0, xmm1);
        movq(rax, xmm0);
    } else if (c.Arg(1).Payload() == 64) {
        cvtsd2ss(xmm0, xmm1);
        movq(rax, xmm0);
        Mask(rax, 32);
    } else
        Reject(CompileError::UnsupportedOperand);
}

void Writer::Handle(Op<O::VecFCvtPacked>, OpContext& c) {
    ReadVector(c.Arg(0), xmm1);
    switch (c.Arg(1).Payload()) {
        case 0:
            cvtdq2ps(xmm0, xmm1);
            break;
        case 1:
            cvtdq2pd(xmm0, xmm1);
            break;
        case 2:
            cvtps2dq(xmm0, xmm1);
            break;
        case 3:
            cvttps2dq(xmm0, xmm1);
            break;
        case 4:
            cvtpd2dq(xmm0, xmm1);
            break;
        case 5:
            cvttpd2dq(xmm0, xmm1);
            break;
        case 6:
            cvtps2pd(xmm0, xmm1);
            break;
        case 7:
            cvtpd2ps(xmm0, xmm1);
            break;
        default:
            Reject(CompileError::UnsupportedOperand);
    }
}
}  // namespace swift::runtime::backend::x64::detail
