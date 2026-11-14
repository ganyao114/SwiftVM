#include "emit.h"

#include <algorithm>

namespace swift::runtime::backend::x64::detail {
using O = OpCode;

namespace {
constexpr bool CanEmitAvxVectorBinary(
        OpCode op, unsigned n, bool sign, bool sse41, bool sse42) noexcept {
    switch (op) {
        case O::VecAnd:
        case O::Vec4And:
        case O::VecOr:
        case O::Vec4Or:
        case O::VecXor:
        case O::VecAndNot:
            return true;
        case O::VecAdd:
        case O::Vec4Add:
        case O::VecSub:
        case O::Vec4Sub:
            return n == 8 || n == 16 || n == 32 || n == 64;
        case O::VecCmpEq:
            return n == 8 || n == 16 || n == 32 || (n == 64 && sse41);
        case O::VecCmpGt:
            return n == 8 || n == 16 || n == 32 || (n == 64 && sse42);
        case O::VecAvg:
        case O::VecSatAdd:
        case O::VecSatSub:
            return n == 8 || n == 16;
        case O::VecMul:
        case O::Vec4Mul:
            return n == 16 || (n == 32 && sse41);
        case O::VecMin:
        case O::VecMax:
            return (n == 8 && (!sign || sse41)) || (n == 16 && (sign || sse41)) ||
                   (n == 32 && sse41);
        default:
            return false;
    }
}

void Lookup(Writer& w, OpContext& c, bool wrap) {
    w.ReadVector(c.Arg(0), w.xmm14);
    w.ReadVector(c.Arg(1), w.xmm15);
    w.pxor(w.xmm0, w.xmm0);
    for (unsigned i = 0; i < 16; ++i) {
        w.ExtractLane(w.xmm15, i, 8, w.rcx);
        Label zero, done;
        if (wrap) {
            w.test(w.ecx, 128);
            w.jnz(zero, w.T_NEAR);
            w.and_(w.ecx, 15);
        } else {
            w.cmp(w.ecx, 16);
            w.jae(zero, w.T_NEAR);
        }
        w.movq(w.rax, w.xmm14);
        w.movhlps(w.xmm1, w.xmm14);
        w.movq(w.rdx, w.xmm1);
        w.bt(w.ecx, 3);
        w.cmovc(w.rax, w.rdx);
        w.and_(w.ecx, 7);
        w.shl(w.ecx, 3);
        w.shr(w.rax, w.cl);
        w.jmp(done, w.T_NEAR);
        w.L(zero);
        w.xor_(w.eax, w.eax);
        w.L(done);
        w.InsertLane(w.xmm0, i, 8, w.rax);
    }
}
}  // namespace

void Writer::ExtractLane(const Xmm& x, unsigned lane, unsigned bits, const Reg64& r, bool sign) {
    movdqa(xmm1, x);
    if (lane * bits / 8) psrldq(xmm1, lane * bits / 8);
    movq(r, xmm1);
    Mask(r, bits);
    if (sign) Sign(r, bits);
}

void Writer::InsertLane(const Xmm& x, unsigned lane, unsigned bits, const Reg64& r) {
    Mask(r, bits);
    movq(xmm1, r);
    if (lane * bits / 8) pslldq(xmm1, lane * bits / 8);
    por(x, xmm1);
}

void Writer::EmitVectorLaneOp(OpCode op, unsigned bits, bool uns) {
    switch (op) {
        case O::VecAdd:
        case O::Vec4Add:
            add(rax, rdx);
            return;
        case O::VecSub:
        case O::Vec4Sub:
            sub(rax, rdx);
            return;
        case O::VecMul:
        case O::Vec4Mul:
            imul(rax, rdx);
            return;
        case O::VecCmpEq:
            cmp(rax, rdx);
            sete(al);
            movzx(eax, al);
            neg(rax);
            return;
        case O::VecCmpGt:
            cmp(rax, rdx);
            setg(al);
            movzx(eax, al);
            neg(rax);
            return;
        case O::VecMin:
        case O::VecMax:
            cmp(rax, rdx);
            if (op == O::VecMin) {
                if (uns)
                    cmova(rax, rdx);
                else
                    cmovg(rax, rdx);
            } else {
                if (uns)
                    cmovb(rax, rdx);
                else
                    cmovl(rax, rdx);
            }
            return;
        case O::VecAvg:
            mov(rcx, rax);
            and_(rax, rdx);
            xor_(rcx, rdx);
            mov(r10, rcx);
            and_(r10d, 1);
            shr(rcx, 1);
            add(rax, rcx);
            add(rax, r10);
            return;
        case O::VecSatAdd:
        case O::VecSatSub: {
            const bool subop = op == O::VecSatSub;
            if (bits == 64) {
                if (uns) {
                    if (subop) {
                        sub(rax, rdx);
                        mov(rcx, 0);
                        cmovc(rax, rcx);
                    } else {
                        add(rax, rdx);
                        mov(rcx, UINT64_MAX);
                        cmovc(rax, rcx);
                    }
                } else {
                    mov(r10, rax);
                    sar(r10, 63);
                    mov(rcx, INT64_MAX);
                    xor_(rcx, r10);
                    if (subop)
                        sub(rax, rdx);
                    else
                        add(rax, rdx);
                    cmovo(rax, rcx);
                }
            } else {
                if (subop)
                    sub(rax, rdx);
                else
                    add(rax, rdx);
                mov(rcx, uns ? 0 : u64{0} - (u64{1} << (bits - 1)));
                cmp(rax, rcx);
                cmovl(rax, rcx);
                mov(rcx, uns ? (u64{1} << bits) - 1 : (u64{1} << (bits - 1)) - 1);
                cmp(rax, rcx);
                cmovg(rax, rcx);
            }
            return;
        }
        default:
            Reject(CompileError::UnsupportedOpcode);
    }
}

void Writer::VectorLanes(OpContext& c) {
    const auto op = c.operation.opcode;
    const unsigned bits =
            (op == O::Vec4Add || op == O::Vec4Sub || op == O::Vec4Mul) ? 32 : c.Arg(2).Payload();
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64)
        Reject(CompileError::UnsupportedOperand);
    ReadVector(c.Arg(0), xmm14);
    ReadVector(c.Arg(1), xmm15);
    pxor(xmm0, xmm0);
    const bool uns =
            (op == O::VecMin || op == O::VecMax || op == O::VecSatAdd || op == O::VecSatSub) &&
            !c.Arg(3).Payload();
    const bool sign =
            op == O::VecCmpGt ||
            ((op == O::VecMin || op == O::VecMax || op == O::VecSatAdd || op == O::VecSatSub) &&
             !uns);
    for (unsigned i = 0; i < 128 / bits; ++i) {
        ExtractLane(xmm14, i, bits, rax, sign);
        ExtractLane(xmm15, i, bits, rdx, sign);
        EmitVectorLaneOp(op, bits, uns);
        InsertLane(xmm0, i, bits, rax);
    }
}

