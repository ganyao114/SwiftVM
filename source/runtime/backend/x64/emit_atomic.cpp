#include "emit.h"
#include "helpers.h"

namespace swift::runtime::backend::x64::detail {
void Writer::Atomic(OpContext& c) {
    const bool rmw = c.operation.opcode == OpCode::AtomicRMW,
               compare = c.operation.opcode == OpCode::CompareAndSwap;
    const auto n = c.width;
    if (n != 8 && n != 16 && n != 32 && n != 64) Reject(CompileError::UnsupportedType);
    Read(c.Arg(rmw ? 1 : 0), rdi);
    Read(c.Arg(rmw ? 2 : 1), rsi);
    Mask(rsi, n);
    if (compare) {
        Read(c.Arg(2), rcx);
        mov(rax, rsi);
        lock();
        cmpxchg(ptr[rdi], R(rcx, n));
        return;
    }
    if (c.operation.opcode == OpCode::AtomicExchange) {
        mov(rax, rsi);
        xchg(ptr[rdi], R(rax, n));
        return;
    }
    if (c.operation.opcode == OpCode::AtomicFetchAdd) {
        mov(rax, rsi);
        lock();
        xadd(ptr[rdi], R(rax, n));
        return;
    }
    const auto kind = static_cast<AtomicRMWOp>(c.Arg(0).Payload());
    if (c.Arg(0).Payload() > 11) Reject(CompileError::UnsupportedOperand);
    Read(c.Arg(3), rdx);
    and_(edx, 1);
    Load(rax, rdi, 0, n);
    Label repeat;
    L(repeat);
    mov(rcx, rax);
    switch (kind) {
        case AtomicRMWOp::Add:
            add(rcx, rsi);
            break;
        case AtomicRMWOp::Sub:
            sub(rcx, rsi);
            break;
        case AtomicRMWOp::And:
            and_(rcx, rsi);
            break;
        case AtomicRMWOp::Or:
            or_(rcx, rsi);
            break;
        case AtomicRMWOp::Xor:
            xor_(rcx, rsi);
            break;
        case AtomicRMWOp::Neg:
            neg(rcx);
            break;
        case AtomicRMWOp::AddCarry:
            add(rcx, rsi);
            add(rcx, rdx);
            break;
        case AtomicRMWOp::SubBorrow:
            sub(rcx, rsi);
            sub(rcx, rdx);
            break;
        case AtomicRMWOp::SignedMax:
        case AtomicRMWOp::SignedMin:
            mov(r10, rsi);
            Sign(rcx, n);
            Sign(r10, n);
            cmp(rcx, r10);
            if (kind == AtomicRMWOp::SignedMax)
                cmovl(rcx, r10);
            else
                cmovg(rcx, r10);
            break;
        case AtomicRMWOp::UnsignedMax:
        case AtomicRMWOp::UnsignedMin:
            cmp(rcx, rsi);
            if (kind == AtomicRMWOp::UnsignedMax)
                cmovb(rcx, rsi);
            else
                cmova(rcx, rsi);
            break;
    }
    lock();
    cmpxchg(ptr[rdi], R(rcx, n));
    jne(repeat, T_NEAR);
}

#define A(name)                                                                                    \
    void Writer::Handle(Op<OpCode::name>, OpContext& c) { Atomic(c); }
// clang-format off
A(CompareAndSwap)
A(AtomicExchange)
A(AtomicFetchAdd)
A(AtomicRMW)
// clang-format on
#undef A

void Writer::Handle(Op<OpCode::CompareAndSwap128>, OpContext& c) {
    Read(c.Arg(0), rdi);
    Label aligned;
    test(rdi, 15);
    jz(aligned, T_NEAR);
    Exit(ExitValue(NativeExit::AlignmentFault));
    L(aligned);
    if (options.cx16) {
        Read(c.Arg(1), rax);
        Read(c.Arg(2), rdx);
        PublishFlags();
        fault_committed_flags = true;
        mov(r10, rbx);
        Read(c.Arg(3), rbx);
        Read(c.Arg(4), rcx);
        lock();
        cmpxchg16b(ptr[rdi]);
        mov(rbx, r10);
        movq(xmm0, rax);
        movq(xmm1, rdx);
        punpcklqdq(xmm0, xmm1);
        return;
    }
    if (Linked()) {
        PrepareCall();
        Read(c.Arg(0), rsi);
        mov(rdi, qword[r12 - 160]);
        mov(edx, 16);
        mov(r11, qword[r12 - 8]);
        Label unsupported, ready, fault;
        test(r11, r11);
        jz(unsupported, T_NEAR);
        CallRegister(r11);
        test(rax, rax);
        jz(ready, T_NEAR);
        cmp(rax, 1);
        je(fault, T_NEAR);
        L(unsupported);
        in_call = false;
        Exit(ExitValue(NativeExit::UnsupportedAtomic));
        L(fault);
        Exit(ExitValue(NativeExit::MemoryFault));
        L(ready);
    }
    PrepareCall();
    Read(c.Arg(0), rdi);
    Read(c.Arg(1), rsi);
    Read(c.Arg(2), rdx);
    Read(c.Arg(3), rcx);
    Read(c.Arg(4), r8);
    Call(reinterpret_cast<std::uintptr_t>(&CompareExchange128), HostSymbol::X64CompareExchange128);
    movq(xmm0, rax);
    movq(xmm1, rdx);
    punpcklqdq(xmm0, xmm1);
}
}  // namespace swift::runtime::backend::x64::detail
