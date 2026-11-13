#include "emit.h"

namespace swift::runtime::backend::x64::detail {
bool Writer::TryEmitZeroIdentityCopy(OpContext& c) {
    CellView other{};
    if (!IsZeroIdentityCopy(c, other)) return false;
    const auto destination = ScalarDestination(c);
    if (IsZeroScalarOperand(other, c.width)) {
        mov(destination.cvt32(), 0);
        c.scalar_normalized = true;
        return true;
    }
    const unsigned copy_bits = (c.width == 32 || TypeBits(View(other)) == 32) ? 32u : 64u;
    Xbyak::Address mem;
    Reg64 src = destination;
    if (MemoryScalarOperand(other, copy_bits, mem)) {
        mov(R(destination, copy_bits), mem);
    } else if (ResidentScalar(other, copy_bits, src)) {
        if (copy_bits == 32) {
            if (src != destination || source.Operations()[other.Payload()].type != ir::Type::U32)
                mov(destination.cvt32(), src.cvt32());
        } else if (src != destination) {
            mov(destination, src);
        }
    } else {
        Read(other, destination);
        if (copy_bits == 32 && TypeBits(View(other)) > 32)
            mov(destination.cvt32(), destination.cvt32());
    }
    c.scalar_normalized = true;
    return true;
}

void Writer::Arithmetic(OpContext& c) {
    if (allocation.homes[c.node.value].rematerialize) {
        c.saved = true;
        return;
    }
    if (TryEmitZeroIdentityCopy(c)) return;
    const auto destination = ScalarDestination(c);
    const bool can_lea = !c.operation.flag_outputs && (c.width == 32 || c.width == 64) &&
                         allocation.homes[c.node.value].auxiliary_offset == UINT32_MAX;
    const bool preserve_flags = options.local_flags && flags.native && can_lea;
    if (allocation.homes[c.node.value].reuse_rhs) {
        Xbyak::Address mem;
        if (!preserve_flags && MemoryScalarOperand(c.Arg(0), c.width, mem)) {
            Read(c.Arg(1), destination);
            add(R(destination, c.width), mem);
            c.scalar_normalized = true;
            CaptureFlags(c);
            return;
        }
        Reg64 left = rcx;
        if (!ResidentScalar(c.Arg(0), c.width, left)) Read(c.Arg(0), rcx);
        Read(c.Arg(1), destination);
        if (preserve_flags && c.operation.opcode == OpCode::Add) {
            lea(R(destination, c.width), ptr[destination + left]);
            c.scalar_normalized = true;
            return;
        }
        add(R(destination, c.width), R(left, c.width));
        c.scalar_normalized = true;
        CaptureFlags(c);
        return;
    }
    auto at = c.operands.begin();
    const bool negop = c.operation.opcode == OpCode::Neg;
    Reg64 right = rcx;
    std::int32_t immediate;
    if (!negop && c.count == 2 &&
        (c.operation.opcode == OpCode::Add || c.operation.opcode == OpCode::Sub) &&
        Immediate32(c.Arg(1), c.width, immediate)) {
        const auto displacement = c.operation.opcode == OpCode::Add ? std::int64_t{immediate}
                                                                    : -std::int64_t{immediate};
        if (can_lea && displacement >= INT32_MIN && displacement <= INT32_MAX) {
            Reg64 lhs = destination;
            const bool lhs_resident = ResidentScalar(c.Arg(0), c.width, lhs);
            if (preserve_flags || (lhs_resident && lhs != destination)) {
                if (!lhs_resident) {
                    Read(c.Arg(0), destination);
                    lhs = destination;
                }
                lea(R(destination, c.width), ptr[lhs + displacement]);
                c.scalar_normalized = true;
                return;
            }
        }
        Read(c.Arg(0), destination);
        if (c.operation.opcode == OpCode::Add)
            add(R(destination, c.width), immediate);
        else
            sub(R(destination, c.width), immediate);
        c.scalar_normalized = true;
        CaptureFlags(c);
        return;
    }
    if (can_lea && c.operation.opcode == OpCode::Add && c.count == 2) {
        Reg64 lhs = destination, rhs = rcx;
        if (ResidentScalar(c.Arg(0), c.width, lhs) && ResidentScalar(c.Arg(1), c.width, rhs) &&
            (preserve_flags || (lhs != destination && rhs != destination))) {
            lea(R(destination, c.width), ptr[lhs + rhs]);
            c.scalar_normalized = true;
            return;
        }
    }
    const auto load_carry_in = [&]() {
        if (options.local_flags && defer_flags) {
            if (options.lahf)
                bt(word[rsp + allocation.flags_slot], 8);
            else
                bt(qword[rsp + allocation.flags_slot], 0);
            if (flags.saved_carry) cmc();
        } else
            bt(rbx, 29);
    };
    if (!negop && !preserve_flags && c.count == 2 && (c.width == 32 || c.width == 64)) {
        Xbyak::Address mem;
        const bool rhs_mem = MemoryScalarOperand(c.Arg(1), c.width, mem);
        const bool lhs_mem = !rhs_mem &&
                             (c.operation.opcode == OpCode::Add ||
                              c.operation.opcode == OpCode::Adc) &&
                             MemoryScalarOperand(c.Arg(0), c.width, mem);
        if (rhs_mem || lhs_mem) {
            Read(rhs_mem ? c.Arg(0) : c.Arg(1), destination);
            const auto a = R(destination, c.width);
            switch (c.operation.opcode) {
                case OpCode::Add:
                    add(a, mem);
                    break;
                case OpCode::Sub:
                    sub(a, mem);
                    break;
                case OpCode::Adc:
                    load_carry_in();
                    adc(a, mem);
                    break;
                case OpCode::Sbb:
                    load_carry_in();
                    sbb(a, mem);
                    break;
                default:
                    Reject(CompileError::UnsupportedOpcode);
            }
            c.scalar_normalized = true;
            CaptureFlags(c);
            return;
        }
    }
    if (negop)
        Read(*at, destination);
    else {
        const auto left = *at++;
        if (c.count != 2 || !ResidentScalar(c.Arg(1), c.width, right) ||
            (preserve_flags && right == destination))
            Expression(at, c.operands.end(), rcx, 1);
        Read(left, destination);
    }
    if (preserve_flags && c.operation.opcode == OpCode::Add) {
        lea(R(destination, c.width), ptr[destination + right]);
        c.scalar_normalized = true;
        return;
    }
    if (!negop && c.width < 8) Mask(rcx, c.width);
    const auto a = R(destination, c.width), b = R(right, c.width);
    switch (c.operation.opcode) {
        case OpCode::Add:
            add(a, b);
            break;
        case OpCode::Sub:
            sub(a, b);
            break;
        case OpCode::Adc:
            if (options.local_flags && defer_flags) {
                if (options.lahf)
                    bt(word[rsp + allocation.flags_slot], 8);
                else
                    bt(qword[rsp + allocation.flags_slot], 0);
                if (flags.saved_carry) cmc();
            } else
                bt(rbx, 29);
            adc(a, b);
            break;
        case OpCode::Sbb:
            if (options.local_flags && defer_flags) {
                if (options.lahf)
                    bt(word[rsp + allocation.flags_slot], 8);
                else
                    bt(qword[rsp + allocation.flags_slot], 0);
                if (flags.saved_carry) cmc();
            } else
                bt(rbx, 29);
            sbb(a, b);
            break;
        case OpCode::Neg:
            neg(a);
            break;
        default:
            Reject(CompileError::UnsupportedOpcode);
    }
    c.scalar_normalized = c.width >= 32;
    CaptureFlags(c);
}

#define AR(name)                                                                                   \
    void Writer::Handle(Op<OpCode::name>, OpContext& c) { Arithmetic(c); }
// clang-format off
AR(Add)
AR(Sub)
AR(Adc)
AR(Sbb)
AR(Neg)
// clang-format on
#undef AR

void Writer::Logical(OpContext& c) {
    if (allocation.homes[c.node.value].rematerialize) {
        c.saved = true;
        return;
    }
    if (TryEmitZeroIdentityCopy(c)) return;
    const auto destination = ScalarDestination(c);
    auto at = c.operands.begin();
    const auto left = *at++;
    const bool unary = at == c.operands.end();
    Reg64 right = rcx;
    std::int32_t immediate;
    if (!unary && c.count == 2 && c.operation.opcode == OpCode::And && c.width == 64 &&
        c.Arg(1).Kind() == OperandKind::Immediate) {
        u64 raw_imm = c.Arg(1).Payload();
        const auto ibits = TypeBits(View(c.Arg(1)));
        if (ibits && ibits < 64) raw_imm &= (u64{1} << ibits) - 1;
        if (raw_imm == UINT32_MAX && !c.operation.flag_outputs &&
            allocation.homes[c.node.value].auxiliary_offset == UINT32_MAX &&
            TypeBits(View(left)) >= 32) {
            Xbyak::Address mem;
            Reg64 src = destination;
            if (ResidentScalar(left, 32, src)) {
                if (src != destination ||
                    source.Operations()[left.Payload()].type != ir::Type::U32)
                    mov(destination.cvt32(), src.cvt32());
            } else if (MemoryScalarOperand(left, 32, mem)) {
                mov(destination.cvt32(), mem);
            } else {
                Read(left, destination);
                mov(destination.cvt32(), destination.cvt32());
            }
            c.scalar_normalized = true;
            return;
        }
        if (raw_imm <= UINT32_MAX) {
            Read(left, destination);
            and_(destination.cvt32(), static_cast<std::int32_t>(static_cast<u32>(raw_imm)));
            if (c.operation.flag_outputs ||
                allocation.homes[c.node.value].auxiliary_offset != UINT32_MAX)
                test(destination, destination);
            c.scalar_normalized = true;
            CaptureFlags(c);
            return;
        }
    }
    if (!unary && c.count == 2 &&
        (c.operation.opcode == OpCode::And || c.operation.opcode == OpCode::Or ||
         c.operation.opcode == OpCode::Xor) &&
        Immediate32(c.Arg(1), c.width, immediate)) {
        Read(left, destination);
        if (c.operation.opcode == OpCode::Or && immediate == 0 && !c.operation.flag_outputs &&
            allocation.homes[c.node.value].auxiliary_offset == UINT32_MAX) {
            c.scalar_normalized =
                    c.width == 64 || (c.width == 32 && TypeBits(View(left)) == 32);
            return;
        }
        if (c.operation.opcode == OpCode::And)
            and_(R(destination, c.width), immediate);
        else if (c.operation.opcode == OpCode::Or)
            or_(R(destination, c.width), immediate);
        else
            xor_(R(destination, c.width), immediate);
        c.scalar_normalized = true;
        CaptureFlags(c);
        return;
    }
    if (options.bmi1 && !unary && c.operation.opcode == OpCode::AndNot &&
        (c.width == 32 || c.width == 64)) {
        const auto r32e = [&](const Reg64& r) {
            return Xbyak::Reg32e(r.getIdx(), c.width);
        };
        Reg64 inv_reg = rcx;
        if (c.count != 2 || !ResidentScalar(c.Arg(1), c.width, inv_reg)) {
            Expression(at, c.operands.end(), rcx, 1);
            inv_reg = rcx;
        }
        Xbyak::Address mem;
        Reg64 src_reg = rax;
        if (MemoryScalarOperand(left, c.width, mem)) {
            andn(r32e(destination), r32e(inv_reg), mem);
        } else if (ResidentScalar(left, c.width, src_reg)) {
            andn(r32e(destination), r32e(inv_reg), r32e(src_reg));
        } else {
            const Reg64 tmp = inv_reg == destination ? rax : destination;
            Read(left, tmp);
            andn(r32e(destination), r32e(inv_reg), r32e(tmp));
        }
        if ((c.operation.flag_outputs & 16) ||
            allocation.homes[c.node.value].auxiliary_offset != UINT32_MAX)
            test(R(destination, c.width), R(destination, c.width));
        c.scalar_normalized = true;
        CaptureFlags(c);
        return;
    }
    if (!unary && c.count == 2 && (c.width == 32 || c.width == 64) &&
        (c.operation.opcode == OpCode::And || c.operation.opcode == OpCode::Or ||
         c.operation.opcode == OpCode::Xor)) {
        Xbyak::Address mem;
        const bool reuse_mem =
                allocation.homes[c.node.value].reuse_rhs && MemoryScalarOperand(left, c.width, mem);
        const bool rhs_mem = !reuse_mem && MemoryScalarOperand(c.Arg(1), c.width, mem);
        const bool lhs_mem = !reuse_mem && !rhs_mem && MemoryScalarOperand(left, c.width, mem);
        if (reuse_mem || rhs_mem || lhs_mem) {
            Read(rhs_mem ? left : c.Arg(1), destination);
            const auto a = R(destination, c.width);
            if (c.operation.opcode == OpCode::And)
                and_(a, mem);
            else if (c.operation.opcode == OpCode::Or)
                or_(a, mem);
            else
                xor_(a, mem);
            c.scalar_normalized = true;
            CaptureFlags(c);
            return;
        }
    }
    if (allocation.homes[c.node.value].reuse_rhs) {
        if (!ResidentScalar(c.Arg(0), c.width, right)) Read(c.Arg(0), rcx);
        Read(c.Arg(1), destination);
    } else {
        if (!unary && (c.count != 2 || !ResidentScalar(c.Arg(1), c.width, right)))
            Expression(at, c.operands.end(), rcx, 1);
        Read(left, destination);
    }
    const auto a = R(destination, c.width == 32 ? 32 : 64);
    const auto b = R(right, c.width == 32 ? 32 : 64);
    switch (c.operation.opcode) {
        case OpCode::And:
            and_(a, b);
            break;
        case OpCode::Or:
            or_(a, b);
            break;
        case OpCode::Xor:
            xor_(a, b);
            break;
        case OpCode::AndNot:
            if (right.getIdx() != rcx.getIdx()) mov(rcx, right);
            not_(rcx);
            and_(a, R(rcx, c.width == 32 ? 32 : 64));
            break;
        case OpCode::Not:
            if (unary) {
                test(destination, destination);
                sete(destination.cvt8());
                movzx(destination.cvt32(), destination.cvt8());
            } else {
                mov(destination, right);
                not_(a);
            }
            break;
        default:
            Reject(CompileError::UnsupportedOpcode);
    }
    if (c.width != 32) Mask(destination, c.width);
    if ((c.width < 32 || c.operation.opcode == OpCode::Not) &&
        (c.operation.flag_outputs || allocation.homes[c.node.value].auxiliary_offset != UINT32_MAX))
        test(R(destination, c.width), R(destination, c.width));
    c.scalar_normalized = true;
    CaptureFlags(c);
}

#define LOG(name)                                                                                  \
    void Writer::Handle(Op<OpCode::name>, OpContext& c) { Logical(c); }
// clang-format off
LOG(And)
LOG(Or)
LOG(Xor)
LOG(AndNot)
LOG(Not)
// clang-format on
#undef LOG

void Writer::Handle(Op<OpCode::Mul>, OpContext& c) {
    const auto destination = ScalarDestination(c);
    std::int32_t immediate;
    if (c.count == 2 && Immediate32(c.Arg(1), c.width, immediate)) {
        Xbyak::Address mem;
        if (MemoryScalarOperand(c.Arg(0), c.width, mem)) {
            if (c.width == 32)
                imul(destination.cvt32(), mem, immediate);
            else
                imul(destination, mem, immediate);
            c.scalar_normalized = true;
            return;
        }
        Reg64 source = destination;
        if (!ResidentScalar(c.Arg(0), c.width, source)) Read(c.Arg(0), destination);
        if (c.width == 32)
            imul(destination.cvt32(), source.cvt32(), immediate);
        else
            imul(destination, source, immediate);
        c.scalar_normalized = true;
        return;
    }
    if (c.count == 2 && (c.width == 32 || c.width == 64)) {
        Xbyak::Address mem;
        const bool rhs_mem = MemoryScalarOperand(c.Arg(1), c.width, mem);
        const bool lhs_mem = !rhs_mem && MemoryScalarOperand(c.Arg(0), c.width, mem);
        if (rhs_mem || lhs_mem) {
            Read(rhs_mem ? c.Arg(0) : c.Arg(1), destination);
            imul(R(destination, c.width), mem);
            c.scalar_normalized = true;
            return;
        }
    }
    Reg64 right = rcx;
    auto at = c.operands.begin();
    ++at;
    if (c.count != 2 || !ResidentScalar(c.Arg(1), c.width, right))
        Expression(at, c.operands.end(), rcx, 1);
    Read(c.Arg(0), destination);
    imul(R(destination, c.width == 32 ? 32 : 64), R(right, c.width == 32 ? 32 : 64));
    c.scalar_normalized = c.width >= 32;
}

void Writer::Handle(Op<OpCode::MulSub>, OpContext& c) {
    Read(c.Arg(0), rax);
    Read(c.Arg(1), rcx);
    imul(rax, rcx);
    Read(c.Arg(2), rcx);
    sub(rcx, rax);
    mov(rax, rcx);
}

void Writer::Handle(Op<OpCode::MulHigh>, OpContext& c) {
    Read(c.Arg(0), rax, true);
    Read(c.Arg(1), rcx, true);
    if (c.Arg(2).Payload())
        imul(rcx);
    else
        mul(rcx);
    mov(rax, rdx);
}

void Writer::Handle(Op<OpCode::Div>, OpContext& c) {
    Read(c.Arg(0), rax);
    auto at = c.operands.begin();
    ++at;
    Expression(at, c.operands.end(), rcx, 1);
    Label ok;
    test(rcx, rcx);
    jnz(ok, T_NEAR);
    Exit(ExitValue(NativeExit::DivideFault));
    L(ok);
    xor_(edx, edx);
    div(rcx);
}

void Writer::Handle(Op<OpCode::SignedDiv64>, OpContext& c) {
    Read(c.Arg(0), rax);
    Read(c.Arg(1), rcx);
    Label nonzero, okay;
    test(rcx, rcx);
    jnz(nonzero, T_NEAR);
    Exit(ExitValue(NativeExit::DivideFault));
    L(nonzero);
    mov(rdx, u64{1} << 63);
    cmp(rax, rdx);
    jne(okay, T_NEAR);
    cmp(rcx, -1);
    jne(okay, T_NEAR);
    Exit(ExitValue(NativeExit::DivideFault));
    L(okay);
    cqo();
    idiv(rcx);
}

void Writer::Shift(OpContext& c) {
    if (allocation.homes[c.node.value].rematerialize) {
        c.saved = true;
        return;
    }
    const auto destination = ScalarDestination(c);
    const auto op = c.operation.opcode;
    if (options.bmi2 && op == OpCode::RorImm && (c.width == 32 || c.width == 64) &&
        c.Arg(1).Kind() == OperandKind::Immediate && TypeBits(View(c.Arg(0))) >= c.width) {
        const auto r32e = [&](const Reg64& r) {
            return Xbyak::Reg32e(r.getIdx(), c.width);
        };
        const auto amount = static_cast<std::uint8_t>(c.Arg(1).Payload() & (c.width - 1));
        Xbyak::Address mem;
        Reg64 src = destination;
        if (MemoryScalarOperand(c.Arg(0), c.width, mem)) {
            rorx(r32e(destination), mem, amount);
        } else if (ResidentScalar(c.Arg(0), c.width, src)) {
            if (amount != 0 || src != destination ||
                (c.width == 32 &&
                 source.Operations()[c.Arg(0).Payload()].type != ir::Type::U32))
                rorx(r32e(destination), r32e(src), amount);
        } else {
            Read(c.Arg(0), destination);
            rorx(r32e(destination), r32e(destination), amount);
        }
        c.scalar_normalized = true;
        return;
    }
    Read(c.Arg(0), destination);
    if (options.local_flags && flags.native && op == OpCode::LslImm &&
        (c.width == 32 || c.width == 64) && !c.operation.flag_outputs &&
        (c.Arg(1).Payload() & (c.width - 1)) <= 3) {
        const auto amount = c.Arg(1).Payload() & (c.width - 1);
        if (amount) lea(R(destination, c.width), ptr[destination * (1u << amount)]);
        c.scalar_normalized = true;
        return;
    }
    const bool immediate = c.Arg(1).Kind() == OperandKind::Immediate;
    if (!immediate) {
        Read(c.Arg(1), rcx);
        and_(ecx, c.width - 1);
    }
    if (op == OpCode::AsrImm || op == OpCode::AsrValue)
        Sign(destination, std::min(c.width, TypeBits(View(c.Arg(0)))));
    else if (c.width < 32 || (c.width == 32 && TypeBits(View(c.Arg(0))) > 32))
        Mask(destination, c.width);
    const auto shift = [&](auto amount) {
        if (op == OpCode::LslImm || op == OpCode::LslValue)
            shl(R(destination, c.width), amount);
        else if (op == OpCode::LsrImm || op == OpCode::LsrValue)
            shr(R(destination, c.width), amount);
        else if (op == OpCode::AsrImm || op == OpCode::AsrValue)
            sar(R(destination, c.width), amount);
        else
            ror(R(destination, c.width), amount);
    };
    if (immediate) {
        const auto amount = c.Arg(1).Payload() & (c.width - 1);
        if (amount) shift(static_cast<int>(amount));
    } else
        shift(cl);
    c.scalar_normalized = c.width >= 32 && !(immediate && !(c.Arg(1).Payload() & (c.width - 1)) &&
                                             (op == OpCode::AsrImm || op == OpCode::AsrValue));
}

#define SH(name)                                                                                   \
    void Writer::Handle(Op<OpCode::name>, OpContext& c) { Shift(c); }
// clang-format off
SH(LslImm)
SH(LslValue)
SH(LsrImm)
SH(LsrValue)
SH(AsrImm)
SH(AsrValue)
SH(RorImm)
SH(RorValue)
// clang-format on
#undef SH

void Writer::Handle(Op<OpCode::TestBit>, OpContext& c) {
    Read(c.Arg(0), rax);
    auto bit = c.Arg(1).Payload();
    if (bit >= TypeBits(View(c.Arg(0))))
        xor_(eax, eax);
    else {
        shr(rax, bit);
        and_(eax, 1);
    }
}

void Writer::Handle(Op<OpCode::TestZero>, OpContext& c) {
    Read(c.Arg(0), rax);
    test(rax, rax);
    sete(al);
    movzx(eax, al);
}

void Writer::Handle(Op<OpCode::TestNotZero>, OpContext& c) {
    Read(c.Arg(0), rax);
    test(rax, rax);
    setne(al);
    movzx(eax, al);
}

void Writer::BitField(OpContext& c) {
    Read(c.Arg(0), rax, true);
    const bool insert = c.operation.opcode == OpCode::BitInsert;
    const unsigned l = c.Arg(insert ? 2 : 1).Payload(), n = c.Arg(insert ? 3 : 2).Payload();
    if (l >= 64 || n > 64 - l) Reject(CompileError::UnsupportedOperand);
    const u64 m = !n ? 0 : n == 64 ? ~u64{} : ((u64{1} << n) - 1) << l;
    if (c.operation.opcode == OpCode::BitExtract) {
        if (l) shr(rax, l);
        mov(rcx, n == 64 ? ~u64{} : n ? (u64{1} << n) - 1 : 0);
        and_(rax, rcx);
    } else {
        mov(rcx, ~m);
        and_(rax, rcx);
        if (insert && n) {
            Read(c.Arg(1), rdx, true);
            shl(rdx, l);
            mov(rcx, m);
            and_(rdx, rcx);
            or_(rax, rdx);
        }
    }
}

#define BF(name)                                                                                   \
    void Writer::Handle(Op<OpCode::name>, OpContext& c) { BitField(c); }
// clang-format off
BF(BitExtract)
BF(BitInsert)
BF(BitClear)
// clang-format on
#undef BF

void Writer::Handle(Op<OpCode::ByteSwap>, OpContext& c) {
    Read(c.Arg(0), rax, true);
    auto n = c.Arg(1).Payload();
    if (n == 16) {
        ror(ax, 8);
        movzx(eax, ax);
    } else if (n == 32)
        bswap(eax);
    else if (n == 64)
        bswap(rax);
    else
        Reject(CompileError::UnsupportedOperand);
}

void Writer::Count(OpContext& c) {
    Read(c.Arg(0), rax, true);
    const bool narrow = c.operation.opcode == OpCode::CountLeadingZeros32 ||
                        c.operation.opcode == OpCode::CountTrailingZeros32;
    const auto n = narrow ? 32 : 64;
    Mask(rax, n);
    Label zero, done;
    test(rax, rax);
    jz(zero, T_NEAR);
    if (c.operation.opcode == OpCode::CountLeadingZeros32 ||
        c.operation.opcode == OpCode::CountLeadingZeros64) {
        bsr(rax, rax);
        xor_(eax, n - 1);
    } else
        bsf(rax, rax);
    jmp(done, T_NEAR);
    L(zero);
    mov(eax, n);
    L(done);
}

#define CT(name)                                                                                   \
    void Writer::Handle(Op<OpCode::name>, OpContext& c) { Count(c); }
// clang-format off
CT(CountLeadingZeros32)
CT(CountLeadingZeros64)
CT(CountTrailingZeros32)
CT(CountTrailingZeros64)
// clang-format on
#undef CT

void Writer::Handle(Op<OpCode::PopCount>, OpContext& c) {
    Read(c.Arg(0), rax, true);
    mov(rcx, rax);
    shr(rcx, 1);
    mov(rdx, UINT64_C(0x5555555555555555));
    and_(rcx, rdx);
    sub(rax, rcx);
    mov(rcx, rax);
    shr(rcx, 2);
    mov(rdx, UINT64_C(0x3333333333333333));
    and_(rax, rdx);
    and_(rcx, rdx);
    add(rax, rcx);
    mov(rcx, rax);
    shr(rcx, 4);
    add(rax, rcx);
    mov(rdx, UINT64_C(0x0f0f0f0f0f0f0f0f));
    and_(rax, rdx);
    mov(rdx, UINT64_C(0x0101010101010101));
    imul(rax, rdx);
    shr(rax, 56);
}

void Writer::Handle(Op<OpCode::Crc32c>, OpContext& c) {
    Read(c.Arg(0), rax, true);
    Mask(rax, 32);
    Read(c.Arg(1), rcx, true);
    const unsigned n = c.Arg(2).Payload();
    if (n != 8 && n != 16 && n != 32 && n != 64) Reject(CompileError::UnsupportedOperand);
    if (options.sse42) {
        if (n == 64)
            crc32(rax, rcx);
        else
            crc32(eax, R(rcx, n));
    } else
        for (unsigned i = 0; i < n; ++i) {
            mov(edx, eax);
            xor_(edx, ecx);
            and_(edx, 1);
            neg(edx);
            and_(edx, 0x82f63b78);
            shr(eax, 1);
            xor_(eax, edx);
            shr(rcx, 1);
        }
}

void Writer::Handle(Op<OpCode::ParallelBitDeposit>, OpContext& c) {
    Read(c.Arg(0), rax);
    Read(c.Arg(1), rcx);
    movq(xmm14, rax);
    xor_(eax, eax);
    mov(r11, 1);
    Label loop, done;
    L(loop);
    test(rcx, rcx);
    jz(done, T_NEAR);
    mov(rdx, rcx);
    neg(rdx);
    and_(rdx, rcx);
    movq(r10, xmm14);
    test(r10, r11);
    mov(r10, 0);
    cmovnz(r10, rdx);
    or_(rax, r10);
    xor_(rcx, rdx);
    shl(r11, 1);
    jmp(loop, T_NEAR);
    L(done);
}

void Writer::Handle(Op<OpCode::ParallelBitExtract>, OpContext& c) {
    Read(c.Arg(0), rax);
    Read(c.Arg(1), rcx);
    movq(xmm14, rax);
    xor_(eax, eax);
    mov(r11, 1);
    Label loop, done;
    L(loop);
    test(rcx, rcx);
    jz(done, T_NEAR);
    mov(rdx, rcx);
    neg(rdx);
    and_(rdx, rcx);
    movq(r10, xmm14);
    test(r10, rdx);
    mov(r10, 0);
    cmovnz(r10, r11);
    or_(rax, r10);
    xor_(rcx, rdx);
    shl(r11, 1);
    jmp(loop, T_NEAR);
    L(done);
}
}  // namespace swift::runtime::backend::x64::detail