void Writer::EmitAvxVectorBinaryInsn(
        OpContext& c, const Xmm& dst, const Xmm& s1, const Xbyak::Operand& s2, unsigned n) {
    const auto op = c.operation.opcode;
    const bool sign = c.count > 3 && c.Arg(3).Payload() != 0;
    if (op == O::VecAnd || op == O::Vec4And) {
        vpand(dst, s1, s2);
    } else if (op == O::VecOr || op == O::Vec4Or) {
        vpor(dst, s1, s2);
    } else if (op == O::VecXor) {
        vpxor(dst, s1, s2);
    } else if (op == O::VecAndNot) {
        vpandn(dst, s1, s2);
    } else if (op == O::VecAdd || op == O::Vec4Add) {
        if (n == 8)
            vpaddb(dst, s1, s2);
        else if (n == 16)
            vpaddw(dst, s1, s2);
        else if (n == 32)
            vpaddd(dst, s1, s2);
        else
            vpaddq(dst, s1, s2);
    } else if (op == O::VecSub || op == O::Vec4Sub) {
        if (n == 8)
            vpsubb(dst, s1, s2);
        else if (n == 16)
            vpsubw(dst, s1, s2);
        else if (n == 32)
            vpsubd(dst, s1, s2);
        else
            vpsubq(dst, s1, s2);
    } else if (op == O::VecCmpEq) {
        if (n == 8)
            vpcmpeqb(dst, s1, s2);
        else if (n == 16)
            vpcmpeqw(dst, s1, s2);
        else if (n == 32)
            vpcmpeqd(dst, s1, s2);
        else
            vpcmpeqq(dst, s1, s2);
    } else if (op == O::VecCmpGt) {
        if (n == 8)
            vpcmpgtb(dst, s1, s2);
        else if (n == 16)
            vpcmpgtw(dst, s1, s2);
        else if (n == 32)
            vpcmpgtd(dst, s1, s2);
        else
            vpcmpgtq(dst, s1, s2);
    } else if (op == O::VecAvg) {
        if (n == 8)
            vpavgb(dst, s1, s2);
        else
            vpavgw(dst, s1, s2);
    } else if (op == O::VecMul || op == O::Vec4Mul) {
        if (n == 16)
            vpmullw(dst, s1, s2);
        else
            vpmulld(dst, s1, s2);
    } else if (op == O::VecMin || op == O::VecMax) {
        const bool mx = op == O::VecMax;
        if (n == 8) {
            if (sign)
                mx ? vpmaxsb(dst, s1, s2) : vpminsb(dst, s1, s2);
            else
                mx ? vpmaxub(dst, s1, s2) : vpminub(dst, s1, s2);
        } else if (n == 16) {
            if (sign)
                mx ? vpmaxsw(dst, s1, s2) : vpminsw(dst, s1, s2);
            else
                mx ? vpmaxuw(dst, s1, s2) : vpminuw(dst, s1, s2);
        } else if (sign) {
            mx ? vpmaxsd(dst, s1, s2) : vpminsd(dst, s1, s2);
        } else {
            mx ? vpmaxud(dst, s1, s2) : vpminud(dst, s1, s2);
        }
    } else if (op == O::VecSatAdd) {
        if (n == 8)
            sign ? vpaddsb(dst, s1, s2) : vpaddusb(dst, s1, s2);
        else
            sign ? vpaddsw(dst, s1, s2) : vpaddusw(dst, s1, s2);
    } else {
        if (n == 8)
            sign ? vpsubsb(dst, s1, s2) : vpsubusb(dst, s1, s2);
        else
            sign ? vpsubsw(dst, s1, s2) : vpsubusw(dst, s1, s2);
    }
}

bool Writer::TryEmitAvxVectorBinary(OpContext& c, const Xmm& dst, unsigned n) {
    const auto op = c.operation.opcode;
    if ((op == O::VecAdd || op == O::VecSub) && n != 8 && n != 16 && n != 32 && n != 64)
        Reject(CompileError::UnsupportedOperand);
    const bool sign = c.count > 3 && c.Arg(3).Payload() != 0;
    if (!options.avx || !CanEmitAvxVectorBinary(op, n, sign, options.sse41, options.sse42))
        return false;
    if (op == O::VecAndNot) {
        Xmm b_xmm = xmm1;
        if (!ResidentVector(c.Arg(1), b_xmm)) {
            ReadVector(c.Arg(1), xmm1);
            b_xmm = xmm1;
        }
        Xbyak::Address mem;
        Xmm a_xmm = dst;
        if (MemoryVectorOperand(c.Arg(0), mem)) {
            EmitAvxVectorBinaryInsn(c, dst, b_xmm, mem, n);
        } else if (ResidentVector(c.Arg(0), a_xmm)) {
            EmitAvxVectorBinaryInsn(c, dst, b_xmm, a_xmm, n);
        } else {
            const Xmm tmp = b_xmm.getIdx() == 0 ? xmm1 : xmm0;
            ReadVector(c.Arg(0), tmp);
            EmitAvxVectorBinaryInsn(c, dst, b_xmm, tmp, n);
        }
        return true;
    }
    const bool comm = op != O::VecSub && op != O::Vec4Sub && op != O::VecCmpGt &&
                      op != O::VecSatSub;
    Xbyak::Address mem;
    if (MemoryVectorOperand(c.Arg(1), mem)) {
        Xmm s1 = dst;
        if (!ResidentVector(c.Arg(0), s1)) {
            ReadVector(c.Arg(0), dst);
            s1 = dst;
        }
        EmitAvxVectorBinaryInsn(c, dst, s1, mem, n);
        return true;
    }
    if (comm && MemoryVectorOperand(c.Arg(0), mem)) {
        Xmm s1 = dst;
        if (!ResidentVector(c.Arg(1), s1)) {
            ReadVector(c.Arg(1), dst);
            s1 = dst;
        }
        EmitAvxVectorBinaryInsn(c, dst, s1, mem, n);
        return true;
    }
    Xmm s1 = xmm1, s2 = xmm0;
    const bool r0 = ResidentVector(c.Arg(0), s1);
    const bool r1 = ResidentVector(c.Arg(1), s2);
    if (!r0) {
        s1 = (r1 && s2.getIdx() == 0) ? xmm1 : dst;
        ReadVector(c.Arg(0), s1);
    }
    if (!r1) {
        s2 = s1.getIdx() == 1 ? xmm0 : xmm1;
        ReadVector(c.Arg(1), s2);
    }
    EmitAvxVectorBinaryInsn(c, dst, s1, s2, n);
    return true;
}

