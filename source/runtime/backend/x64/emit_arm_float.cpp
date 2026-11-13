#include "emit.h"
#include "runtime/arch/arm64/numeric.h"
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-enum-enum-conversion"
#endif
#include "vixl/aarch64/constants-aarch64.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace swift::runtime::backend::x64::detail {

void Writer::CheckArmFPInput(
        const Xmm& input, unsigned bits, unsigned lanes, bool central, Label& slow) {
    for (unsigned lane = 0; lane < lanes; ++lane) {
        Label zero;
        movdqa(xmm15, input);
        if (lane) psrldq(xmm15, lane * bits / 8);
        movq(rax, xmm15);
        if (bits == 32) {
            add(eax, eax);
            jz(zero, T_NEAR);
            if (!central) {
                cmp(eax, 0x01000000);
                jb(slow, T_NEAR);
            } else {
                shr(eax, 24);
                sub(eax, 65);
                cmp(eax, 124);
                ja(slow, T_NEAR);
            }
        } else {
            add(rax, rax);
            jz(zero, T_NEAR);
            shr(rax, 53);
            if (!central) {
                test(eax, eax);
                jz(slow, T_NEAR);
            } else {
                sub(eax, 513);
                cmp(eax, 1020);
                ja(slow, T_NEAR);
            }
        }
        L(zero);
    }
}

