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
struct ArmNumericRequest {
    u64 base{};
    u64 encoded{};
    runtime::arm64::FPValueInfo info{};
    runtime::arm64::Numeric family{};
    u32 word{};
    unsigned rd{};
    unsigned rn{};
    unsigned rm{};
    bool typed{};
    bool flush{};

    unsigned IndexedLane() const noexcept {
        return info.bits == 32 ? (((word >> 11) & 1) << 1) | ((word >> 21) & 1)
                               : ((word >> 11) & 1);
    }
};

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

bool Writer::DecodeArmNumericRequest(OpContext& c, ArmNumericRequest& req) {
    using Operation = runtime::arm64::FPValueKind;
    using State = runtime::arm64::GuestState;
    req.typed = c.operation.opcode != OpCode::CallLambda;
    if (req.typed) {
        if (c.count < 3 || c.count > c.args.size() ||
            c.Arg(c.count - 1).Kind() != OperandKind::Immediate ||
            c.Arg(c.count - 2).Kind() != OperandKind::Immediate)
            Reject(CompileError::UnsupportedOperand);
        req.encoded = c.Arg(c.count - 1).Payload();
    } else {
        if (!ArmNativeFP() || options.arm_fpcr == UINT32_MAX || allocation.fpcr_may_change ||
            c.count != 5 || c.Arg(1).Kind() != OperandKind::Immediate ||
            c.Arg(1).Payload() !=
                    reinterpret_cast<std::uintptr_t>(&runtime::arm64::ExecuteNumeric) ||
            c.Arg(2).Kind() != OperandKind::Value)
            return false;
        const auto pointer = NodeId{u32(c.Arg(2).Payload())};
        if (source.Operations()[pointer.value].opcode != OpCode::GetUniformAddress) return false;
        req.base = source.Operands(pointer).front().Payload();
        if (req.base > INT32_MAX - sizeof(State) ||
            options.arm_fpcr_offset != req.base + offsetof(State, fpcr) ||
            options.arm_fpsr_offset != req.base + offsetof(State, fpsr))
            return false;
        auto instruction = c.Arg(3);
        if (instruction.Kind() == OperandKind::Value &&
            source.Operations()[instruction.Payload()].opcode == OpCode::LoadImm)
            instruction = source.Operands(NodeId{u32(instruction.Payload())}).front();
        if (instruction.Kind() != OperandKind::Immediate) return false;
        req.encoded = instruction.Payload();
    }
    req.info = runtime::arm64::DescribeFPValues(req.encoded);
    if (req.info.kind == Operation::Invalid) return false;
    req.family = static_cast<runtime::arm64::Numeric>((req.encoded >> 32) & 255);
    req.word = static_cast<u32>(req.encoded);
    req.rd = req.word & 31;
    req.rn = (req.word >> 5) & 31;
    req.rm = (req.word >> 16) & 31;
    req.flush = (options.arm_fpcr & (1u << 24)) != 0;
    return true;
}

void Writer::ValidateTypedArmNumeric(OpContext& c, const ArmNumericRequest& req) {
    using Operation = runtime::arm64::FPValueKind;
    const auto operation = req.info.kind;
    const unsigned inputs = operation == Operation::Fused     ? 3
                            : operation == Operation::Compare ? 2
                                                              : 1;
    const auto op = c.operation.opcode;
    const bool valid =
            op == OpCode::ArmFPTernary   ? operation == Operation::Fused
            : op == OpCode::ArmFPCompare ? operation == Operation::Compare
            : op == OpCode::ArmFPConvert
                    ? operation == Operation::FromInt || operation == Operation::ToInt
                    : op == OpCode::ArmFPUnary &&
                              (operation == Operation::Sqrt || operation == Operation::Precision);
    const auto offsets = c.Arg(c.count - 2).Payload();
    if (!valid || c.count != inputs + 2 || u32(offsets) > INT32_MAX - 4 ||
        (offsets >> 32) > INT32_MAX - 4 ||
        (operation == Operation::Compare ? c.operation.type != ir::Type::Void
         : operation == Operation::ToInt ? c.operation.type != ir::Type::U64
                                         : c.operation.type != ir::Type::V128))
        Reject(CompileError::UnsupportedOperand);
    const auto expected_input_type =
            operation == Operation::FromInt ? ir::Type::U64 : ir::Type::V128;
    for (unsigned i = 0; i < inputs; ++i)
        if (View(c.Arg(i)) != expected_input_type) Reject(CompileError::UnsupportedOperand);
    if (ArmNativeFP() &&
        (u32(offsets) != options.arm_fpcr_offset || (offsets >> 32) != options.arm_fpsr_offset))
        Reject(CompileError::UnsupportedOperand);
}