bool Writer::TryEmitSseVectorBitwiseOrAddSub(OpCode op, const Xmm& dst, unsigned n) {
    if (op == O::VecAnd || op == O::Vec4And) {
        if (options.avx)
            vpand(dst, dst, xmm1);
        else
            pand(dst, xmm1);
        return true;
    }
    if (op == O::VecOr || op == O::Vec4Or) {
        if (options.avx)
            vpor(dst, dst, xmm1);
        else
            por(dst, xmm1);
        return true;
    }
    if (op == O::VecXor) {
        if (options.avx)
            vpxor(dst, dst, xmm1);
        else
            pxor(dst, xmm1);
        return true;
    }
    if (op == O::VecAndNot) {
        if (options.avx) {
            vpandn(dst, xmm1, dst);
        } else {
            pandn(xmm1, dst);
            movdqa(dst, xmm1);
        }
        return true;
    }
    if (op == O::VecAdd || op == O::Vec4Add) {
        if (options.avx) {
            if (n == 8)
                vpaddb(dst, dst, xmm1);
            else if (n == 16)
                vpaddw(dst, dst, xmm1);
            else if (n == 32)
                vpaddd(dst, dst, xmm1);
            else if (n == 64)
                vpaddq(dst, dst, xmm1);
            else
                Reject(CompileError::UnsupportedOperand);
        } else {
            if (n == 8)
                paddb(dst, xmm1);
            else if (n == 16)
                paddw(dst, xmm1);
            else if (n == 32)
                paddd(dst, xmm1);
            else if (n == 64)
                paddq(dst, xmm1);
            else
                Reject(CompileError::UnsupportedOperand);
        }
        return true;
    }
    if (op == O::VecSub || op == O::Vec4Sub) {
        if (options.avx) {
            if (n == 8)
                vpsubb(dst, dst, xmm1);
            else if (n == 16)
                vpsubw(dst, dst, xmm1);
            else if (n == 32)
                vpsubd(dst, dst, xmm1);
            else if (n == 64)
                vpsubq(dst, dst, xmm1);
            else
                Reject(CompileError::UnsupportedOperand);
        } else {
            if (n == 8)
                psubb(dst, xmm1);
            else if (n == 16)
                psubw(dst, xmm1);
            else if (n == 32)
                psubd(dst, xmm1);
            else if (n == 64)
                psubq(dst, xmm1);
            else
                Reject(CompileError::UnsupportedOperand);
        }
        return true;
    }
    return false;
}

bool Writer::TryEmitSseVectorCompareOrMinMax(OpContext& c, const Xmm& dst, unsigned n) {
    const auto op = c.operation.opcode;
    if (op == O::VecCmpEq) {
        if (n == 8)
            pcmpeqb(dst, xmm1);
        else if (n == 16)
            pcmpeqw(dst, xmm1);
        else if (n == 32)
            pcmpeqd(dst, xmm1);
        else if (n == 64 && options.sse41)
            pcmpeqq(dst, xmm1);
        else
            return false;
        return true;
    }
    if (op == O::VecCmpGt) {
        if (n == 8)
            pcmpgtb(dst, xmm1);
        else if (n == 16)
            pcmpgtw(dst, xmm1);
        else if (n == 32)
            pcmpgtd(dst, xmm1);
        else if (n == 64 && options.sse42)
            pcmpgtq(dst, xmm1);
        else
            return false;
        return true;
    }
    if (op == O::VecAvg && (n == 8 || n == 16)) {
        if (n == 8)
            pavgb(dst, xmm1);
        else
            pavgw(dst, xmm1);
        return true;
    }
    if ((op == O::VecMul || op == O::Vec4Mul) && n == 16) {
        pmullw(dst, xmm1);
        return true;
    }
    if ((op == O::VecMul || op == O::Vec4Mul) && n == 32 && options.sse41) {
        pmulld(dst, xmm1);
        return true;
    }
    if (op == O::VecMin || op == O::VecMax) {
        const bool sign = c.Arg(3).Payload(), maximum = op == O::VecMax;
        if (n == 8 && !sign) {
            if (maximum)
                pmaxub(dst, xmm1);
            else
                pminub(dst, xmm1);
            return true;
        }
        if (n == 16 && sign) {
            if (maximum)
                pmaxsw(dst, xmm1);
            else
                pminsw(dst, xmm1);
            return true;
        }
        if (options.sse41) {
            if (n == 8 && sign) {
                if (maximum)
                    pmaxsb(dst, xmm1);
                else
                    pminsb(dst, xmm1);
                return true;
            }
            if (n == 16 && !sign) {
                if (maximum)
                    pmaxuw(dst, xmm1);
                else
                    pminuw(dst, xmm1);
                return true;
            }
            if (n == 32) {
                if (sign) {
                    if (maximum)
                        pmaxsd(dst, xmm1);
                    else
                        pminsd(dst, xmm1);
                } else {
                    if (maximum)
                        pmaxud(dst, xmm1);
                    else
                        pminud(dst, xmm1);
                }
                return true;
            }
        }
        return false;
    }
    if ((op == O::VecSatAdd || op == O::VecSatSub) && (n == 8 || n == 16)) {
        const bool u = !c.Arg(3).Payload();
        if (op == O::VecSatAdd) {
            if (n == 8) {
                if (u)
                    paddusb(dst, xmm1);
                else
                    paddsb(dst, xmm1);
            } else {
                if (u)
                    paddusw(dst, xmm1);
                else
                    paddsw(dst, xmm1);
            }
        } else {
            if (n == 8) {
                if (u)
                    psubusb(dst, xmm1);
                else
                    psubsb(dst, xmm1);
            } else {
                if (u)
                    psubusw(dst, xmm1);
                else
                    psubsw(dst, xmm1);
            }
        }
        return true;
    }
    return false;
}