bool Writer::TryArmNumeric(OpContext& c) {
    using namespace vixl::aarch64;
    using runtime::arm64::Numeric;
    using State = runtime::arm64::GuestState;
    if (!ArmNativeFP() || options.arm_fpcr == UINT32_MAX || allocation.fpcr_may_change ||
        c.count != 5 || c.Arg(1).Kind() != OperandKind::Immediate ||
        c.Arg(1).Payload() != reinterpret_cast<std::uintptr_t>(&runtime::arm64::ExecuteNumeric) ||
        c.Arg(2).Kind() != OperandKind::Value)
        return false;
    const auto pointer = NodeId{u32(c.Arg(2).Payload())};
    if (source.Operations()[pointer.value].opcode != OpCode::GetUniformAddress) return false;
    const auto base = source.Operands(pointer).front().Payload();
    if (base > INT32_MAX - sizeof(State) ||
        options.arm_fpcr_offset != base + offsetof(State, fpcr) ||
        options.arm_fpsr_offset != base + offsetof(State, fpsr))
        return false;
    const auto constant = [&](CellView cell, u64& value) {
        if (cell.Kind() == OperandKind::Immediate) {
            value = cell.Payload();
            return true;
        }
        if (cell.Kind() != OperandKind::Value ||
            source.Operations()[cell.Payload()].opcode != OpCode::LoadImm)
            return false;
        value = source.Operands(NodeId{u32(cell.Payload())}).front().Payload();
        return true;
    };
    u64 encoded;
    if (!constant(c.Arg(3), encoded)) return false;
    const auto family = static_cast<Numeric>((encoded >> 32) & 255);
    const u32 word = encoded, type = (word >> 22) & 3;
    unsigned bits = type == 1 ? 64 : 32, lanes = 1;
    const unsigned rd = word & 31, rn = (word >> 5) & 31, rm = (word >> 16) & 31;
    enum class Operation { Compare, Fused, Divide, Sqrt, Precision, FromInt, ToInt };
    Operation operation;
    unsigned control = 0, output_bits = bits, integer_bits = word >> 31 ? 64 : 32;
    bool indexed = false;
    if (family == Numeric::Scalar && type != 3) {
        if ((word & FPCompareFMask) == FPCompareFixed ||
            (word & FPConditionalCompareFMask) == FPConditionalCompareFixed)
            operation = Operation::Compare;
        else if ((word & FPDataProcessing3SourceFMask) == FPDataProcessing3SourceFixed) {
            operation = Operation::Fused;
            control = ((bool(word & (1u << 15)) != bool(word & (1u << 21))) ? 1u : 0u) |
                      ((word & (1u << 21)) ? 2u : 0u);
        } else if ((word & FPDataProcessing2SourceMask) == FDIV_s ||
                   (word & FPDataProcessing2SourceMask) == FDIV_d)
            operation = Operation::Divide;
        else if ((word & FPDataProcessing1SourceMask) == FSQRT_s ||
                 (word & FPDataProcessing1SourceMask) == FSQRT_d)
            operation = Operation::Sqrt;
        else if ((word & FPDataProcessing1SourceMask) == FCVT_ds ||
                 (word & FPDataProcessing1SourceMask) == FCVT_sd) {
            operation = Operation::Precision;
            output_bits = bits == 32 ? 64 : 32;
        } else if ((word & FPIntegerConvertFMask) == FPIntegerConvertFixed &&
                   (((word >> 16) & 31) == 2 || ((word >> 16) & 31) == 3))
            operation = Operation::FromInt;
        else if ((word & FPIntegerConvertFMask) == FPIntegerConvertFixed &&
                 (((word >> 16) & 31) == 24 || ((word >> 16) & 31) == 25))
            operation = Operation::ToInt;
        else
            return false;
    } else if (family == Numeric::Fused &&
               ((word & NEON3SameFPMask) == NEON_FMLA || (word & NEON3SameFPMask) == NEON_FMLS)) {
        operation = Operation::Fused;
        bits = word & (1u << 22) ? 64 : 32;
        lanes = (word & NEON_Q ? 128 : 64) / bits;
        control = (word & NEON3SameFPMask) == NEON_FMLS ? 1 : 0;
        output_bits = bits;
    } else if (family == Numeric::Indexed && ((word >> 22) & 3) >= 2) {
        const bool scalar = word & NEONScalar;
        const auto mask =
                scalar ? u32(NEONScalarByIndexedElementMask) : u32(NEONByIndexedElementMask);
        const auto flags = scalar ? (u32(NEON_Q) | u32(NEONScalar)) : 0;
        const auto op = word & mask;
        if (op != (u32(NEON_FMLA_H_byelement) | flags) &&
            op != (u32(NEON_FMLS_H_byelement) | flags))
            return false;
        operation = Operation::Fused;
        bits = ((word >> 22) & 3) == 2 ? 32 : 64;
        lanes = scalar ? 1 : (word & NEON_Q ? 128 : 64) / bits;
        control = op == (u32(NEON_FMLS_H_byelement) | flags) ? 1 : 0;
        output_bits = bits;
        indexed = true;
    } else
        return false;
    if (operation == Operation::Fused && !options.fma) return false;
    if (!runtime::arm64::SupportsNumeric(family, word)) return false;

    cold_numeric.push_back({c.node, {}, {}, LiveCallMasks()});
    auto& path = cold_numeric.back();
    auto& slow = path.entry;
    const bool flush = options.arm_fpcr & (1u << 24);
    const auto vector_offset = [&](unsigned reg) { return base + offsetof(State, q) + reg * 16; };
    const auto clear_inactive = [&](const Xmm& value) {
        if (lanes * bits == 32) {
            movd(eax, value);
            movd(value, eax);
        } else if (lanes * bits == 64)
            movq(value, value);
    };
    const auto read_gpr = [&](unsigned reg) {
        if (reg == 31)
            mov(eax, 0);
        else {
            const auto offset = base + reg * 8;
            const auto pin = PinnedSlot(offset, 8);
            if (pin >= 0)
                mov(rax, Reg64(PinnedRegister(pin)));
            else
                mov(rax, qword[r12 + offset]);
        }
    };
    const auto write_gpr = [&](unsigned reg) {
        if (reg == 31) return;
        const auto offset = base + reg * 8;
        const auto pin = PinnedSlot(offset, 8);
        if (pin >= 0)
            mov(Reg64(PinnedRegister(pin)), rax);
        else
            mov(qword[r12 + offset], rax);
    };
    if (operation == Operation::FromInt) {
        read_gpr(rn);
        const bool is_unsigned = word & (1u << 16);
        if (integer_bits == 32) {
            if (is_unsigned)
                mov(eax, eax);
            else
                movsxd(rax, eax);
        } else if (is_unsigned) {
            test(rax, rax);
            js(slow, T_NEAR);
        }
        pxor(xmm0, xmm0);
        if (bits == 32)
            cvtsi2ss(xmm0, rax);
        else
            cvtsi2sd(xmm0, rax);
    } else {
        movdqu(xmm0, ptr[r12 + vector_offset(rn)]);
        clear_inactive(xmm0);
        if (operation == Operation::Compare || operation == Operation::Divide ||
            operation == Operation::Fused) {
            if (operation == Operation::Compare && (word & FPCompareFMask) == FPCompareFixed &&
                (word & 8))
                pxor(xmm1, xmm1);
            else
                movdqu(xmm1, ptr[r12 + vector_offset(rm)]);
            if (indexed) {
                const auto index = bits == 32 ? (((word >> 11) & 1) << 1) | ((word >> 21) & 1)
                                              : (word >> 11) & 1;
                pshufd(xmm1, xmm1, bits == 32 ? index * 0x55 : index ? 0xee : 0x44);
            }
            clear_inactive(xmm1);
        }
        if (operation == Operation::Fused) {
            const auto addend = family == Numeric::Scalar ? (word >> 10) & 31 : rd;
            movdqu(xmm14, ptr[r12 + vector_offset(addend)]);
            clear_inactive(xmm14);
        }
        if (flush) {
            if (operation == Operation::Precision && bits == 64) {
                jmp(slow, T_NEAR);
            } else {
                const bool central = operation == Operation::Fused ||
                                     operation == Operation::Divide || operation == Operation::Sqrt;
                CheckArmFPInput(xmm0, bits, lanes, central, slow);
                if (operation == Operation::Compare || operation == Operation::Divide ||
                    operation == Operation::Fused)
                    CheckArmFPInput(xmm1, bits, lanes, central, slow);
                if (operation == Operation::Fused)
                    CheckArmFPInput(xmm14, bits, lanes, central, slow);
            }
        }
        if (operation == Operation::Compare) {
            if (bits == 32) {
                if (word & 16)
                    comiss(xmm0, xmm1);
                else
                    ucomiss(xmm0, xmm1);
            } else {
                if (word & 16)
                    comisd(xmm0, xmm1);
                else
                    ucomisd(xmm0, xmm1);
            }
            mov(eax, 2);
            mov(edx, 8);
            cmovb(eax, edx);
            mov(edx, 6);
            cmove(eax, edx);
            mov(edx, 3);
            cmovp(eax, edx);
        } else if (operation == Operation::ToInt) {
            const bool is_unsigned = word & (1u << 16);
            movq(rax, xmm0);
            if (bits == 32) mov(eax, eax);
            if (is_unsigned) {
                if (bits == 32)
                    test(eax, eax);
                else
                    test(rax, rax);
                js(slow, T_NEAR);
            }
            mov(rdx, bits == 32 ? u64{0x7fffffff} : UINT64_C(0x7fffffffffffffff));
            and_(rax, rdx);
            const auto limit = is_unsigned && integer_bits == 32 ? 32u : integer_bits - 1;
            mov(rdx, bits == 32 ? u64{127 + limit} << 23 : u64{1023 + limit} << 52);
            cmp(rax, rdx);
            jae(slow, T_NEAR);
            if (bits == 32)
                cvttss2si(rax, xmm0);
            else
                cvttsd2si(rax, xmm0);
            if (integer_bits == 32) mov(eax, eax);
            write_gpr(rd);
            mov(eax, 0);
        } else {
            if (operation == Operation::Fused) {
                // Negate only the active lanes before ARM NaN source selection.
                const auto negate = [&](const Xmm& value) {
                    mov(rax,
                        bits == 32 ? UINT64_C(0x8000000080000000) : UINT64_C(0x8000000000000000));
                    movq(xmm15, rax);
                    punpcklqdq(xmm15, xmm15);
                    pxor(value, xmm15);
                };
                if (control & 1) negate(xmm0);
                if (control & 2) negate(xmm14);
                if (bits == 32) {
                    if (lanes == 1)
                        vfmadd213ss(xmm0, xmm1, xmm14);
                    else
                        vfmadd213ps(xmm0, xmm1, xmm14);
                } else {
                    if (lanes == 1)
                        vfmadd213sd(xmm0, xmm1, xmm14);
                    else
                        vfmadd213pd(xmm0, xmm1, xmm14);
                }
            } else if (operation == Operation::Divide) {
                if (bits == 32)
                    divss(xmm0, xmm1);
                else
                    divsd(xmm0, xmm1);
            } else if (operation == Operation::Sqrt) {
                if (bits == 32)
                    sqrtss(xmm0, xmm0);
                else
                    sqrtsd(xmm0, xmm0);
            } else if (operation == Operation::Precision) {
                if (bits == 32)
                    cvtss2sd(xmm0, xmm0);
                else
                    cvtsd2ss(xmm0, xmm0);
            }
            if (lanes * output_bits == 32) {
                movd(eax, xmm0);
                movd(xmm0, eax);
            } else if (lanes * output_bits == 64)
                movq(xmm0, xmm0);
            CheckArmFPResult(c, output_bits, lanes, slow);
        }
    }
    if (operation != Operation::Compare && operation != Operation::ToInt) {
        movdqu(ptr[r12 + vector_offset(rd)], xmm0);
        mov(eax, 0);
    }
    mov(dword[r12 + base + offsetof(State, helper_status)], 0);
    L(path.resume);
    c.scalar_normalized = true;
    return true;
}
}  // namespace swift::runtime::backend::x64::detail
