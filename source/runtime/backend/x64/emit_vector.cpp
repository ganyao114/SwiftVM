#include <algorithm>
#include "emit.h"

namespace swift::runtime::backend::x64::detail {
using O = OpCode;

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

void Writer::VectorLanes(OpContext& c) {
    const auto op = c.operation.opcode;
    unsigned bits =
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
        switch (op) {
            case O::VecAdd:
            case O::Vec4Add:
                add(rax, rdx);
                break;
            case O::VecSub:
            case O::Vec4Sub:
                sub(rax, rdx);
                break;
            case O::VecMul:
            case O::Vec4Mul:
                imul(rax, rdx);
                break;
            case O::VecCmpEq:
                cmp(rax, rdx);
                sete(al);
                movzx(eax, al);
                neg(rax);
                break;
            case O::VecCmpGt:
                cmp(rax, rdx);
                setg(al);
                movzx(eax, al);
                neg(rax);
                break;
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
                break;
            case O::VecAvg:
                mov(rcx, rax);
                and_(rax, rdx);
                xor_(rcx, rdx);
                mov(r10, rcx);
                and_(r10d, 1);
                shr(rcx, 1);
                add(rax, rcx);
                add(rax, r10);
                break;
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
                break;
            }
            default:
                Reject(CompileError::UnsupportedOpcode);
        }
        InsertLane(xmm0, i, bits, rax);
    }
}

void Writer::Vector(OpContext& c) {
    auto op = c.operation.opcode;
    const auto& home = allocation.homes[c.node.value];
    const Xmm dst = home.spilled ? xmm0 : Xmm(home.reg);
    c.vector = dst;
    const unsigned n =
            (op == O::Vec4Add || op == O::Vec4Sub || op == O::Vec4Mul) ? 32 : c.Arg(2).Payload();
    if (options.avx &&
        (op == O::VecAnd || op == O::Vec4And || op == O::VecOr || op == O::Vec4Or ||
         op == O::VecXor || op == O::VecAndNot || op == O::VecAdd || op == O::Vec4Add ||
         op == O::VecSub || op == O::Vec4Sub)) {
        const auto emit_vex_bin = [&](const Xmm& s1, const Xbyak::Operand& s2) {
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
                else if (n == 64)
                    vpaddq(dst, s1, s2);
                else
                    Reject(CompileError::UnsupportedOperand);
            } else {
                if (n == 8)
                    vpsubb(dst, s1, s2);
                else if (n == 16)
                    vpsubw(dst, s1, s2);
                else if (n == 32)
                    vpsubd(dst, s1, s2);
                else if (n == 64)
                    vpsubq(dst, s1, s2);
                else
                    Reject(CompileError::UnsupportedOperand);
            }
        };
        if (op == O::VecAndNot) {
            Xmm b_xmm = xmm1;
            if (!ResidentVector(c.Arg(1), b_xmm)) {
                ReadVector(c.Arg(1), xmm1);
                b_xmm = xmm1;
            }
            Xbyak::Address mem;
            Xmm a_xmm = dst;
            if (MemoryVectorOperand(c.Arg(0), mem)) {
                emit_vex_bin(b_xmm, mem);
            } else if (ResidentVector(c.Arg(0), a_xmm)) {
                emit_vex_bin(b_xmm, a_xmm);
            } else {
                const Xmm tmp = b_xmm.getIdx() == 0 ? xmm1 : xmm0;
                ReadVector(c.Arg(0), tmp);
                emit_vex_bin(b_xmm, tmp);
            }
            return;
        }
        const bool comm = op != O::VecSub && op != O::Vec4Sub;
        Xbyak::Address mem;
        if (MemoryVectorOperand(c.Arg(1), mem)) {
            Xmm s1 = dst;
            if (!ResidentVector(c.Arg(0), s1)) {
                ReadVector(c.Arg(0), dst);
                s1 = dst;
            }
            emit_vex_bin(s1, mem);
            return;
        }
        if (comm && MemoryVectorOperand(c.Arg(0), mem)) {
            Xmm s1 = dst;
            if (!ResidentVector(c.Arg(1), s1)) {
                ReadVector(c.Arg(1), dst);
                s1 = dst;
            }
            emit_vex_bin(s1, mem);
            return;
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
        emit_vex_bin(s1, s2);
        return;
    }
    ReadVector(c.Arg(0), dst);
    ReadVector(c.Arg(1), xmm1);
    if (op == O::VecAnd || op == O::Vec4And) {
        if (options.avx)
            vpand(dst, dst, xmm1);
        else
            pand(dst, xmm1);
        return;
    }
    if (op == O::VecOr || op == O::Vec4Or) {
        if (options.avx)
            vpor(dst, dst, xmm1);
        else
            por(dst, xmm1);
        return;
    }
    if (op == O::VecXor) {
        if (options.avx)
            vpxor(dst, dst, xmm1);
        else
            pxor(dst, xmm1);
        return;
    }
    if (op == O::VecAndNot) {
        if (options.avx) {
            vpandn(dst, xmm1, dst);
        } else {
            pandn(xmm1, dst);
            movdqa(dst, xmm1);
        }
        return;
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
        return;
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
        return;
    }
    if (op == O::VecCmpEq) {
        if (n == 8)
            pcmpeqb(dst, xmm1);
        else if (n == 16)
            pcmpeqw(dst, xmm1);
        else if (n == 32)
            pcmpeqd(dst, xmm1);
        else if (n == 64 && options.sse41)
            pcmpeqq(dst, xmm1);
        else {
            c.vector = xmm0;
            VectorLanes(c);
            return;
        }
        return;
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
        else {
            c.vector = xmm0;
            VectorLanes(c);
            return;
        }
        return;
    }
    if (op == O::VecAvg && (n == 8 || n == 16)) {
        if (n == 8)
            pavgb(dst, xmm1);
        else
            pavgw(dst, xmm1);
        return;
    }
    if ((op == O::VecMul || op == O::Vec4Mul) && n == 16) {
        pmullw(dst, xmm1);
        return;
    }
    if ((op == O::VecMul || op == O::Vec4Mul) && n == 32 && options.sse41) {
        pmulld(dst, xmm1);
        return;
    }
    if (op == O::VecMin || op == O::VecMax) {
        const bool sign = c.Arg(3).Payload(), maximum = op == O::VecMax;
        if (n == 8 && !sign) {
            if (maximum)
                pmaxub(dst, xmm1);
            else
                pminub(dst, xmm1);
            return;
        }
        if (n == 16 && sign) {
            if (maximum)
                pmaxsw(dst, xmm1);
            else
                pminsw(dst, xmm1);
            return;
        }
        if (options.sse41) {
            if (n == 8 && sign) {
                if (maximum)
                    pmaxsb(dst, xmm1);
                else
                    pminsb(dst, xmm1);
                return;
            }
            if (n == 16 && !sign) {
                if (maximum)
                    pmaxuw(dst, xmm1);
                else
                    pminuw(dst, xmm1);
                return;
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
                return;
            }
        }
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
        return;
    }
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
    Read(c.Arg(0), rax);
    if (options.avx)
        vmovq(xmm0, rax);
    else
        movq(xmm0, rax);
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
        unsigned at = 2 * i + odd;
        ExtractLane(at >= 128 / bits ? xmm15 : xmm14, at % (128 / bits), bits, rax);
        InsertLane(xmm0, i, bits, rax);
    }
}