void Writer::Vector(OpContext& c) {
    const auto op = c.operation.opcode;
    const auto& home = allocation.homes[c.node.value];
    const Xmm dst = home.spilled ? xmm0 : Xmm(home.reg);
    c.vector = dst;
    const unsigned n =
            (op == O::Vec4Add || op == O::Vec4Sub || op == O::Vec4Mul) ? 32 : c.Arg(2).Payload();
    if (TryEmitAvxVectorBinary(c, dst, n)) return;
    ReadVector(c.Arg(1), xmm1);
    ReadVector(c.Arg(0), dst);
    if (TryEmitSseVectorBitwiseOrAddSub(op, dst, n)) return;
    if (TryEmitSseVectorCompareOrMinMax(c, dst, n)) return;
    c.vector = xmm0;
    VectorLanes(c);
}

#define VB(name)                                                                                   \
    void Writer::Handle(Op<O::name>, OpContext& c) { Vector(c); }
// clang-format off
VB(Vec4Add)
VB(Vec4Sub)
VB(Vec4Mul)
VB(Vec4And)
VB(Vec4Or)
VB(VecXor)
VB(VecAnd)
VB(VecOr)
VB(VecAndNot)
VB(VecAdd)
VB(VecSub)
VB(VecCmpEq)
VB(VecCmpGt)
VB(VecAvg)
VB(VecMin)
VB(VecMax)
VB(VecMul)
VB(VecSatAdd)
VB(VecSatSub)
// clang-format on
#undef VB

void Writer::Handle(Op<O::VecMulHigh16>, OpContext& c) {
    ReadVector(c.Arg(0), xmm0);
    ReadVector(c.Arg(1), xmm1);
    if (c.Arg(2).Payload())
        pmulhw(xmm0, xmm1);
    else
        pmulhuw(xmm0, xmm1);
}

void Writer::Handle(Op<O::VecMadd16>, OpContext& c) {
    ReadVector(c.Arg(0), xmm0);
    ReadVector(c.Arg(1), xmm1);
    pmaddwd(xmm0, xmm1);
}

void Writer::Handle(Op<O::VecAbsDiffSum8>, OpContext& c) {
    ReadVector(c.Arg(0), xmm0);
    ReadVector(c.Arg(1), xmm1);
    psadbw(xmm0, xmm1);
}

void Writer::Handle(Op<O::VecLoadConst>, OpContext& c) {
    const auto& home = allocation.homes[c.node.value];
    const Xmm dst = (!home.spilled && !home.terminal_only) ? Xmm(home.reg) : xmm0;
    c.vector = dst;
    const u64 lo = c.Arg(0).Payload(), hi = c.Arg(1).Payload();
    if (lo == 0 && hi == 0) {
        if (options.avx)
            vpxor(dst, dst, dst);
        else
            pxor(dst, dst);
        return;
    }
    if (hi == 0) {
        mov(rax, lo);
        if (options.avx)
            vmovq(dst, rax);
        else
            movq(dst, rax);
        return;
    }
    if (lo == hi && options.avx) {
        mov(rax, lo);
        vmovq(dst, rax);
        vmovddup(dst, dst);
        return;
    }
    mov(rax, lo);
    if (options.avx) {
        vmovq(dst, rax);
        mov(rax, hi);
        vmovq(xmm1, rax);
        vpunpcklqdq(dst, dst, xmm1);
    } else {
        movq(dst, rax);
        mov(rax, hi);
        movq(xmm1, rax);
        punpcklqdq(dst, xmm1);
    }
}

void Writer::Handle(Op<O::VecSharedZero>, OpContext& c) {
    const auto& home = allocation.homes[c.node.value];
    const Xmm dst = home.spilled ? xmm0 : Xmm(home.reg);
    c.vector = dst;
    if (options.avx)
        vpxor(dst, dst, dst);
    else
        pxor(dst, dst);
}

void Writer::Handle(Op<O::VecDup64>, OpContext& c) {
    const auto& home = allocation.homes[c.node.value];
    const Xmm dst = (!home.spilled && !home.terminal_only) ? Xmm(home.reg) : xmm0;
    c.vector = dst;
    if (options.avx) {
        const auto arg = c.Arg(0);
        if (arg.Kind() == OperandKind::Value &&
            TypeBits(source.Operations()[arg.Payload()].type) == 64) {
            const auto load_id = static_cast<u32>(arg.Payload());
            const auto& ah = allocation.homes[load_id];
            const auto& lop = source.Operations()[load_id];
            if (ah.rematerialize &&
                (lop.opcode == OpCode::LoadMemory || lop.opcode == OpCode::LoadMemoryTSO)) {
                const auto ops = source.Operands(NodeId{load_id});
                auto at = ops.begin();
                const auto addr = MemoryAddress(at, ops.end());
                if (dst.getIdx() == 0) xmm0_node = NodeId::invalid;
                const auto first = static_cast<u32>(getSize());
                vmovddup(dst, qword[addr]);
                RecordGuestAccess(first, 1, 8);
                return;
            }
            if (ah.spilled && ah.uniform_offset != UINT32_MAX) {
                vmovddup(dst, qword[r12 + ah.uniform_offset]);
                return;
            }
            if (ah.spilled && !ah.rematerialize) {
                vmovddup(dst, qword[rsp + ah.stack_offset]);
                return;
            }
        }
        ReadVector(arg, dst);
        vmovddup(dst, dst);
        return;
    }
    ReadVector(c.Arg(0), dst);
    punpcklqdq(dst, dst);
}

