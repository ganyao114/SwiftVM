#include "emit.h"
#include "helpers.h"
#include "runtime/arch/arm64/numeric.h"
#include "runtime/arch/x86/floating.h"

namespace swift::runtime::backend::x64::detail {
using O = OpCode;

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

void Writer::CheckArmFPResult(OpContext&, unsigned bits, unsigned lanes, Label& slow) {
    if (options.sse41) {
        const auto index = bits == 32 ? 0u : 2u;
        fp_constants_used[index] = fp_constants_used[index + 1] = true;
        movdqa(xmm14, xmm0);
        movdqu(xmm15, ptr[rip + fp_constants[index]]);
        pand(xmm14, xmm15);
        movdqu(xmm15, ptr[rip + fp_constants[index + 1]]);
        if (bits == 32)
            pcmpeqd(xmm14, xmm15);
        else
            pcmpeqq(xmm14, xmm15);
        movdqa(xmm15, xmm0);
        if (bits == 32)
            cmpps(xmm15, xmm0, 3);
        else
            cmppd(xmm15, xmm0, 3);
        por(xmm14, xmm15);
        pmovmskb(ecx, xmm14);
        Label ready;
        jrcxz(ready);
        jmp(slow, T_NEAR);
        L(ready);
        return;
    }
    for (unsigned lane = 0; lane < lanes; ++lane) {
        movdqa(xmm14, xmm0);
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

void Writer::EmitFPConstants() {
    constexpr u64 values[]{UINT64_C(0x7fffffff7fffffff),
                           UINT64_C(0x0080000000800000),
                           UINT64_C(0x7fffffffffffffff),
                           UINT64_C(0x0010000000000000)};
    for (unsigned index = 0; index < fp_constants.size(); ++index) {
        if (!fp_constants_used[index]) continue;
        align(16);
        L(fp_constants[index]);
        dq(values[index]);
        dq(values[index]);
    }
}

void Writer::BeginArmFPRegion(u32 control, u32 status) {
    if (arm_fp_region) return;
    // Exact numerical leaves preserve FPCR. A shared stub reuses the
    // control condition already checked at its original guest call site.
    const auto tmp = allocation.call_offset + 208 + stack_bias;
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
    const auto tmp = allocation.call_offset + 208 + stack_bias;
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
    } else if (HasAnyEffect(effects, Effect::ReadGuestState | Effect::WriteGuestState))
        boundary = true;
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

void Writer::Handle(Op<O::ArmScalarFP>, OpContext&) { Reject(CompileError::UnsupportedOpcode); }

void Writer::Handle(Op<O::ArmScalarFMA>, OpContext&) { Reject(CompileError::UnsupportedOpcode); }

void Writer::Handle(Op<O::ArmFPBinary>, OpContext& c) {
    const auto control = c.Arg(2).Payload(), status = c.Arg(3).Payload(),
               format = c.Arg(4).Payload();
    const bool post_check =
            ArmNativeFP() && options.arm_fpcr != UINT32_MAX && !(options.arm_fpcr & (1u << 24));
    if (options.local_flags && flags.dirty && !(post_check && options.sse41)) {
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
    const unsigned tmp = allocation.call_offset + 208;
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
    auto& slow = path.entry;
    if (allocation.fpcr_may_change) {
        jmp(slow, T_NEAR);
        L(path.resume);
        return;
    }
    if (options.arm_fpcr == UINT32_MAX) {
        cmp(dword[r12 + control], 0);
        jne(slow, T_NEAR);
    }
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
    // nonzero sum/difference is normal. Only IXC can arise; cancellation and
    // signed zeros have identical round-to-nearest semantics. No speculative
    // result or exception needs rollback, including the MinNorm boundary.
    if (!post_check)
        for (const auto input : {xmm0, xmm1}) {
            for (unsigned lane = 0; lane < lanes; ++lane) {
                Label zero;
                if (lane) {
                    movdqa(xmm14, input);
                    psrldq(xmm14, lane * bits / 8);
                    movq(rax, xmm14);
                } else
                    movq(rax, input);
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
        } else {
            if (scalar)
                mulss(xmm0, xmm1);
            else
                mulps(xmm0, xmm1);
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
        } else {
            if (scalar)
                mulsd(xmm0, xmm1);
            else
                mulpd(xmm0, xmm1);
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
    } else if (!scalar && lanes * bits == 64)
        movq(xmm0, xmm0);
    if (post_check) CheckArmFPResult(c, bits, lanes, slow);
    L(path.resume);
    flags = preserved_flags;
}

void Writer::EmitColdPaths() {
    const auto tmp = allocation.call_offset + 208;
    for (auto& path : cold_fp) {
        current = path.node;
        L(path.entry);
        flags = path.flags;
        if (flags.dirty) SaveNativeFlags(true);
        auto input = source.Operands(path.node).begin();
        ReadVector(*input++, xmm0);
        ReadVector(*input, xmm1);
        movdqu(ptr[rsp + tmp], xmm0);
        movdqu(ptr[rsp + tmp + 16], xmm1);
        call(fp_stubs[path.stub].entry);
        if (path.flags.dirty) {
            flags = {true, false, true, false, true};
            if (path.flags.saved && !path.flags.saved_carry) {
                xor_(qword[rsp + allocation.flags_slot], options.lahf ? 0x100 : 1);
                flags.saved_carry = false;
            }
            if (path.flags.native) {
                EnsureNativeFlags();
                if (flags.carry != path.flags.carry) cmc();
            }
        }
        jmp(path.resume, T_NEAR);
    }
    for (auto& stub : fp_stubs) {
        align(16);
        L(stub.entry);
        sub(rsp, 8);
        stack_bias = 16;
        flags = {stub.key.flags_saved, false, stub.key.flags_saved, false, true};
        arm_fp_region = stub.key.region;
        arm_fp_control = stub.key.control;
        arm_fp_status = stub.key.status;
        PrepareCall({stub.key.scalars, stub.key.vectors});
        lea(rdi, ptr[rsp + tmp + stack_bias + 32]);
        lea(rsi, ptr[rsp + tmp + stack_bias]);
        lea(rdx, ptr[rsp + tmp + stack_bias + 16]);
        mov(ecx, stub.key.format);
        if (options.arm_fpcr == UINT32_MAX || allocation.fpcr_may_change)
            mov(r8d, dword[r12 + stub.key.control]);
        else
            mov(r8d, options.arm_fpcr);
        lea(r9, ptr[r12 + stub.key.status]);
        Call(reinterpret_cast<std::uintptr_t>(&runtime::arm64::EvaluateVectorBinaryFP),
             HostSymbol::ArmFPBinary);
        movdqu(xmm0, ptr[rsp + tmp + stack_bias + 32]);
        if (stub.key.flags_saved) ImportPackedFlags();
        if (stub.key.region) BeginArmFPRegion(stub.key.control, stub.key.status);
        add(rsp, 8);
        ret();
        stack_bias = 0;
    }
    arm_fp_region = false;
    for (auto& path : cold_numeric) {
        current = path.node;
        L(path.entry);
        flags = {};
        OpContext context{source.Operations()[current.value], current, source.Operands(current)};
        for (const auto input : context.operands) context.args[context.count++] = input;
        context.width = TypeBits(context.operation.type);
        PrepareCall(path.masks);
        LambdaCall(context);
        jmp(path.resume, T_NEAR);
    }
    for (auto& path : cold_events) {
        L(path.entry);
        flags = path.flags;
        guest_pc = path.pc;
        mov(rax, path.pc);
        mov(qword[r12 + options.pc_offset], rax);
        Exit(runtime::ExitValue(runtime::NativeExit::GuestEvents));
    }
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
