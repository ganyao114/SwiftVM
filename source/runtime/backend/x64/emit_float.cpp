#include "emit.h"
#include "helpers.h"
#include "runtime/arch/arm64/numeric.h"
#include "runtime/arch/x86/floating.h"

namespace swift::runtime::backend::x64::detail {
using O = OpCode;

namespace {
struct FloatCompareEncoding {
    unsigned predicate{3};
    bool swap{};
};

constexpr FloatCompareEncoding EncodeFloatCompare(unsigned relations) noexcept {
    switch (relations) {
        case 1:
            return {1, false};
        case 2:
            return {0, false};
        case 3:
            return {2, false};
        case 4:
            return {1, true};
        case 5:
            return {4, false};
        case 6:
            return {2, true};
        case 7:
            return {7, false};
        case 8:
            return {3, false};
        case 9:
            return {6, true};
        case 10:
            return {0, false};
        case 11:
            return {5, true};
        case 12:
            return {6, false};
        case 13:
            return {4, false};
        case 14:
            return {5, false};
        default:
            return {3, false};
    }
}
}  // namespace

void Writer::HarvestArmFPStatus(bool saved, bool restore) {
    if (!ArmNativeFP()) return;
    SaveNativeFlags();
    flags.native = false;
    const auto slot = Linked() ? dword[r12 - 40] : dword[rsp + mxcsr_slot + 4 + stack_bias];
    if (!saved) stmxcsr(slot);
    mov(r10d, slot);
    test(r10d, 63);
    Label clean;
    jz(clean, T_NEAR);
    mov(r11d, r10d);
    shr(r10d, 1);
    and_(r10d, 0x1e);
    and_(r11d, 1);
    or_(r10d, r11d);
    or_(dword[r12 + options.arm_fpsr_offset], r10d);
    and_(slot, ~63u);
    if (restore) ldmxcsr(slot);
    L(clean);
}

void Writer::EmitIndexedFPBroadcast(u32 rhs_node, unsigned bits, unsigned index) {
    const auto& rh = allocation.homes[rhs_node];
    if (bits == 64 && rh.spilled && rh.uniform_offset != UINT32_MAX) {
        vmovddup(xmm1, qword[r12 + rh.uniform_offset + index * 8]);
    } else if (bits == 64 && rh.spilled) {
        vmovddup(xmm1, qword[rsp + rh.stack_offset + index * 8]);
    } else if (bits == 64 && !rh.spilled) {
        if (index == 0)
            vmovddup(xmm1, Xmm(rh.reg));
        else
            vpermilpd(xmm1, Xmm(rh.reg), 3);
    } else if (rh.spilled && rh.uniform_offset != UINT32_MAX) {
        vpermilps(xmm1, ptr[r12 + rh.uniform_offset], index * 0x55);
    } else if (rh.spilled) {
        vpermilps(xmm1, ptr[rsp + rh.stack_offset], index * 0x55);
    } else {
        vpermilps(xmm1, Xmm(rh.reg), index * 0x55);
    }
}

void Writer::LoadAvxBroadcast2S(CellView rhs, unsigned index, const Xmm& dst) {
    if (rhs.Kind() == OperandKind::Value &&
        source.Operations()[rhs.Payload()].type == ir::Type::V128) {
        const auto& rh = allocation.homes[rhs.Payload()];
        if (rh.spilled && rh.uniform_offset != UINT32_MAX)
            vpermilps(dst, ptr[r12 + rh.uniform_offset], index * 0x55);
        else if (rh.spilled)
            vpermilps(dst, ptr[rsp + rh.stack_offset], index * 0x55);
        else
            vpermilps(dst, Xmm(rh.reg), index * 0x55);
    } else {
        ReadVector(rhs, dst);
        vpermilps(dst, dst, index * 0x55);
    }
    vmovq(dst, dst);
}

void Writer::CheckArmFPResult(
        OpContext& c, unsigned bits, unsigned lanes, Label& slow, bool only_nan) {
    const bool keep_flags = options.local_flags && flags.dirty && (flags.native || flags.floating);
    if (options.avx) {
        if (only_nan) {
            if (bits == 32)
                vcmpps(xmm14, c.vector, c.vector, 3);
            else
                vcmppd(xmm14, c.vector, c.vector, 3);
        } else {
            const auto index = bits == 32 ? 0u : 2u;
            fp_constants_used[index] = fp_constants_used[index + 1] = true;
            if (xmm15_fp_const != static_cast<std::int8_t>(index)) {
                vmovdqu(xmm15, ptr[rip + fp_constants[index]]);
                xmm15_fp_const = static_cast<std::int8_t>(index);
            }
            vpand(xmm14, c.vector, xmm15);
            if (bits == 32)
                vcmpps(xmm14, xmm14, ptr[rip + fp_constants[index + 1]], 8);
            else
                vcmppd(xmm14, xmm14, ptr[rip + fp_constants[index + 1]], 8);
        }
        if (keep_flags) {
            vpmovmskb(ecx, xmm14);
            Label ready;
            jrcxz(ready);
            jmp(slow, T_NEAR);
            L(ready);
        } else if (lanes > 1) {
            vptest(xmm14, xmm14);
            jnz(slow, T_NEAR);
        } else {
            vpmovmskb(ecx, xmm14);
            test(ecx, ecx);
            jnz(slow, T_NEAR);
        }
        return;
    }
    if (options.sse41) {
        const auto index = bits == 32 ? 0u : 2u;
        fp_constants_used[index] = fp_constants_used[index + 1] = true;
        movdqa(xmm14, c.vector);
        movdqu(xmm15, ptr[rip + fp_constants[index]]);
        pand(xmm14, xmm15);
        movdqu(xmm15, ptr[rip + fp_constants[index + 1]]);
        if (bits == 32)
            pcmpeqd(xmm14, xmm15);
        else
            pcmpeqq(xmm14, xmm15);
        movdqa(xmm15, c.vector);
        if (bits == 32)
            cmpps(xmm15, c.vector, 3);
        else
            cmppd(xmm15, c.vector, 3);
        por(xmm14, xmm15);
        pmovmskb(ecx, xmm14);
        if (keep_flags) {
            Label ready;
            jrcxz(ready);
            jmp(slow, T_NEAR);
            L(ready);
        } else {
            test(ecx, ecx);
            jnz(slow, T_NEAR);
        }
        return;
    }
    for (unsigned lane = 0; lane < lanes; ++lane) {
        movdqa(xmm14, c.vector);
        if (lane) psrldq(xmm14, lane * bits / 8);
        movq(rax, xmm14);
        if (bits == 32) {
            and_(eax, 0x7fffffff);
            cmp(eax, 0x7f800000);
            ja(slow, T_NEAR);
            cmp(eax, 0x00800000);
        } else {
            mov(rdx, UINT64_C(0x7fffffffffffffff));
            and_(rax, rdx);
            mov(rdx, UINT64_C(0x7ff0000000000000));
            cmp(rax, rdx);
            ja(slow, T_NEAR);
            mov(rdx, UINT64_C(0x0010000000000000));
            cmp(rax, rdx);
        }
        je(slow, T_NEAR);
    }
}

void Writer::BeginArmFPRegion(u32 control, u32 status) {
    if (arm_fp_region) return;
    // Exact numerical leaves preserve FPCR. A shared stub reuses the
    // control condition already checked at its original guest call site.
    const auto tmp = allocation.call_offset + 224 + stack_bias;
    constexpr unsigned rounding[]{0, 2, 1, 3};
    stmxcsr(dword[rsp + tmp + 48]);
    mov(dword[rsp + tmp + 52], 0x1f80 | (rounding[(options.arm_fpcr >> 22) & 3] << 13));
    ldmxcsr(dword[rsp + tmp + 52]);
    arm_fp_region = true;
    arm_fp_control = control;
    arm_fp_status = status;
}

void Writer::EndArmFPRegion() {
    if (!arm_fp_region) return;
    SaveNativeFlags();
    flags.native = false;
    const auto tmp = allocation.call_offset + 224 + stack_bias;
    stmxcsr(dword[rsp + tmp + 52]);
    ldmxcsr(dword[rsp + tmp + 48]);
    mov(r11d, dword[rsp + tmp + 52]);
    shr(r11d, 1);
    and_(r11d, 16);
    or_(dword[r12 + arm_fp_status], r11d);
    arm_fp_region = false;
}

void Writer::PrepareArmFPRegion(OpContext& c) {
    if (!arm_fp_region) return;
    if (c.operation.opcode == O::ArmFPBinary) {
        if (c.Arg(2).Payload() != arm_fp_control || c.Arg(3).Payload() != arm_fp_status)
            EndArmFPRegion();
        return;
    }
    const auto effects = EffectsFor(c.operation.opcode, c.operation.Metadata());
    bool boundary = HasAnyEffect(effects,
                                 Effect::MayFault | Effect::Unknown | Effect::External |
                                         Effect::Control | Effect::ReadHostState |
                                         Effect::WriteHostState | Effect::WriteFlags);
    if (c.operation.opcode == O::LoadUniform || c.operation.opcode == O::StoreUniform) {
        const auto field = c.Arg(0);
        const auto bytes = (TypeBits(field.TypeView()) + 7) / 8;
        for (auto offset : {arm_fp_control, arm_fp_status})
            boundary |= field.Payload() < u64{offset} + 4 && offset < field.Payload() + bytes;
    } else if (HasAnyEffect(effects, Effect::ReadGuestState | Effect::WriteGuestState)) {
        boundary = true;
    }
    if (boundary) EndArmFPRegion();
}

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

void Writer::EmitFloatHelperLoop(
        OpContext& c, unsigned bits, unsigned flags, bool fused, bool scalar) {
    const unsigned tmp = allocation.call_offset + 224;
    movdqu(ptr[rsp + tmp], xmm0);
    movdqu(ptr[rsp + tmp + 16], xmm1);
    if (fused) {
        ReadVector(c.Arg(2), xmm14);
        movdqu(ptr[rsp + tmp + 32], xmm14);
    }
    if (scalar) {
        movdqu(ptr[rsp + tmp + 48], xmm0);
    } else {
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
}

void Writer::EmitFloatRound(OpContext& c, unsigned bits, bool scalar) {
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
}

void Writer::EmitFloatCompareMask(OpContext& c, unsigned bits, bool scalar) {
    const unsigned relations = c.Arg(3).Payload() & 15;
    const bool signaling = c.Arg(3).Payload() & 16;
    const bool quiet_ordered = FloatingCompareNeedsCall(relations);
    if (FloatingCompareNeedsCall(c.Arg(3).Payload())) {
        // SSE2 has no quiet ordered LT/LE predicate. Use the shared pure
        // numerical leaf only for those shapes instead of introducing IE.
        const auto tmp = allocation.call_offset + 224;
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
        return;
    }
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
    const auto [predicate, swap] = EncodeFloatCompare(relations);
    movdqa(xmm14, swap ? xmm1 : xmm0);
    compare(xmm14, swap ? xmm0 : xmm1, predicate);
    if (relations == 5 || relations == 10) {
        compare(xmm0, xmm1, relations == 5 ? 7 : 3);
        if (relations == 5)
            pand(xmm14, xmm0);
        else
            por(xmm14, xmm0);
    } else if (relations == 0) {
        pxor(xmm14, xmm14);
    } else if (relations == 15) {
        pcmpeqd(xmm14, xmm14);
    }
    movdqa(xmm0, xmm14);
    if (scalar) {
        if (bits == 32)
            movss(xmm15, xmm0);
        else
            movsd(xmm15, xmm0);
        movdqa(xmm0, xmm15);
    }
}

void Writer::EmitFloatUnary(OpContext& c, unsigned bits, bool scalar) {
    const auto kind = c.Arg(3).Payload();
    if (kind > 2) Reject(CompileError::UnsupportedOperand);
    if (bits == 32) {
        if (kind == 0) {
            if (scalar)
                sqrtss(xmm0, xmm1);
            else
                sqrtps(xmm0, xmm1);
            return;
        }
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
        return;
    }
    if (kind == 0) {
        if (scalar)
            sqrtsd(xmm0, xmm1);
        else
            sqrtpd(xmm0, xmm1);
        return;
    }
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

void Writer::EmitFloatMinMax(OpContext& c, unsigned bits, bool scalar) {
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
    if (options.mxcsr_offset == UINT32_MAX) return;
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

void Writer::EmitFloatArithmetic(OpCode op, unsigned bits, bool scalar) {
    const bool addop = op == O::VecFAdd || op == O::VecFAddScalar32 || op == O::VecFAddScalar64;
    const bool subop = op == O::VecFSub || op == O::VecFSubScalar32 || op == O::VecFSubScalar64;
    const bool mulop = op == O::VecFMul || op == O::VecFMulScalar32 || op == O::VecFMulScalar64;
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
        return;
    }
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
    if (fused || (round && !options.sse41))
        EmitFloatHelperLoop(c, bits, flags, fused, scalar);
    else if (round)
        EmitFloatRound(c, bits, scalar);
    else if (mask)
        EmitFloatCompareMask(c, bits, scalar);
    else if (unary)
        EmitFloatUnary(c, bits, scalar);
    else if (minmax)
        EmitFloatMinMax(c, bits, scalar);
    else
        EmitFloatArithmetic(op, bits, scalar);
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

void Writer::Handle(Op<O::ArmScalarFP>, OpContext&) { Reject(CompileError::UnsupportedOpcode); }

void Writer::Handle(Op<O::ArmScalarFMA>, OpContext&) { Reject(CompileError::UnsupportedOpcode); }

bool Writer::EmitAvxArmFPBinary128(
        OpContext& c, const Xmm& dst, u64 format, unsigned bits, unsigned kind) {
    Xmm lhs = xmm0;
    if (c.Arg(0).Kind() == OperandKind::Value &&
        source.Operations()[c.Arg(0).Payload()].type == ir::Type::V128 &&
        !allocation.homes[c.Arg(0).Payload()].spilled) {
        lhs = Xmm(allocation.homes[c.Arg(0).Payload()].reg);
    } else {
        ReadVector(c.Arg(0), xmm0);
    }
    const auto emit_packed = [&](const Xbyak::Operand& rhs) {
        if (bits == 32) {
            if (kind == 0)
                vaddps(dst, lhs, rhs);
            else if (kind == 1)
                vsubps(dst, lhs, rhs);
            else if (kind == 2)
                vmulps(dst, lhs, rhs);
            else
                vdivps(dst, lhs, rhs);
        } else {
            if (kind == 0)
                vaddpd(dst, lhs, rhs);
            else if (kind == 1)
                vsubpd(dst, lhs, rhs);
            else if (kind == 2)
                vmulpd(dst, lhs, rhs);
            else
                vdivpd(dst, lhs, rhs);
        }
    };
    const auto arg1 = c.Arg(1);
    const bool v128_rhs = arg1.Kind() == OperandKind::Value &&
                          source.Operations()[arg1.Payload()].type == ir::Type::V128;
    bool preserved_bcast = false;
    if (format & 64) {
        const unsigned index = format >> 7;
        if (v128_rhs) {
            const u32 rhs_id = static_cast<u32>(arg1.Payload());
            const std::uint8_t bkey =
                    static_cast<std::uint8_t>((bits == 64 ? 0x10u : 0x20u) | index);
            if (xmm1_bcast_node != rhs_id || xmm1_bcast_key != bkey) {
                EmitIndexedFPBroadcast(rhs_id, bits, index);
                xmm1_bcast_node = rhs_id;
                xmm1_bcast_key = bkey;
            }
            preserved_bcast = true;
        } else {
            ReadVector(arg1, xmm1);
            if (bits == 64) {
                if (index == 0)
                    vmovddup(xmm1, xmm1);
                else
                    vpermilpd(xmm1, xmm1, 3);
            } else {
                vpermilps(xmm1, xmm1, index * 0x55);
            }
        }
        emit_packed(xmm1);
        return preserved_bcast;
    }
    if (v128_rhs) {
        const auto& rh = allocation.homes[arg1.Payload()];
        if (rh.spilled && rh.uniform_offset != UINT32_MAX)
            emit_packed(ptr[r12 + rh.uniform_offset]);
        else if (rh.spilled)
            emit_packed(ptr[rsp + rh.stack_offset]);
        else
            emit_packed(Xmm(rh.reg));
        return true;
    }
    ReadVector(arg1, xmm1);
    emit_packed(xmm1);
    return false;
}

void Writer::EmitAvxArmFPBinary2S(OpContext& c, const Xmm& dst, u64 format, unsigned kind) {
    LoadAvxVectorLow64(c.Arg(0), xmm0);
    if (format & 64)
        LoadAvxBroadcast2S(c.Arg(1), format >> 7, xmm1);
    else
        LoadAvxVectorLow64(c.Arg(1), xmm1);
    if (kind == 0) {
        vaddps(dst, xmm0, xmm1);
    } else if (kind == 1) {
        vsubps(dst, xmm0, xmm1);
        vmovq(dst, dst);
    } else {
        vmulps(dst, xmm0, xmm1);
    }
}

bool Writer::EmitAvxArmFPBinaryScalar32(OpContext& c, const Xmm& dst, u64 format, unsigned kind) {
    LoadAvxVectorLow32(c.Arg(0), xmm0);
    const auto emit_scalar32 = [&](const Xbyak::Operand& rhs) {
        if (kind == 0)
            vaddss(dst, xmm0, rhs);
        else if (kind == 1)
            vsubss(dst, xmm0, rhs);
        else if (kind == 2)
            vmulss(dst, xmm0, rhs);
        else
            vdivss(dst, xmm0, rhs);
    };
    const auto arg1 = c.Arg(1);
    const unsigned lane_idx = (format & 64) ? (format >> 7) : 0u;
    if (arg1.Kind() == OperandKind::Value &&
        source.Operations()[arg1.Payload()].type == ir::Type::V128) {
        const auto& rh = allocation.homes[arg1.Payload()];
        if (rh.spilled && rh.uniform_offset != UINT32_MAX) {
            emit_scalar32(dword[r12 + rh.uniform_offset + lane_idx * 4]);
            return true;
        }
        if (rh.spilled) {
            emit_scalar32(dword[rsp + rh.stack_offset + lane_idx * 4]);
            return true;
        }
        if (lane_idx) {
            vpermilps(xmm1, Xmm(rh.reg), lane_idx);
            emit_scalar32(xmm1);
            return false;
        }
        emit_scalar32(Xmm(rh.reg));
        return true;
    }
    ReadVector(arg1, xmm1);
    if (lane_idx) vpermilps(xmm1, xmm1, lane_idx);
    emit_scalar32(xmm1);
    return false;
}

bool Writer::EmitAvxArmFPBinaryScalar64(OpContext& c, const Xmm& dst, u64 format, unsigned kind) {
    LoadAvxVectorLow64(c.Arg(0), xmm0);
    const auto emit_scalar64 = [&](const Xbyak::Operand& rhs) {
        if (kind == 0)
            vaddsd(dst, xmm0, rhs);
        else if (kind == 1)
            vsubsd(dst, xmm0, rhs);
        else if (kind == 2)
            vmulsd(dst, xmm0, rhs);
        else
            vdivsd(dst, xmm0, rhs);
    };
    const auto arg1 = c.Arg(1);
    const unsigned hi_lane = (format & 64) ? (format >> 7) : 0u;
    if (arg1.Kind() == OperandKind::Value &&
        source.Operations()[arg1.Payload()].type == ir::Type::V128) {
        const auto& rh = allocation.homes[arg1.Payload()];
        if (rh.spilled && rh.uniform_offset != UINT32_MAX) {
            emit_scalar64(qword[r12 + rh.uniform_offset + hi_lane * 8]);
            return true;
        }
        if (rh.spilled) {
            emit_scalar64(qword[rsp + rh.stack_offset + hi_lane * 8]);
            return true;
        }
        if (hi_lane) {
            vpshufd(xmm1, Xmm(rh.reg), 0xee);
            emit_scalar64(xmm1);
            return false;
        }
        emit_scalar64(Xmm(rh.reg));
        return true;
    }
    ReadVector(arg1, xmm1);
    if (hi_lane) vpshufd(xmm1, xmm1, 0xee);
    emit_scalar64(xmm1);
    return false;
}

bool Writer::TryEmitAvxArmFPBinary(OpContext& c,
                                   FPColdPath& path,
                                   u64 format,
                                   unsigned bits,
                                   unsigned lanes,
                                   unsigned kind) {
    if (!options.avx || !(lanes * bits == 128 || lanes == 1 || (lanes == 2 && bits == 32)))
        return false;
    const auto& h = allocation.homes[c.node.value];
    const auto aliases = [&](unsigned reg) {
        for (unsigned idx : {0u, 1u}) {
            const auto arg = c.Arg(idx);
            if (arg.Kind() == OperandKind::Value &&
                source.Operations()[arg.Payload()].type == ir::Type::V128 &&
                !allocation.homes[arg.Payload()].spilled &&
                allocation.homes[arg.Payload()].reg == reg)
                return true;
        }
        return false;
    };
    const Xmm dst = (!h.spilled && !h.terminal_only && !aliases(h.reg)) ? Xmm(h.reg) : xmm0;
    c.vector = dst;
    path.dest_reg = static_cast<std::uint8_t>(dst.getIdx());
    bool preserved_bcast = false;
    if (lanes * bits == 128)
        preserved_bcast = EmitAvxArmFPBinary128(c, dst, format, bits, kind);
    else if (lanes == 2 && bits == 32)
        EmitAvxArmFPBinary2S(c, dst, format, kind);
    else if (bits == 32)
        preserved_bcast = EmitAvxArmFPBinaryScalar32(c, dst, format, kind);
    else
        preserved_bcast = EmitAvxArmFPBinaryScalar64(c, dst, format, kind);

    CheckArmFPResult(c, bits, lanes, path.entry, kind == 0 || kind == 1);
    if (!preserved_bcast) xmm1_bcast_node = NodeId::invalid;
    if (xmm1_bcast_node != NodeId::invalid &&
        allocation.homes[xmm1_bcast_node].last_use <= h.definition_position)
        xmm1_bcast_node = NodeId::invalid;
    path.fp_const = xmm15_fp_const;
    path.bcast_node = xmm1_bcast_node;
    path.bcast_key = xmm1_bcast_key;
    kept_xmm15_fp_const = true;
    kept_xmm1_bcast = xmm1_bcast_node != NodeId::invalid;
    L(path.resume);
    return true;
}

void Writer::EmitSseArmFPBinary(OpContext& c,
                                FPColdPath& path,
                                u64 status,
                                u64 format,
                                unsigned bits,
                                unsigned lanes,
                                unsigned kind,
                                bool regional,
                                bool post_check) {
    const unsigned tmp = allocation.call_offset + 224;
    auto& slow = path.entry;
    ReadVector(c.Arg(0), xmm0);
    ReadVector(c.Arg(1), xmm1);
    if (format & 64) {
        const unsigned index = format >> 7;
        if (bits == 32)
            pshufd(xmm1, xmm1, index * 0x55);
        else
            pshufd(xmm1, xmm1, index ? 0xee : 0x44);
    }
    // Zero or central finite exponents only. For F32 e in [65,189],
    // F64 e in [513,1533]: even a product remains normal, and the smallest
    // nonzero sum/difference is normal. Add/sub/mul can only raise IXC here.
    // Division also admits zero divisors through native FPSR accumulation
    // and checks its result for NaN before committing the destination.
    if (!post_check) {
        for (const auto input : {xmm0, xmm1}) {
            for (unsigned lane = 0; lane < lanes; ++lane) {
                Label zero;
                if (lane) {
                    movdqa(xmm14, input);
                    psrldq(xmm14, lane * bits / 8);
                    movq(rax, xmm14);
                } else {
                    movq(rax, input);
                }
                if (bits == 32) {
                    add(eax, eax);
                    jz(zero, T_SHORT);
                    shr(eax, 24);
                    sub(eax, 65);
                    cmp(eax, 124);
                } else {
                    add(rax, rax);
                    jz(zero, T_SHORT);
                    shr(rax, 53);
                    sub(eax, 513);
                    cmp(eax, 1020);
                }
                ja(slow, T_NEAR);
                L(zero);
            }
        }
    }
    // Clear inactive inputs before packed execution: they must not create
    // exceptions. Scalar instructions also explicitly zero their upper result.
    if (lanes * bits <= 64) {
        movq(xmm0, xmm0);
        movq(xmm1, xmm1);
    }
    if (!regional && !ArmNativeFP()) {
        stmxcsr(dword[rsp + tmp + 48]);
        mov(dword[rsp + tmp + 52], 0x1f80);
        ldmxcsr(dword[rsp + tmp + 52]);
    }
    const bool scalar = lanes == 1;
    if (bits == 32) {
        if (kind == 0) {
            if (scalar)
                addss(xmm0, xmm1);
            else
                addps(xmm0, xmm1);
        } else if (kind == 1) {
            if (scalar)
                subss(xmm0, xmm1);
            else
                subps(xmm0, xmm1);
        } else if (kind == 2) {
            if (scalar)
                mulss(xmm0, xmm1);
            else
                mulps(xmm0, xmm1);
        } else {
            divss(xmm0, xmm1);
        }
    } else {
        if (kind == 0) {
            if (scalar)
                addsd(xmm0, xmm1);
            else
                addpd(xmm0, xmm1);
        } else if (kind == 1) {
            if (scalar)
                subsd(xmm0, xmm1);
            else
                subpd(xmm0, xmm1);
        } else if (kind == 2) {
            if (scalar)
                mulsd(xmm0, xmm1);
            else
                mulpd(xmm0, xmm1);
        } else {
            divsd(xmm0, xmm1);
        }
    }
    if (!regional && !ArmNativeFP()) {
        stmxcsr(dword[rsp + tmp + 52]);
        ldmxcsr(dword[rsp + tmp + 48]);
        mov(eax, dword[rsp + tmp + 52]);
        shr(eax, 1);
        and_(eax, 16);
        or_(dword[r12 + status], eax);
    }
    if (lanes * bits == 32) {
        movd(eax, xmm0);
        movd(xmm0, eax);
    } else if (!scalar && lanes * bits == 64) {
        movq(xmm0, xmm0);
    }
    if (post_check || kind == 3) CheckArmFPResult(c, bits, lanes, slow);
    L(path.resume);
}

void Writer::Handle(Op<O::ArmFPBinary>, OpContext& c) {
    const auto control = c.Arg(2).Payload(), status = c.Arg(3).Payload(),
               format = c.Arg(4).Payload();
    const bool post_check =
            ArmNativeFP() && options.arm_fpcr != UINT32_MAX && !(options.arm_fpcr & (1u << 24));
    if (options.local_flags && flags.dirty && (flags.native || flags.floating)) {
        SaveNativeFlags(true);
        flags.native = false;
    }
    const auto preserved_flags = flags;
    if (!c.vector_result || View(c.Arg(0)) != ir::Type::V128 || View(c.Arg(1)) != ir::Type::V128 ||
        control > INT32_MAX - 4 || status > INT32_MAX - 4 || format > UINT32_MAX ||
        !runtime::arm64::ValidArmFPBinaryMode(format))
        Reject(CompileError::UnsupportedOperand);
    if (ArmNativeFP() && (control != options.arm_fpcr_offset || status != options.arm_fpsr_offset))
        Reject(CompileError::UnsupportedOperand);
    const unsigned bits = format & 4 ? 64 : 32, lanes = (format >> 3) & 7, kind = format & 3;
    const bool regional =
            options.arm_fpcr != UINT32_MAX && !ArmNativeFP() && !allocation.fpcr_may_change;
    if (regional) BeginArmFPRegion(control, status);
    const auto masks = LiveCallMasks();
    const FPCallKey key{u32(control),
                        u32(status),
                        u32(format),
                        masks.scalars,
                        masks.vectors,
                        regional,
                        flags.dirty};
    auto [stub, inserted] = fp_stub_index.try_emplace(key, fp_stubs.size());
    if (inserted) fp_stubs.push_back({key, {}});
    cold_fp.push_back({c.node, {}, {}, stub->second, preserved_flags});
    auto& path = cold_fp.back();
    if (allocation.fpcr_may_change) {
        jmp(path.entry, T_NEAR);
        L(path.resume);
        return;
    }
    if (kind == 3 && !ArmNativeFP()) {
        jmp(path.entry, T_NEAR);
        L(path.resume);
        flags = preserved_flags;
        return;
    }
    if (options.arm_fpcr == UINT32_MAX) {
        cmp(dword[r12 + control], 0);
        jne(path.entry, T_NEAR);
    }
    if (post_check && TryEmitAvxArmFPBinary(c, path, format, bits, lanes, kind)) {
        flags = preserved_flags;
        return;
    }
    EmitSseArmFPBinary(c, path, status, format, bits, lanes, kind, regional, post_check);
    flags = preserved_flags;
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
    } else {
        Reject(CompileError::UnsupportedOperand);
    }
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
    const unsigned from = c.Arg(1).Payload(), to = c.Arg(2).Payload();
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
    const unsigned from = c.Arg(1).Payload(), to = c.Arg(2).Payload();
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
    } else {
        Reject(CompileError::UnsupportedOperand);
    }
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