void Writer::Handle(Op<O::VecScalar>, OpContext& c) {
    const auto& home = allocation.homes[c.node.value];
    const Xmm dst = (!home.spilled && !home.terminal_only) ? Xmm(home.reg) : xmm0;
    c.vector = dst;
    const auto arg = c.Arg(0);
    if (arg.Kind() == OperandKind::Value) {
        const auto def_bits = TypeBits(source.Operations()[arg.Payload()].type);
        const auto bits = std::min(def_bits, TypeBits(View(arg)));
        const auto& ah = allocation.homes[arg.Payload()];
        if ((bits == 32 || bits == 64) && ah.spilled && ah.uniform_offset != UINT32_MAX) {
            if (bits == 32)
                options.avx ? vmovd(dst, dword[r12 + ah.uniform_offset])
                            : movd(dst, dword[r12 + ah.uniform_offset]);
            else
                options.avx ? vmovq(dst, qword[r12 + ah.uniform_offset])
                            : movq(dst, qword[r12 + ah.uniform_offset]);
            return;
        }
        if ((bits == 32 || bits == 64) && ah.spilled && !ah.rematerialize) {
            if (bits == 32)
                options.avx ? vmovd(dst, dword[rsp + ah.stack_offset])
                            : movd(dst, dword[rsp + ah.stack_offset]);
            else
                options.avx ? vmovq(dst, qword[rsp + ah.stack_offset])
                            : movq(dst, qword[rsp + ah.stack_offset]);
            return;
        }
    }
    Read(arg, rax);
    if (options.avx)
        vmovq(dst, rax);
    else
        movq(dst, rax);
}

void Writer::Handle(Op<O::VecExtract64>, OpContext& c) {
    const auto lane = c.Arg(1).Payload();
    if (lane > 1) Reject(CompileError::UnsupportedOperand);
    if (c.Arg(0).Kind() == OperandKind::Value &&
        source.Operations()[c.Arg(0).Payload()].type == ir::Type::V128) {
        const auto& ah = allocation.homes[c.Arg(0).Payload()];
        if (ah.spilled && ah.uniform_offset != UINT32_MAX) {
            mov(ScalarDestination(c), qword[r12 + ah.uniform_offset + lane * 8]);
            c.scalar_normalized = true;
            return;
        }
        if (ah.spilled) {
            mov(ScalarDestination(c), qword[rsp + ah.stack_offset + lane * 8]);
            c.scalar_normalized = true;
            return;
        }
        if (lane == 0) {
            if (options.avx)
                vmovq(ScalarDestination(c), Xmm(ah.reg));
            else
                movq(ScalarDestination(c), Xmm(ah.reg));
            c.scalar_normalized = true;
            return;
        }
        if (options.avx) {
            vpextrq(ScalarDestination(c), Xmm(ah.reg), 1);
            c.scalar_normalized = true;
            return;
        }
        if (options.sse41) {
            pextrq(ScalarDestination(c), Xmm(ah.reg), 1);
            c.scalar_normalized = true;
            return;
        }
    }
    ReadVector(c.Arg(0), xmm14);
    ExtractLane(xmm14, lane, 64, ScalarDestination(c));
    c.scalar_normalized = true;
}

void Writer::Handle(Op<O::VecExtract16>, OpContext& c) {
    if (c.Arg(1).Payload() > 7) Reject(CompileError::UnsupportedOperand);
    ReadVector(c.Arg(0), xmm0);
    pextrw(eax, xmm0, c.Arg(1).Payload());
}

void Writer::Handle(Op<O::VecInsert16>, OpContext& c) {
    if (c.Arg(2).Payload() > 7) Reject(CompileError::UnsupportedOperand);
    ReadVector(c.Arg(0), xmm0);
    Read(c.Arg(1), rax);
    pinsrw(xmm0, eax, c.Arg(2).Payload());
}

void Writer::Handle(Op<O::VecShuffle32>, OpContext& c) {
    const auto& home = allocation.homes[c.node.value];
    const Xmm dst = home.spilled ? xmm0 : Xmm(home.reg);
    c.vector = dst;
    if (c.Arg(0).Kind() == OperandKind::Value &&
        source.Operations()[c.Arg(0).Payload()].type == ir::Type::V128 &&
        !allocation.homes[c.Arg(0).Payload()].spilled) {
        pshufd(dst, Xmm(allocation.homes[c.Arg(0).Payload()].reg), c.Arg(1).Payload());
    } else {
        ReadVector(c.Arg(0), xmm1);
        pshufd(dst, xmm1, c.Arg(1).Payload());
    }
}

void Writer::Handle(Op<O::VecShuffle32TwoSrc>, OpContext& c) {
    ReadVector(c.Arg(0), xmm0);
    ReadVector(c.Arg(1), xmm1);
    shufps(xmm0, xmm1, c.Arg(2).Payload());
}

void Writer::Handle(Op<O::VecShuffle16>, OpContext& c) {
    ReadVector(c.Arg(0), xmm1);
    if (c.Arg(2).Payload())
        pshufhw(xmm0, xmm1, c.Arg(1).Payload());
    else
        pshuflw(xmm0, xmm1, c.Arg(1).Payload());
}

void Writer::Handle(Op<O::VecDupPairs32>, OpContext& c) {
    if (c.Arg(1).Payload() > 1) Reject(CompileError::UnsupportedOperand);
    ReadVector(c.Arg(0), xmm1);
    pshufd(xmm0, xmm1, c.Arg(1).Payload() ? 0xf5 : 0xa0);
}

void Writer::Handle(Op<O::VecZip>, OpContext& c) {
    const auto& home = allocation.homes[c.node.value];
    const Xmm dst = home.spilled ? xmm0 : Xmm(home.reg);
    c.vector = dst;
    ReadVector(c.Arg(1), xmm1);
    ReadVector(c.Arg(0), dst);
    const auto n = c.Arg(2).Payload();
    const bool h = c.Arg(3).Payload();
    if (h) {
        if (n == 8)
            punpckhbw(dst, xmm1);
        else if (n == 16)
            punpckhwd(dst, xmm1);
        else if (n == 32)
            punpckhdq(dst, xmm1);
        else if (n == 64)
            punpckhqdq(dst, xmm1);
        else
            Reject(CompileError::UnsupportedOperand);
    } else {
        if (n == 8)
            punpcklbw(dst, xmm1);
        else if (n == 16)
            punpcklwd(dst, xmm1);
        else if (n == 32)
            punpckldq(dst, xmm1);
        else if (n == 64)
            punpcklqdq(dst, xmm1);
        else
            Reject(CompileError::UnsupportedOperand);
    }
}

