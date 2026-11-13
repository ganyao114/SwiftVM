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
    using Operation = runtime::arm64::FPValueKind;
    using State = runtime::arm64::GuestState;
    const bool typed = c.operation.opcode != OpCode::CallLambda;
    u64 base{}, encoded{};
    if (typed) {
        if (c.count < 3 || c.count > c.args.size() ||
            c.Arg(c.count - 1).Kind() != OperandKind::Immediate ||
            c.Arg(c.count - 2).Kind() != OperandKind::Immediate)
            Reject(CompileError::UnsupportedOperand);
        encoded = c.Arg(c.count - 1).Payload();
    } else {
        if (!ArmNativeFP() || options.arm_fpcr == UINT32_MAX || allocation.fpcr_may_change ||
            c.count != 5 || c.Arg(1).Kind() != OperandKind::Immediate ||
            c.Arg(1).Payload() !=
                    reinterpret_cast<std::uintptr_t>(&runtime::arm64::ExecuteNumeric) ||
            c.Arg(2).Kind() != OperandKind::Value)
            return false;
        const auto pointer = NodeId{u32(c.Arg(2).Payload())};
        if (source.Operations()[pointer.value].opcode != OpCode::GetUniformAddress) return false;
        base = source.Operands(pointer).front().Payload();
        if (base > INT32_MAX - sizeof(State) ||
            options.arm_fpcr_offset != base + offsetof(State, fpcr) ||
            options.arm_fpsr_offset != base + offsetof(State, fpsr))
            return false;
        auto instruction = c.Arg(3);
        if (instruction.Kind() == OperandKind::Value &&
            source.Operations()[instruction.Payload()].opcode == OpCode::LoadImm)
            instruction = source.Operands(NodeId{u32(instruction.Payload())}).front();
        if (instruction.Kind() != OperandKind::Immediate) return false;
        encoded = instruction.Payload();
    }
    const auto info = runtime::arm64::DescribeFPValues(encoded);
    const auto operation = info.kind;
    if (operation == Operation::Invalid) return false;
    const auto family = static_cast<Numeric>((encoded >> 32) & 255);
    const u32 word = encoded, bits = info.bits, lanes = info.lanes, control = info.control,
              output_bits = info.output_bits, integer_bits = info.integer_bits;
    const unsigned rd = word & 31, rn = (word >> 5) & 31, rm = (word >> 16) & 31;
    const bool indexed = info.indexed;
    if (typed) {
        const unsigned inputs = operation == Operation::Fused     ? 3
                                : operation == Operation::Compare ? 2
                                                                  : 1;
        const auto op = c.operation.opcode;
        const bool valid =
                op == OpCode::ArmFPTernary   ? operation == Operation::Fused
                : op == OpCode::ArmFPCompare ? operation == Operation::Compare
                : op == OpCode::ArmFPConvert
                        ? operation == Operation::FromInt || operation == Operation::ToInt
                        : op == OpCode::ArmFPUnary && (operation == Operation::Sqrt ||
                                                       operation == Operation::Precision);
        const auto offsets = c.Arg(c.count - 2).Payload();
        if (!valid || c.count != inputs + 2 || u32(offsets) > INT32_MAX - 4 ||
            (offsets >> 32) > INT32_MAX - 4 ||
            (operation == Operation::Compare ? c.operation.type != ir::Type::Void
             : operation == Operation::ToInt ? c.operation.type != ir::Type::U64
                                             : c.operation.type != ir::Type::V128))
            Reject(CompileError::UnsupportedOperand);
        for (unsigned i = 0; i < inputs; ++i)
            if (View(c.Arg(i)) !=
                (operation == Operation::FromInt ? ir::Type::U64 : ir::Type::V128))
                Reject(CompileError::UnsupportedOperand);
        if (ArmNativeFP() &&
            (u32(offsets) != options.arm_fpcr_offset || (offsets >> 32) != options.arm_fpsr_offset))
            Reject(CompileError::UnsupportedOperand);
    }
    const bool native = ArmNativeFP() && options.arm_fpcr != UINT32_MAX &&
                        !allocation.fpcr_may_change &&
                        (operation != Operation::Fused || options.fma);
    if (!typed && !native) return false;
    cold_numeric.push_back({c.node, {}, {}, LiveCallMasks(), flags, guest_pc});
    auto& path = cold_numeric.back();
    auto& slow = path.entry;
    if (!native) {
        jmp(slow, T_NEAR);
        L(path.resume);
        if (operation == Operation::Compare && options.local_flags) {
            flags = {true, true, false, false, false};
            flags.floating = true;
        }
        return true;
    }
    const bool flush = options.arm_fpcr & (1u << 24);
    if (options.avx && typed && operation == Operation::Fused && !flush &&
        (lanes * bits == 128 || (lanes == 1 && bits == 64)) && control == 0) {
        const auto& h = allocation.homes[c.node.value];
        auto aliases = [&](unsigned reg) {
            for (unsigned idx : {0u, 1u, 2u}) {
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
        Xmm lhs = xmm14;
        if (c.Arg(0).Kind() == OperandKind::Value &&
            source.Operations()[c.Arg(0).Payload()].type == ir::Type::V128 &&
            !allocation.homes[c.Arg(0).Payload()].spilled) {
            lhs = Xmm(allocation.homes[c.Arg(0).Payload()].reg);
        } else {
            ReadVector(c.Arg(0), xmm14);
        }
        if (lanes == 1) {
            const auto arg2 = c.Arg(2);
            if (arg2.Kind() == OperandKind::Value &&
                source.Operations()[arg2.Payload()].type == ir::Type::V128) {
                const auto& h2 = allocation.homes[arg2.Payload()];
                if (h2.spilled && h2.uniform_offset != UINT32_MAX)
                    vmovq(dst, qword[r12 + h2.uniform_offset]);
                else if (h2.spilled)
                    vmovq(dst, qword[rsp + h2.stack_offset]);
                else
                    vmovq(dst, Xmm(h2.reg));
            } else {
                ReadVector(arg2, dst);
                vmovq(dst, dst);
            }
        } else {
            ReadVector(c.Arg(2), dst);
        }
        const auto arg1 = c.Arg(1);
        const bool v128_rhs = arg1.Kind() == OperandKind::Value &&
                              source.Operations()[arg1.Payload()].type == ir::Type::V128;
        bool preserved_bcast = false;
        if (lanes == 1) {
            const unsigned index = indexed ? ((word >> 11) & 1) : 0u;
            if (v128_rhs) {
                const auto& rh = allocation.homes[arg1.Payload()];
                if (rh.spilled && rh.uniform_offset != UINT32_MAX) {
                    vfmadd231sd(dst, lhs, qword[r12 + rh.uniform_offset + index * 8]);
                    preserved_bcast = true;
                } else if (rh.spilled) {
                    vfmadd231sd(dst, lhs, qword[rsp + rh.stack_offset + index * 8]);
                    preserved_bcast = true;
                } else if (index) {
                    vpermilpd(xmm1, Xmm(rh.reg), 3);
                    vfmadd231sd(dst, lhs, xmm1);
                } else {
                    vfmadd231sd(dst, lhs, Xmm(rh.reg));
                    preserved_bcast = true;
                }
            } else {
                ReadVector(arg1, xmm1);
                if (index) vpermilpd(xmm1, xmm1, 3);
                vfmadd231sd(dst, lhs, xmm1);
            }
        } else if (indexed) {
            const auto index =
                    bits == 32 ? (((word >> 11) & 1) << 1) | ((word >> 21) & 1) : (word >> 11) & 1;
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
            if (bits == 32)
                vfmadd231ps(dst, lhs, xmm1);
            else
                vfmadd231pd(dst, lhs, xmm1);
        } else if (v128_rhs) {
            const auto& rh = allocation.homes[arg1.Payload()];
            if (rh.spilled && rh.uniform_offset != UINT32_MAX) {
                if (bits == 32)
                    vfmadd231ps(dst, lhs, ptr[r12 + rh.uniform_offset]);
                else
                    vfmadd231pd(dst, lhs, ptr[r12 + rh.uniform_offset]);
            } else if (rh.spilled) {
                if (bits == 32)
                    vfmadd231ps(dst, lhs, ptr[rsp + rh.stack_offset]);
                else
                    vfmadd231pd(dst, lhs, ptr[rsp + rh.stack_offset]);
            } else {
                if (bits == 32)
                    vfmadd231ps(dst, lhs, Xmm(rh.reg));
                else
                    vfmadd231pd(dst, lhs, Xmm(rh.reg));
            }
            preserved_bcast = true;
        } else {
            ReadVector(arg1, xmm1);
            if (bits == 32)
                vfmadd231ps(dst, lhs, xmm1);
            else
                vfmadd231pd(dst, lhs, xmm1);
        }
        CheckArmFPResult(c, output_bits, lanes, slow);
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
        c.scalar_normalized = true;
        return true;
    }
    const auto vector_offset = [&](unsigned reg) { return base + offsetof(State, q) + reg * 16; };
    const auto clear_inactive = [&](const Xmm& value) {
        if (lanes * bits == 32) {
            movd(eax, value);
            movd(value, eax);
        } else if (lanes * bits == 64)
            movq(value, value);
    };
    const auto read_gpr = [&](unsigned reg) {
        if (typed)
            Read(c.Arg(0), rax);
        else if (reg == 31)
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
        if (typed || reg == 31) return;
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
        if (typed)
            ReadVector(c.Arg(0), xmm0);
        else
            movdqu(xmm0, ptr[r12 + vector_offset(rn)]);
        clear_inactive(xmm0);
        if (operation == Operation::Compare || operation == Operation::Divide ||
            operation == Operation::Fused) {
            if (operation == Operation::Compare && (word & FPCompareFMask) == FPCompareFixed &&
                (word & 8))
                pxor(xmm1, xmm1);
            else if (typed)
                ReadVector(c.Arg(1), xmm1);
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
            if (typed)
                ReadVector(c.Arg(2), xmm14);
            else
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
            if (typed && options.local_flags) {
                flags = {true, true, false, false, false};
                flags.floating = true;
            } else {
                mov(eax, 2);
                mov(edx, 8);
                cmovb(eax, edx);
                mov(edx, 6);
                cmove(eax, edx);
                mov(edx, 3);
                cmovp(eax, edx);
                if (typed) {
                    shl(rax, 28);
                    MergeFlags(rax, 15);
                }
            }
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
            if (!typed) mov(eax, 0);
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
    if (!typed) {
        if (operation != Operation::Compare && operation != Operation::ToInt) {
            movdqu(ptr[r12 + vector_offset(rd)], xmm0);
            mov(eax, 0);
        }
        mov(dword[r12 + base + offsetof(State, helper_status)], 0);
    }
    L(path.resume);
    c.scalar_normalized = true;
    return true;
}

void Writer::ArmFPValues(OpContext& c) {
    if (!TryArmNumeric(c)) Reject(CompileError::UnsupportedOperand);
}

void Writer::Handle(Op<OpCode::ArmFPTernary>, OpContext& c) { ArmFPValues(c); }

void Writer::Handle(Op<OpCode::ArmFPUnary>, OpContext& c) { ArmFPValues(c); }

void Writer::Handle(Op<OpCode::ArmFPConvert>, OpContext& c) { ArmFPValues(c); }

void Writer::Handle(Op<OpCode::ArmFPCompare>, OpContext& c) { ArmFPValues(c); }

void Writer::Handle(Op<OpCode::ArmFPBitUnary>, OpContext& c) {
    const auto control = c.Arg(1).Payload(), word = c.Arg(2).Payload();
    if (!c.vector_result || View(c.Arg(0)) != ir::Type::V128 || control > INT32_MAX - 4 ||
        word > UINT32_MAX || !runtime::arm64::IsFPBitUnary(word))
        Reject(CompileError::UnsupportedOperand);
    const auto type = (word >> 22) & 3;
    const auto bits = type == 3 ? 16u : type == 1 ? 64u : 32u;
    const auto op = (word & vixl::aarch64::FPDataProcessing1SourceMask) & ~(3u << 22);
    const u64 mask = UINT64_MAX >> (64 - bits), sign = u64{1} << (bits - 1);
    ReadVector(c.Arg(0), xmm0);
    movq(rax, xmm0);
    if (bits < 64) and_(eax, static_cast<u32>(mask));
    if (op != vixl::aarch64::FMOV_s) {
        const bool dynamic = options.arm_fpcr == UINT32_MAX || allocation.fpcr_may_change ||
                             control != options.arm_fpcr_offset;
        if (dynamic) mov(rdx, rax);
        mov(r10, op == vixl::aarch64::FABS_s ? mask & ~sign : sign);
        if (op == vixl::aarch64::FABS_s)
            and_(rax, r10);
        else
            xor_(rax, r10);
        if (dynamic) {
            Label done;
            const u64 exponent = bits == 16 ? 0x7c00 : bits == 32 ? 0x7f800000 : 0x7ff0000000000000;
            mov(r10, exponent);
            mov(r11, rdx);
            and_(r11, r10);
            cmp(r11, r10);
            jne(done, T_NEAR);
            mov(r10, mask & ~(sign | exponent));
            test(rdx, r10);
            jz(done, T_NEAR);
            test(dword[r12 + control], 2);
            jz(done, T_NEAR);
            mov(rax, rdx);
            L(done);
        }
    }
    movq(xmm0, rax);
}
}  // namespace swift::runtime::backend::x64::detail