void Writer::EmitAvxFmaInsn(const Xmm& dst,
                            const Xmm& lhs,
                            const Xbyak::Operand& rhs,
                            const ArmNumericRequest& req) {
    const auto bits = req.info.bits, lanes = req.info.lanes, control = req.info.control;
    if (lanes == 1) {
        if (bits == 32) {
            if (control == 0)
                vfmadd231ss(dst, lhs, rhs);
            else if (control == 1)
                vfnmadd231ss(dst, lhs, rhs);
            else if (control == 2)
                vfmsub231ss(dst, lhs, rhs);
            else
                vfnmsub231ss(dst, lhs, rhs);
        } else {
            if (control == 0)
                vfmadd231sd(dst, lhs, rhs);
            else if (control == 1)
                vfnmadd231sd(dst, lhs, rhs);
            else if (control == 2)
                vfmsub231sd(dst, lhs, rhs);
            else
                vfnmsub231sd(dst, lhs, rhs);
        }
    } else if (bits == 32) {
        if (control == 0)
            vfmadd231ps(dst, lhs, rhs);
        else if (control == 1)
            vfnmadd231ps(dst, lhs, rhs);
        else if (control == 2)
            vfmsub231ps(dst, lhs, rhs);
        else
            vfnmsub231ps(dst, lhs, rhs);
    } else {
        if (control == 0)
            vfmadd231pd(dst, lhs, rhs);
        else if (control == 1)
            vfnmadd231pd(dst, lhs, rhs);
        else if (control == 2)
            vfmsub231pd(dst, lhs, rhs);
        else
            vfnmsub231pd(dst, lhs, rhs);
    }
}

bool Writer::EmitAvxArmFusedScalarRhs(CellView arg1,
                                      const Xmm& dst,
                                      const Xmm& lhs,
                                      const ArmNumericRequest& req) {
    const auto bits = req.info.bits;
    const unsigned index = req.info.indexed ? req.IndexedLane() : 0u;
    const bool v128_rhs = arg1.Kind() == OperandKind::Value &&
                          source.Operations()[arg1.Payload()].type == ir::Type::V128;
    if (v128_rhs) {
        const auto& rh = allocation.homes[arg1.Payload()];
        if (rh.spilled && rh.uniform_offset != UINT32_MAX) {
            if (bits == 32)
                EmitAvxFmaInsn(dst, lhs, dword[r12 + rh.uniform_offset + index * 4], req);
            else
                EmitAvxFmaInsn(dst, lhs, qword[r12 + rh.uniform_offset + index * 8], req);
            return true;
        }
        if (rh.spilled) {
            if (bits == 32)
                EmitAvxFmaInsn(dst, lhs, dword[rsp + rh.stack_offset + index * 4], req);
            else
                EmitAvxFmaInsn(dst, lhs, qword[rsp + rh.stack_offset + index * 8], req);
            return true;
        }
        if (index) {
            if (bits == 32)
                vpermilps(xmm1, Xmm(rh.reg), index);
            else
                vpermilpd(xmm1, Xmm(rh.reg), 3);
            EmitAvxFmaInsn(dst, lhs, xmm1, req);
            return false;
        }
        EmitAvxFmaInsn(dst, lhs, Xmm(rh.reg), req);
        return true;
    }
    ReadVector(arg1, xmm1);
    if (index) {
        if (bits == 32)
            vpermilps(xmm1, xmm1, index);
        else
            vpermilpd(xmm1, xmm1, 3);
    }
    EmitAvxFmaInsn(dst, lhs, xmm1, req);
    return false;
}