void Writer::Handle(Op<O::VecUnzip>, OpContext& c) {
    const auto bits = c.Arg(2).Payload();
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64)
        Reject(CompileError::UnsupportedOperand);
    const bool odd = c.Arg(3).Payload();
    const auto& home = allocation.homes[c.node.value];
    const Xmm dst = home.spilled ? xmm0 : Xmm(home.reg);
    if (bits == 64) {
        c.vector = dst;
        ReadVector(c.Arg(1), xmm1);
        ReadVector(c.Arg(0), dst);
        if (odd)
            punpckhqdq(dst, xmm1);
        else
            punpcklqdq(dst, xmm1);
        return;
    }
    if (bits == 32) {
        c.vector = dst;
        ReadVector(c.Arg(1), xmm1);
        ReadVector(c.Arg(0), dst);
        shufps(dst, xmm1, odd ? 0xdd : 0x88);
        return;
    }
    if (options.ssse3) {
        c.vector = dst;
        ReadVector(c.Arg(1), xmm1);
        ReadVector(c.Arg(0), dst);
        const u64 mask = bits == 8 ? (odd ? 0x0f0d0b0907050301ULL : 0x0e0c0a0806040200ULL)
                                   : (odd ? 0x0f0e0b0a07060302ULL : 0x0d0c090805040100ULL);
        mov(rax, mask);
        movq(xmm14, rax);
        pshufb(dst, xmm14);
        pshufb(xmm1, xmm14);
        punpcklqdq(dst, xmm1);
        return;
    }
    ReadVector(c.Arg(0), xmm14);
    ReadVector(c.Arg(1), xmm15);
    pxor(xmm0, xmm0);
    for (unsigned i = 0; i < 128 / bits; ++i) {
        const unsigned at = 2 * i + odd;
        ExtractLane(at >= 128 / bits ? xmm15 : xmm14, at % (128 / bits), bits, rax);
        InsertLane(xmm0, i, bits, rax);
    }
}

void Writer::Handle(Op<O::VecExtractBytes>, OpContext& c) {
    const unsigned n = c.Arg(2).Payload();
    if (n > 16) Reject(CompileError::UnsupportedOperand);
    ReadVector(c.Arg(0), xmm0);
    ReadVector(c.Arg(1), xmm1);
    psrldq(xmm0, n);
    pslldq(xmm1, 16 - n);
    por(xmm0, xmm1);
}

void Writer::Handle(Op<O::VecByteShift>, OpContext& c) {
    const unsigned n = c.Arg(2).Payload();
    if (n > 16) Reject(CompileError::UnsupportedOperand);
    ReadVector(c.Arg(0), xmm0);
    ReadVector(c.Arg(1), xmm1);
    if (c.Arg(3).Payload()) {
        pslldq(xmm0, n);
        psrldq(xmm1, 16 - n);
    } else {
        psrldq(xmm0, n);
        pslldq(xmm1, 16 - n);
    }
    por(xmm0, xmm1);
}

void Writer::Handle(Op<O::VecMovMask>, OpContext& c) {
    ReadVector(c.Arg(0), xmm14);
    const unsigned b = c.Arg(1).Payload();
    if (b == 8) {
        pmovmskb(eax, xmm14);
    } else if (b == 32) {
        movmskps(eax, xmm14);
    } else if (b == 64) {
        movmskpd(eax, xmm14);
    } else if (b == 16) {
        psraw(xmm14, 15);
        packsswb(xmm14, xmm14);
        pmovmskb(eax, xmm14);
        and_(eax, 255);
    } else {
        Reject(CompileError::UnsupportedOperand);
    }
}

void Writer::Handle(Op<O::VecExtend>, OpContext& c) {
    const unsigned from = c.Arg(1).Payload(), to = c.Arg(2).Payload();
    if ((from != 8 && from != 16 && from != 32) || (to != 16 && to != 32 && to != 64) ||
        from >= to)
        Reject(CompileError::UnsupportedOperand);
    if (options.sse41) {
        const auto& home = allocation.homes[c.node.value];
        const Xmm dst = home.spilled ? xmm0 : Xmm(home.reg);
        c.vector = dst;
        Xmm src = xmm1;
        if (c.Arg(0).Kind() == OperandKind::Value &&
            source.Operations()[c.Arg(0).Payload()].type == ir::Type::V128 &&
            !allocation.homes[c.Arg(0).Payload()].spilled) {
            src = Xmm(allocation.homes[c.Arg(0).Payload()].reg);
        } else {
            ReadVector(c.Arg(0), xmm1);
        }
        const bool sign = c.Arg(3).Payload();
        if (from == 8 && to == 16) {
            if (sign)
                pmovsxbw(dst, src);
            else
                pmovzxbw(dst, src);
        } else if (from == 8 && to == 32) {
            if (sign)
                pmovsxbd(dst, src);
            else
                pmovzxbd(dst, src);
        } else if (from == 8 && to == 64) {
            if (sign)
                pmovsxbq(dst, src);
            else
                pmovzxbq(dst, src);
        } else if (from == 16 && to == 32) {
            if (sign)
                pmovsxwd(dst, src);
            else
                pmovzxwd(dst, src);
        } else if (from == 16 && to == 64) {
            if (sign)
                pmovsxwq(dst, src);
            else
                pmovzxwq(dst, src);
        } else {
            if (sign)
                pmovsxdq(dst, src);
            else
                pmovzxdq(dst, src);
        }
        return;
    }
    ReadVector(c.Arg(0), xmm14);
    pxor(xmm0, xmm0);
    for (unsigned i = 0; i < 128 / to; ++i) {
        ExtractLane(xmm14, i, from, rax, c.Arg(3).Payload());
        InsertLane(xmm0, i, to, rax);
    }
}

