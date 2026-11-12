#include "emit.h"

namespace swift::runtime::backend::x64::detail {
void Writer::Arithmetic(OpContext& c) {
    const auto destination = ScalarDestination(c);
    auto at = c.operands.begin();
    const bool negop = c.operation.opcode == OpCode::Neg;
    std::int32_t immediate;
    if (!negop && c.count == 2 &&
        (c.operation.opcode == OpCode::Add || c.operation.opcode == OpCode::Sub) &&
        Immediate32(c.Arg(1), c.width, immediate)) {
        Read(c.Arg(0), destination);
        if (c.operation.opcode == OpCode::Add)
            add(R(destination, c.width), immediate);
        else
            sub(R(destination, c.width), immediate);
        c.scalar_normalized = true;
        CaptureFlags(c);
        return;
    }
    if (negop)
        Read(*at, destination);
    else {
        const auto left = *at++;
        Expression(at, c.operands.end(), rcx, 1);
        Read(left, destination);
    }
    if (!negop && c.width < 8) Mask(rcx, c.width);
    const auto a = R(destination, c.width), b = R(rcx, c.width);
    switch (c.operation.opcode) {
        case OpCode::Add:
            add(a, b);
            break;
        case OpCode::Sub:
            sub(a, b);
            break;
        case OpCode::Adc:
            bt(rbx, 29);
            adc(a, b);
            break;
        case OpCode::Sbb:
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
    const auto destination = ScalarDestination(c);
    auto at = c.operands.begin();
    const auto left = *at++;
    const bool unary = at == c.operands.end();
    std::int32_t immediate;
    if (!unary && c.count == 2 &&
        (c.operation.opcode == OpCode::And || c.operation.opcode == OpCode::Or ||
         c.operation.opcode == OpCode::Xor) &&
        Immediate32(c.Arg(1), c.width, immediate)) {
        Read(left, destination);
        if (c.operation.opcode == OpCode::And)
            and_(R(destination, c.width), immediate);
        else if (c.operation.opcode == OpCode::Or)
            or_(R(destination, c.width), immediate);
        else
            xor_(R(destination, c.width), immediate);
        if (c.operation.flag_outputs ||
            allocation.homes[c.node.value].auxiliary_offset != UINT32_MAX)
            test(R(destination, c.width), R(destination, c.width));
        c.scalar_normalized = true;
        CaptureFlags(c);
        return;
    }
    if (!unary) Expression(at, c.operands.end(), rcx, 1);
    Read(left, destination);
    switch (c.operation.opcode) {
        case OpCode::And:
            and_(destination, rcx);
            break;
        case OpCode::Or:
            or_(destination, rcx);
            break;
        case OpCode::Xor:
            xor_(destination, rcx);
            break;
        case OpCode::AndNot:
            not_(rcx);
            and_(destination, rcx);
            break;
        case OpCode::Not:
            if (unary) {
                test(destination, destination);
                sete(destination.cvt8());
                movzx(destination.cvt32(), destination.cvt8());
            } else {
                mov(destination, rcx);
                not_(destination);
            }
            break;
        default:
            Reject(CompileError::UnsupportedOpcode);
    }
    Mask(destination, c.width);
    if (c.operation.flag_outputs || allocation.homes[c.node.value].auxiliary_offset != UINT32_MAX)
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
    Read(c.Arg(0), rax);
    auto at = c.operands.begin();
    ++at;
    Expression(at, c.operands.end(), rcx, 1);
    imul(rax, rcx);
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
    const auto destination = ScalarDestination(c);
    Read(c.Arg(0), destination);
    const auto op = c.operation.opcode;
    const bool immediate = c.Arg(1).Kind() == OperandKind::Immediate;
    if (!immediate) {
        Read(c.Arg(1), rcx);
        and_(ecx, c.width - 1);
    }
    if (op == OpCode::AsrImm || op == OpCode::AsrValue)
        Sign(destination, std::min(c.width, TypeBits(View(c.Arg(0)))));
    else
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
    mov(rdi, rax);
    xor_(eax, eax);
    mov(rsi, 1);
    Label loop, done;
    L(loop);
    test(rcx, rcx);
    jz(done, T_NEAR);
    mov(rdx, rcx);
    neg(rdx);
    and_(rdx, rcx);
    test(rdi, rsi);
    mov(r10, 0);
    cmovnz(r10, rdx);
    or_(rax, r10);
    xor_(rcx, rdx);
    shl(rsi, 1);
    jmp(loop, T_NEAR);
    L(done);
}

void Writer::Handle(Op<OpCode::ParallelBitExtract>, OpContext& c) {
    Read(c.Arg(0), rdi);
    Read(c.Arg(1), rcx);
    xor_(eax, eax);
    mov(rsi, 1);
    Label loop, done;
    L(loop);
    test(rcx, rcx);
    jz(done, T_NEAR);
    mov(rdx, rcx);
    neg(rdx);
    and_(rdx, rcx);
    test(rdi, rdx);
    mov(r10, 0);
    cmovnz(r10, rsi);
    or_(rax, r10);
    xor_(rcx, rdx);
    shl(rsi, 1);
    jmp(loop, T_NEAR);
    L(done);
}
}  // namespace swift::runtime::backend::x64::detail