void Writer::Handle(Op<O::VecExtractBytes>, OpContext& c) {
    unsigned n = c.Arg(2).Payload();
    if (n > 16) Reject(CompileError::UnsupportedOperand);
    ReadVector(c.Arg(0), xmm0);
    ReadVector(c.Arg(1), xmm1);
    psrldq(xmm0, n);
    pslldq(xmm1, 16 - n);
    por(xmm0, xmm1);
}

void Writer::Handle(Op<O::VecByteShift>, OpContext& c) {
    unsigned n = c.Arg(2).Payload();
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
    unsigned b = c.Arg(1).Payload();
    if (b == 8)
        pmovmskb(eax, xmm14);
    else if (b == 32)
        movmskps(eax, xmm14);
    else if (b == 64)
        movmskpd(eax, xmm14);
    else if (b == 16) {
        psraw(xmm14, 15);
        packsswb(xmm14, xmm14);
        pmovmskb(eax, xmm14);
        and_(eax, 255);
    } else
        Reject(CompileError::UnsupportedOperand);
}

void Writer::Handle(Op<O::VecExtend>, OpContext& c) {
    unsigned from = c.Arg(1).Payload(), to = c.Arg(2).Payload();
    if ((from != 8 && from != 16 && from != 32) || (to != 16 && to != 32 && to != 64) || from >= to)
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
    unsigned from = c.Arg(2).Payload(), to = from / 2;
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
    unsigned n = c.Arg(2).Payload();
    if (n != 8 && n != 16 && n != 32) Reject(CompileError::UnsupportedOperand);
    ReadVector(c.Arg(0), xmm14);
    ReadVector(c.Arg(1), xmm15);
    pxor(xmm0, xmm0);
    for (unsigned i = 0; i < 64 / n; ++i) {
        ExtractLane(xmm14, i * 2, n, rax, c.Arg(3).Payload());
        ExtractLane(xmm15, i * 2, n, rdx, c.Arg(3).Payload());
        imul(rax, rdx);
        InsertLane(xmm0, i, n * 2, rax);
    }
}