void Writer::Handle(Op<O::VecPack>, OpContext& c) {
    const unsigned from = c.Arg(2).Payload(), to = from / 2;
    if (from != 16 && from != 32 && from != 64) Reject(CompileError::UnsupportedOperand);
    const bool uns = c.Arg(3).Payload();
    if (from == 16 || (from == 32 && (!uns || options.sse41))) {
        ReadVector(c.Arg(0), xmm0);
        ReadVector(c.Arg(1), xmm1);
        if (from == 16) {
            if (uns)
                packuswb(xmm0, xmm1);
            else
                packsswb(xmm0, xmm1);
        } else {
            if (uns)
                packusdw(xmm0, xmm1);
            else
                packssdw(xmm0, xmm1);
        }
        return;
    }
    ReadVector(c.Arg(0), xmm14);
    ReadVector(c.Arg(1), xmm15);
    pxor(xmm0, xmm0);
    for (unsigned i = 0; i < 256 / from; ++i) {
        ExtractLane(i >= 128 / from ? xmm15 : xmm14, i % (128 / from), from, rax, true);
        mov(rcx, uns ? 0 : u64{0} - (u64{1} << (to - 1)));
        cmp(rax, rcx);
        cmovl(rax, rcx);
        mov(rcx, uns ? (u64{1} << to) - 1 : (u64{1} << (to - 1)) - 1);
        cmp(rax, rcx);
        cmovg(rax, rcx);
        InsertLane(xmm0, i, to, rax);
    }
}

void Writer::Handle(Op<O::VecMulWiden>, OpContext& c) {
    const unsigned n = c.Arg(2).Payload();
    if (n != 8 && n != 16 && n != 32) Reject(CompileError::UnsupportedOperand);
    const bool sign = c.Arg(3).Payload() != 0;
    const auto& home = allocation.homes[c.node.value];
    const Xmm dst = home.spilled ? xmm0 : Xmm(home.reg);
    if (n == 32 && (!sign || options.sse41)) {
        c.vector = dst;
        ReadVector(c.Arg(1), xmm1);
        ReadVector(c.Arg(0), dst);
        if (sign)
            options.avx ? vpmuldq(dst, dst, xmm1) : pmuldq(dst, xmm1);
        else
            options.avx ? vpmuludq(dst, dst, xmm1) : pmuludq(dst, xmm1);
        return;
    }
    if (n == 8 || (n == 16 && options.sse41)) {
        c.vector = dst;
        ReadVector(c.Arg(1), xmm1);
        ReadVector(c.Arg(0), dst);
        if (n == 8) {
            psllw(dst, 8);
            psllw(xmm1, 8);
            sign ? psraw(dst, 8) : psrlw(dst, 8);
            sign ? psraw(xmm1, 8) : psrlw(xmm1, 8);
            pmullw(dst, xmm1);
        } else {
            pslld(dst, 16);
            pslld(xmm1, 16);
            sign ? psrad(dst, 16) : psrld(dst, 16);
            sign ? psrad(xmm1, 16) : psrld(xmm1, 16);
            pmulld(dst, xmm1);
        }
        return;
    }
    ReadVector(c.Arg(0), xmm14);
    ReadVector(c.Arg(1), xmm15);
    pxor(xmm0, xmm0);
    for (unsigned i = 0; i < 64 / n; ++i) {
        ExtractLane(xmm14, i * 2, n, rax, sign);
        ExtractLane(xmm15, i * 2, n, rdx, sign);
        imul(rax, rdx);
        InsertLane(xmm0, i, n * 2, rax);
    }
}

void Writer::Handle(Op<O::VecSignedMulWidenLow>, OpContext& c) {
    const unsigned n = c.Arg(2).Payload();
    if (n != 8 && n != 16 && n != 32) Reject(CompileError::UnsupportedOperand);
    const auto& home = allocation.homes[c.node.value];
    const Xmm dst = home.spilled ? xmm0 : Xmm(home.reg);
    if (options.sse41) {
        c.vector = dst;
        ReadVector(c.Arg(1), xmm1);
        ReadVector(c.Arg(0), dst);
        if (n == 8) {
            options.avx ? vpmovsxbw(dst, dst) : pmovsxbw(dst, dst);
            options.avx ? vpmovsxbw(xmm1, xmm1) : pmovsxbw(xmm1, xmm1);
            options.avx ? vpmullw(dst, dst, xmm1) : pmullw(dst, xmm1);
        } else if (n == 16) {
            options.avx ? vpmovsxwd(dst, dst) : pmovsxwd(dst, dst);
            options.avx ? vpmovsxwd(xmm1, xmm1) : pmovsxwd(xmm1, xmm1);
            options.avx ? vpmulld(dst, dst, xmm1) : pmulld(dst, xmm1);
        } else {
            options.avx ? vpshufd(dst, dst, 0x50) : pshufd(dst, dst, 0x50);
            options.avx ? vpshufd(xmm1, xmm1, 0x50) : pshufd(xmm1, xmm1, 0x50);
            options.avx ? vpmuldq(dst, dst, xmm1) : pmuldq(dst, xmm1);
        }
        return;
    }
    if (n == 16) {
        c.vector = dst;
        ReadVector(c.Arg(1), xmm1);
        ReadVector(c.Arg(0), dst);
        movdqa(xmm14, dst);
        pmullw(dst, xmm1);
        pmulhw(xmm14, xmm1);
        punpcklwd(dst, xmm14);
        return;
    }
    ReadVector(c.Arg(0), xmm14);
    ReadVector(c.Arg(1), xmm15);
    pxor(xmm0, xmm0);
    for (unsigned i = 0; i < 64 / n; ++i) {
        ExtractLane(xmm14, i, n, rax, true);
        ExtractLane(xmm15, i, n, rdx, true);
        imul(rax, rdx);
        InsertLane(xmm0, i, n * 2, rax);
    }
}

void Writer::Handle(Op<O::VecTableLookup8>, OpContext& c) {
    if (options.ssse3) {
        const auto& home = allocation.homes[c.node.value];
        const Xmm dst = home.spilled ? xmm0 : Xmm(home.reg);
        c.vector = dst;
        ReadVector(c.Arg(1), xmm1);
        ReadVector(c.Arg(0), dst);
        options.avx ? vpshufb(dst, dst, xmm1) : pshufb(dst, xmm1);
    } else {
        Lookup(*this, c, true);
    }
}

void Writer::Handle(Op<O::VecShuffle32Indexed>, OpContext& c) {
    if (options.ssse3) {
        const auto& home = allocation.homes[c.node.value];
        const Xmm dst = home.spilled ? xmm0 : Xmm(home.reg);
        c.vector = dst;
        ReadVector(c.Arg(1), xmm1);
        ReadVector(c.Arg(0), dst);
        mov(rax, UINT64_C(0x7070707070707070));
        if (options.avx) {
            vmovq(xmm14, rax);
            vmovddup(xmm14, xmm14);
            vpaddusb(xmm1, xmm1, xmm14);
            vpshufb(dst, dst, xmm1);
        } else {
            movq(xmm14, rax);
            punpcklqdq(xmm14, xmm14);
            paddusb(xmm1, xmm14);
            pshufb(dst, xmm1);
        }
        return;
    }
    Lookup(*this, c, false);
}