bool Writer::EmitAvxArmFusedVectorRhs(CellView arg1,
                                      const Xmm& dst,
                                      const Xmm& lhs,
                                      const ArmNumericRequest& req) {
    const auto bits = req.info.bits, lanes = req.info.lanes, control = req.info.control;
    const bool v128_rhs = arg1.Kind() == OperandKind::Value &&
                          source.Operations()[arg1.Payload()].type == ir::Type::V128;
    if (lanes == 2 && bits == 32) {
        if (req.info.indexed)
            LoadAvxBroadcast2S(arg1, req.IndexedLane(), xmm1);
        else
            LoadAvxVectorLow64(arg1, xmm1);
        EmitAvxFmaInsn(dst, lhs, xmm1, req);
        if (control != 0) vmovq(dst, dst);
        return false;
    }
    if (req.info.indexed) {
        const auto index = req.IndexedLane();
        bool preserved = false;
        if (v128_rhs) {
            const u32 rhs_id = static_cast<u32>(arg1.Payload());
            const std::uint8_t bkey =
                    static_cast<std::uint8_t>((bits == 64 ? 0x10u : 0x20u) | index);
            if (xmm1_bcast_node != rhs_id || xmm1_bcast_key != bkey) {
                EmitIndexedFPBroadcast(rhs_id, bits, index);
                xmm1_bcast_node = rhs_id;
                xmm1_bcast_key = bkey;
            }
            preserved = true;
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
        EmitAvxFmaInsn(dst, lhs, xmm1, req);
        return preserved;
    }
    if (v128_rhs) {
        const auto& rh = allocation.homes[arg1.Payload()];
        if (rh.spilled && rh.uniform_offset != UINT32_MAX)
            EmitAvxFmaInsn(dst, lhs, ptr[r12 + rh.uniform_offset], req);
        else if (rh.spilled)
            EmitAvxFmaInsn(dst, lhs, ptr[rsp + rh.stack_offset], req);
        else
            EmitAvxFmaInsn(dst, lhs, Xmm(rh.reg), req);
        return true;
    }
    ReadVector(arg1, xmm1);
    EmitAvxFmaInsn(dst, lhs, xmm1, req);
    return false;
}

bool Writer::TryEmitAvxArmFusedFP(OpContext& c,
                                  NumericColdPath& path,
                                  const ArmNumericRequest& req) {
    using Operation = runtime::arm64::FPValueKind;
    const auto bits = req.info.bits, lanes = req.info.lanes, control = req.info.control,
               output_bits = req.info.output_bits;
    if (!options.avx || !req.typed || req.info.kind != Operation::Fused || req.flush ||
        (bits != 32 && bits != 64) ||
        !(lanes * bits == 128 || lanes == 1 || (lanes == 2 && bits == 32)) || control > 3)
        return false;

    const auto& h = allocation.homes[c.node.value];
    const auto aliases = [&](unsigned reg) {
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
    if (lanes == 2 && bits == 32) {
        LoadAvxVectorLow64(c.Arg(0), xmm14);
    } else if (c.Arg(0).Kind() == OperandKind::Value &&
               source.Operations()[c.Arg(0).Payload()].type == ir::Type::V128 &&
               !allocation.homes[c.Arg(0).Payload()].spilled) {
        lhs = Xmm(allocation.homes[c.Arg(0).Payload()].reg);
    } else {
        ReadVector(c.Arg(0), xmm14);
    }

    if (lanes == 1 && bits == 32)
        LoadAvxVectorLow32(c.Arg(2), dst);
    else if (lanes * bits == 64)
        LoadAvxVectorLow64(c.Arg(2), dst);
    else
        ReadVector(c.Arg(2), dst);

    const bool preserved_bcast = lanes == 1 ? EmitAvxArmFusedScalarRhs(c.Arg(1), dst, lhs, req)
                                            : EmitAvxArmFusedVectorRhs(c.Arg(1), dst, lhs, req);

    CheckArmFPResult(c, output_bits, lanes, path.entry);
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

void Writer::LoadSseArmNumericInputs(OpContext& c, Label& slow, const ArmNumericRequest& req) {
    using namespace vixl::aarch64;
    using runtime::arm64::Numeric;
    using Operation = runtime::arm64::FPValueKind;
    using State = runtime::arm64::GuestState;
    const auto operation = req.info.kind;
    const auto bits = req.info.bits, lanes = req.info.lanes;
    const auto vector_offset = [&](unsigned reg) {
        return req.base + offsetof(State, q) + reg * 16;
    };
    const auto clear_inactive = [&](const Xmm& value) {
        if (lanes * bits == 32) {
            movd(eax, value);
            movd(value, eax);
        } else if (lanes * bits == 64) {
            movq(value, value);
        }
    };

    if (req.typed)
        ReadVector(c.Arg(0), xmm0);
    else
        movdqu(xmm0, ptr[r12 + vector_offset(req.rn)]);
    clear_inactive(xmm0);

    if (operation == Operation::Compare || operation == Operation::Divide ||
        operation == Operation::Fused) {
        if (operation == Operation::Compare && (req.word & FPCompareFMask) == FPCompareFixed &&
            (req.word & 8))
            pxor(xmm1, xmm1);
        else if (req.typed)
            ReadVector(c.Arg(1), xmm1);
        else
            movdqu(xmm1, ptr[r12 + vector_offset(req.rm)]);
        if (req.info.indexed) {
            const auto index = req.IndexedLane();
            pshufd(xmm1, xmm1, bits == 32 ? index * 0x55 : index ? 0xee : 0x44);
        }
        clear_inactive(xmm1);
    }
    if (operation == Operation::Fused) {
        const auto addend = req.family == Numeric::Scalar ? (req.word >> 10) & 31 : req.rd;
        if (req.typed)
            ReadVector(c.Arg(2), xmm14);
        else
            movdqu(xmm14, ptr[r12 + vector_offset(addend)]);
        clear_inactive(xmm14);
    }
    if (req.flush) {
        if (operation == Operation::Precision && bits == 64) {
            jmp(slow, T_NEAR);
        } else {
            const bool central = operation == Operation::Fused || operation == Operation::Divide ||
                                 operation == Operation::Sqrt;
            CheckArmFPInput(xmm0, bits, lanes, central, slow);
            if (operation == Operation::Compare || operation == Operation::Divide ||
                operation == Operation::Fused)
                CheckArmFPInput(xmm1, bits, lanes, central, slow);
            if (operation == Operation::Fused) CheckArmFPInput(xmm14, bits, lanes, central, slow);
        }
    }
}

void Writer::EmitSseArmNumericCompare(const ArmNumericRequest& req) {
    if (req.info.bits == 32) {
        if (req.word & 16)
            comiss(xmm0, xmm1);
        else
            ucomiss(xmm0, xmm1);
    } else {
        if (req.word & 16)
            comisd(xmm0, xmm1);
        else
            ucomisd(xmm0, xmm1);
    }
    if (req.typed && options.local_flags) {
        flags = {true, true, false, false, false};
        flags.floating = true;
        return;
    }
    mov(eax, 2);
    mov(edx, 8);
    cmovb(eax, edx);
    mov(edx, 6);
    cmove(eax, edx);
    mov(edx, 3);
    cmovp(eax, edx);
    if (req.typed) {
        shl(rax, 28);
        MergeFlags(rax, 15);
    }
}

void Writer::EmitSseArmNumericToInt(Label& slow, const ArmNumericRequest& req) {
    const auto bits = req.info.bits, integer_bits = req.info.integer_bits;
    const bool is_unsigned = req.word & (1u << 16);
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
    if (!req.typed && req.rd != 31) {
        const auto offset = req.base + req.rd * 8;
        const auto pin = PinnedSlot(offset, 8);
        if (pin >= 0)
            mov(Reg64(PinnedRegister(pin)), rax);
        else
            mov(qword[r12 + offset], rax);
    }
    if (!req.typed) mov(eax, 0);
}

void Writer::EmitSseArmNumericValueOp(OpContext& c, Label& slow, const ArmNumericRequest& req) {
    using Operation = runtime::arm64::FPValueKind;
    const auto operation = req.info.kind;
    const auto bits = req.info.bits, lanes = req.info.lanes, control = req.info.control,
               output_bits = req.info.output_bits;
    if (operation == Operation::Fused) {
        // Negate only the active lanes before ARM NaN source selection.
        const auto negate = [&](const Xmm& value) {
            mov(rax, bits == 32 ? UINT64_C(0x8000000080000000) : UINT64_C(0x8000000000000000));
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
    } else if (lanes * output_bits == 64) {
        movq(xmm0, xmm0);
    }
    CheckArmFPResult(c, output_bits, lanes, slow);
}

void Writer::EmitSseArmNumericBody(OpContext& c, Label& slow, const ArmNumericRequest& req) {
    using Operation = runtime::arm64::FPValueKind;
    LoadSseArmNumericInputs(c, slow, req);
    if (req.info.kind == Operation::Compare) {
        EmitSseArmNumericCompare(req);
        return;
    }
    if (req.info.kind == Operation::ToInt) {
        EmitSseArmNumericToInt(slow, req);
        return;
    }
    EmitSseArmNumericValueOp(c, slow, req);
}

void Writer::EmitSseArmNumericFastPath(OpContext& c,
                                       NumericColdPath& path,
                                       const ArmNumericRequest& req) {
    using Operation = runtime::arm64::FPValueKind;
    using State = runtime::arm64::GuestState;
    auto& slow = path.entry;
    if (req.info.kind == Operation::FromInt) {
        if (req.typed) {
            Read(c.Arg(0), rax);
        } else if (req.rn == 31) {
            mov(eax, 0);
        } else {
            const auto offset = req.base + req.rn * 8;
            const auto pin = PinnedSlot(offset, 8);
            if (pin >= 0)
                mov(rax, Reg64(PinnedRegister(pin)));
            else
                mov(rax, qword[r12 + offset]);
        }
        const bool is_unsigned = req.word & (1u << 16);
        if (req.info.integer_bits == 32) {
            if (is_unsigned)
                mov(eax, eax);
            else
                movsxd(rax, eax);
        } else if (is_unsigned) {
            test(rax, rax);
            js(slow, T_NEAR);
        }
        pxor(xmm0, xmm0);
        if (req.info.bits == 32)
            cvtsi2ss(xmm0, rax);
        else
            cvtsi2sd(xmm0, rax);
    } else {
        EmitSseArmNumericBody(c, slow, req);
    }
    if (!req.typed) {
        if (req.info.kind != Operation::Compare && req.info.kind != Operation::ToInt) {
            movdqu(ptr[r12 + req.base + offsetof(State, q) + req.rd * 16], xmm0);
            mov(eax, 0);
        }
        mov(dword[r12 + req.base + offsetof(State, helper_status)], 0);
    }
    L(path.resume);
    c.scalar_normalized = true;
}

bool Writer::TryArmNumeric(OpContext& c) {
    using Operation = runtime::arm64::FPValueKind;
    ArmNumericRequest req;
    if (!DecodeArmNumericRequest(c, req)) return false;
    if (req.typed) ValidateTypedArmNumeric(c, req);

    const bool native = ArmNativeFP() && options.arm_fpcr != UINT32_MAX &&
                        !allocation.fpcr_may_change &&
                        (req.info.kind != Operation::Fused || options.fma);
    if (!req.typed && !native) return false;

    cold_numeric.push_back({c.node, {}, {}, LiveCallMasks(), flags, guest_pc});
    auto& path = cold_numeric.back();
    if (!native) {
        jmp(path.entry, T_NEAR);
        L(path.resume);
        if (req.info.kind == Operation::Compare && options.local_flags) {
            flags = {true, true, false, false, false};
            flags.floating = true;
        }
        return true;
    }
    if (TryEmitAvxArmFusedFP(c, path, req)) return true;
    EmitSseArmNumericFastPath(c, path, req);
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
            const u64 exponent =
                    bits == 16 ? 0x7c00 : bits == 32 ? 0x7f800000 : 0x7ff0000000000000;
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