void Writer::Handle(Op<O::VecSignedMulWidenLow>, OpContext& c) {
    unsigned n = c.Arg(2).Payload();
    if (n != 8 && n != 16 && n != 32) Reject(CompileError::UnsupportedOperand);
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

namespace {
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

void Writer::Handle(Op<O::VecTableLookup8>, OpContext& c) {
    if (options.ssse3) {
        ReadVector(c.Arg(0), xmm0);
        ReadVector(c.Arg(1), xmm1);
        pshufb(xmm0, xmm1);
    } else
        Lookup(*this, c, true);
}

void Writer::Handle(Op<O::VecShuffle32Indexed>, OpContext& c) { Lookup(*this, c, false); }

namespace {
void VShift(Writer& w, OpContext& c, bool left, bool arithmetic, bool imm) {
    unsigned n = c.Arg(2).Payload();
    if (n != 8 && n != 16 && n != 32 && n != 64) w.Reject(CompileError::UnsupportedOperand);
    if (n == 8 && imm && !arithmetic) {
        const auto& home = w.allocation.homes[c.node.value];
        const Xmm dst = home.spilled ? w.xmm0 : Xmm(home.reg);
        c.vector = dst;
        const u64 raw = c.Arg(1).Payload();
        if (raw >= 8) {
            w.pxor(dst, dst);
            return;
        }
        w.ReadVector(c.Arg(0), dst);
        if (raw == 0) return;
        const unsigned count = static_cast<unsigned>(raw);
        const u64 byte_mask = left ? ((0xffu << count) & 0xffu) : (0xffu >> count);
        if (left)
            w.psllw(dst, count);
        else
            w.psrlw(dst, count);
        w.mov(w.rax, u64{0x0101010101010101ULL} * byte_mask);
        w.movq(w.xmm1, w.rax);
        w.punpcklqdq(w.xmm1, w.xmm1);
        w.pand(dst, w.xmm1);
        return;
    }
    if (n != 8 && (!arithmetic || n != 64) && (imm || w.View(c.Arg(1)) != ir::Type::V128)) {
        const auto& home = w.allocation.homes[c.node.value];
        const Xmm dst = home.spilled ? w.xmm0 : Xmm(home.reg);
        c.vector = dst;
        if (!imm) {
            w.Read(c.Arg(1), w.rax);
            w.movq(w.xmm1, w.rax);
        }
        w.ReadVector(c.Arg(0), dst);
        const unsigned count = imm ? std::min<u64>(c.Arg(1).Payload(), 255) : 0;
        if (imm) {
            if (left) {
                if (n == 16)
                    w.psllw(dst, count);
                else if (n == 32)
                    w.pslld(dst, count);
                else
                    w.psllq(dst, count);
            } else if (arithmetic) {
                if (n == 16)
                    w.psraw(dst, count);
                else
                    w.psrad(dst, count);
            } else {
                if (n == 16)
                    w.psrlw(dst, count);
                else if (n == 32)
                    w.psrld(dst, count);
                else
                    w.psrlq(dst, count);
            }
        } else {
            if (left) {
                if (n == 16)
                    w.psllw(dst, w.xmm1);
                else if (n == 32)
                    w.pslld(dst, w.xmm1);
                else
                    w.psllq(dst, w.xmm1);
            } else if (arithmetic) {
                if (n == 16)
                    w.psraw(dst, w.xmm1);
                else
                    w.psrad(dst, w.xmm1);
            } else {
                if (n == 16)
                    w.psrlw(dst, w.xmm1);
                else if (n == 32)
                    w.psrld(dst, w.xmm1);
                else
                    w.psrlq(dst, w.xmm1);
            }
        }
        return;
    }
    w.ReadVector(c.Arg(0), w.xmm14);
    if (!imm) {
        if (w.View(c.Arg(1)) == ir::Type::V128)
            w.ReadVector(c.Arg(1), w.xmm15);
        else
            w.Read(c.Arg(1), w.r11);
    }
    w.pxor(w.xmm0, w.xmm0);
    for (unsigned i = 0; i < 128 / n; ++i) {
        w.ExtractLane(w.xmm14, i, n, w.rax, arithmetic);
        if (imm)
            w.mov(w.rcx, c.Arg(1).Payload());
        else if (w.View(c.Arg(1)) == ir::Type::V128)
            w.ExtractLane(w.xmm15, i, n, w.rcx);
        else
            w.mov(w.rcx, w.r11);
        Label okay, done;
        w.cmp(w.rcx, n);
        w.jb(okay, w.T_NEAR);
        if (arithmetic)
            w.sar(w.rax, 63);
        else
            w.xor_(w.eax, w.eax);
        w.jmp(done, w.T_NEAR);
        w.L(okay);
        if (left)
            w.shl(w.rax, w.cl);
        else if (arithmetic)
            w.sar(w.rax, w.cl);
        else
            w.shr(w.rax, w.cl);
        w.L(done);
        w.InsertLane(w.xmm0, i, n, w.rax);
    }
}
}  // namespace

#define VS(name, left, arith, imm)                                                                 \
    void Writer::Handle(Op<O::name>, OpContext& c) { VShift(*this, c, left, arith, imm); }
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