void Writer::EmitByteImmVectorShift(OpContext& c, bool left) {
    const auto& home = allocation.homes[c.node.value];
    const Xmm dst = home.spilled ? xmm0 : Xmm(home.reg);
    c.vector = dst;
    const u64 raw = c.Arg(1).Payload();
    if (raw >= 8) {
        pxor(dst, dst);
        return;
    }
    ReadVector(c.Arg(0), dst);
    if (raw == 0) return;
    const unsigned count = static_cast<unsigned>(raw);
    const u64 byte_mask = left ? ((0xffu << count) & 0xffu) : (0xffu >> count);
    if (left)
        psllw(dst, count);
    else
        psrlw(dst, count);
    mov(rax, u64{0x0101010101010101ULL} * byte_mask);
    movq(xmm1, rax);
    punpcklqdq(xmm1, xmm1);
    pand(dst, xmm1);
}

void Writer::EmitHardwareVectorShift(
        OpContext& c, unsigned n, bool left, bool arithmetic, bool imm) {
    const auto& home = allocation.homes[c.node.value];
    const Xmm dst = home.spilled ? xmm0 : Xmm(home.reg);
    c.vector = dst;
    if (!imm) {
        Read(c.Arg(1), rax);
        options.avx ? vmovq(xmm1, rax) : movq(xmm1, rax);
    }
    Xmm src = dst;
    if (!options.avx || !ResidentVector(c.Arg(0), src)) {
        ReadVector(c.Arg(0), dst);
        src = dst;
    }
    if (imm) {
        const unsigned count = std::min<u64>(c.Arg(1).Payload(), 255);
        if (left) {
            if (n == 16)
                options.avx ? vpsllw(dst, src, count) : psllw(dst, count);
            else if (n == 32)
                options.avx ? vpslld(dst, src, count) : pslld(dst, count);
            else
                options.avx ? vpsllq(dst, src, count) : psllq(dst, count);
        } else if (arithmetic) {
            if (n == 16)
                options.avx ? vpsraw(dst, src, count) : psraw(dst, count);
            else
                options.avx ? vpsrad(dst, src, count) : psrad(dst, count);
        } else {
            if (n == 16)
                options.avx ? vpsrlw(dst, src, count) : psrlw(dst, count);
            else if (n == 32)
                options.avx ? vpsrld(dst, src, count) : psrld(dst, count);
            else
                options.avx ? vpsrlq(dst, src, count) : psrlq(dst, count);
        }
        return;
    }
    if (left) {
        if (n == 16)
            options.avx ? vpsllw(dst, src, xmm1) : psllw(dst, xmm1);
        else if (n == 32)
            options.avx ? vpslld(dst, src, xmm1) : pslld(dst, xmm1);
        else
            options.avx ? vpsllq(dst, src, xmm1) : psllq(dst, xmm1);
    } else if (arithmetic) {
        if (n == 16)
            options.avx ? vpsraw(dst, src, xmm1) : psraw(dst, xmm1);
        else
            options.avx ? vpsrad(dst, src, xmm1) : psrad(dst, xmm1);
    } else {
        if (n == 16)
            options.avx ? vpsrlw(dst, src, xmm1) : psrlw(dst, xmm1);
        else if (n == 32)
            options.avx ? vpsrld(dst, src, xmm1) : psrld(dst, xmm1);
        else
            options.avx ? vpsrlq(dst, src, xmm1) : psrlq(dst, xmm1);
    }
}

void Writer::EmitLaneLoopVectorShift(
        OpContext& c, unsigned n, bool left, bool arithmetic, bool imm) {
    ReadVector(c.Arg(0), xmm14);
    if (!imm) {
        if (View(c.Arg(1)) == ir::Type::V128)
            ReadVector(c.Arg(1), xmm15);
        else
            Read(c.Arg(1), r11);
    }
    pxor(xmm0, xmm0);
    for (unsigned i = 0; i < 128 / n; ++i) {
        ExtractLane(xmm14, i, n, rax, arithmetic);
        if (imm)
            mov(rcx, c.Arg(1).Payload());
        else if (View(c.Arg(1)) == ir::Type::V128)
            ExtractLane(xmm15, i, n, rcx);
        else
            mov(rcx, r11);
        Label okay, done;
        cmp(rcx, n);
        jb(okay, T_NEAR);
        if (arithmetic)
            sar(rax, 63);
        else
            xor_(eax, eax);
        jmp(done, T_NEAR);
        L(okay);
        if (left)
            shl(rax, cl);
        else if (arithmetic)
            sar(rax, cl);
        else
            shr(rax, cl);
        L(done);
        InsertLane(xmm0, i, n, rax);
    }
}

void Writer::EmitVectorShift(OpContext& c, bool left, bool arithmetic, bool imm) {
    const unsigned n = c.Arg(2).Payload();
    if (n != 8 && n != 16 && n != 32 && n != 64) Reject(CompileError::UnsupportedOperand);
    if (n == 8 && imm && !arithmetic) {
        EmitByteImmVectorShift(c, left);
        return;
    }
    if (n != 8 && (!arithmetic || n != 64) && (imm || View(c.Arg(1)) != ir::Type::V128)) {
        EmitHardwareVectorShift(c, n, left, arithmetic, imm);
        return;
    }
    EmitLaneLoopVectorShift(c, n, left, arithmetic, imm);
}

#define VS(name, left, arith, imm)                                                                 \
    void Writer::Handle(Op<O::name>, OpContext& c) { EmitVectorShift(c, left, arith, imm); }
// clang-format off
VS(VecShiftLeft, true, false, false)
VS(VecShiftRight, false, false, false)
VS(VecShiftRightArithmetic, false, true, false)
VS(VecShiftLeftImm, true, false, true)
VS(VecShiftRightImm, false, false, true)
VS(VecShiftRightArithmeticImm, false, true, true)
// clang-format on
#undef VS
}  // namespace swift::runtime::backend::x64::detail
