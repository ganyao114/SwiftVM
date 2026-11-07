#include "emit.h"
#include "runtime/arch/x86/flags.h"

namespace swift::runtime::backend::x64::detail {
namespace f = runtime::x86::flags;

u64 PackedFlagMask(u64 s) {
    return ((s & 1) ? f::Carry : 0) | ((s & 2) ? f::Overflow : 0) | ((s & 4) ? f::Zero : 0) |
           ((s & 8) ? f::Sign : 0) | ((s & 16) ? 255 : 0) | ((s & 64) ? f::Auxiliary : 0) |
           ((s & 128) ? f::Direction : 0);
}

void Writer::MergeFlags(const Reg64& r, u64 selected) {
    auto m = PackedFlagMask(selected);
    if (!m) return;
    mov(r11, m);
    and_(r11, r);
    mov(rsi, ~m);
    and_(rbx, rsi);
    or_(rbx, r11);
}

void Writer::PackFlags(const Reg64& native, const Reg64& result) {
    mov(r10, result);
    and_(r10d, 255);
    const auto put = [&](unsigned src, unsigned dst) {
        mov(r11, native);
        shr(r11, src);
        and_(r11d, 1);
        shl(r11, dst);
        or_(r10, r11);
    };
    put(0, 29);
    put(6, 30);
    put(7, 31);
    put(11, 28);
    put(4, 26);
}

void Writer::CaptureFlags(OpContext& c) {
    if (!c.operation.flag_outputs && allocation.homes[c.node.value].auxiliary_offset == UINT32_MAX)
        return;
    pushfq();
    pop(rdx);
    PackFlags(rdx, c.destination);
    if (c.operation.opcode == OpCode::And || c.operation.opcode == OpCode::Or ||
        c.operation.opcode == OpCode::Xor || c.operation.opcode == OpCode::Not ||
        c.operation.opcode == OpCode::AndNot) {
        mov(r11, ~f::Auxiliary);
        and_(r10, r11);
    }
    const auto off = allocation.homes[c.node.value].auxiliary_offset;
    if (off != UINT32_MAX) mov(qword[rsp + off], r10);
    if (c.operation.Metadata().profile == FlagProfile::ArithmeticRaw &&
        (c.operation.opcode == OpCode::Sub || c.operation.opcode == OpCode::Sbb ||
         c.operation.opcode == OpCode::Neg))
        btc(r10, 29);
    MergeFlags(r10, c.operation.flag_outputs);
}

void Writer::Condition(const Reg64& out, ir::Condition c) {
    const auto bit = [&](const Reg64& r, unsigned b) {
        mov(r, rbx);
        shr(r, b);
        and_(r.cvt32(), 1);
    };
    switch (c) {
        case ir::Condition::EQ:
            bit(out, 30);
            break;
        case ir::Condition::NE:
            bit(out, 30);
            xor_(out.cvt32(), 1);
            break;
        case ir::Condition::CS:
            bit(out, 29);
            break;
        case ir::Condition::CC:
            bit(out, 29);
            xor_(out.cvt32(), 1);
            break;
        case ir::Condition::MI:
            bit(out, 31);
            break;
        case ir::Condition::PL:
            bit(out, 31);
            xor_(out.cvt32(), 1);
            break;
        case ir::Condition::VS:
            bit(out, 28);
            break;
        case ir::Condition::VC:
            bit(out, 28);
            xor_(out.cvt32(), 1);
            break;
        case ir::Condition::HI:
        case ir::Condition::LS:
            bit(out, 29);
            bit(r10, 30);
            xor_(r10d, 1);
            and_(out, r10);
            if (c == ir::Condition::LS) xor_(out.cvt32(), 1);
            break;
        case ir::Condition::GE:
        case ir::Condition::LT:
        case ir::Condition::GT:
        case ir::Condition::LE:
            bit(out, 31);
            bit(r10, 28);
            xor_(out, r10);
            if (c == ir::Condition::GT || c == ir::Condition::LE) {
                bit(r10, 30);
                or_(out, r10);
            }
            if (c == ir::Condition::GE || c == ir::Condition::GT) xor_(out.cvt32(), 1);
            break;
        default:
            mov(out, 1);
            break;
    }
}

void Writer::Handle(Op<OpCode::GetFlags>, OpContext&) { mov(rax, rbx); }

void Writer::Handle(Op<OpCode::RestoreFlags>, OpContext& c) {
    Read(c.Arg(0), rax, true);
    MergeFlags(rax, c.Arg(1).Payload());
}

void Writer::Handle(Op<OpCode::SaveFlags>, OpContext& c) {
    switch (source.Operations()[c.Arg(0).Payload()].opcode) {
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::Adc:
        case OpCode::Sbb:
        case OpCode::Neg:
        case OpCode::And:
        case OpCode::Or:
        case OpCode::Xor:
        case OpCode::AndNot:
        case OpCode::Not:
            break;
        default:
            Reject(CompileError::UnsupportedFlags);
    }
    const auto off = allocation.homes[c.Arg(0).Payload()].auxiliary_offset;
    if (off == UINT32_MAX) Reject(CompileError::UnsupportedFlags);
    mov(rax, qword[rsp + off]);
    MergeFlags(rax, c.Arg(1).Payload());
}

void Writer::Handle(Op<OpCode::BranchOnlyFlags>, OpContext& c) {
    Handle(Op<OpCode::SaveFlags>{}, c);
}

void Writer::Handle(Op<OpCode::ClearFlags>, OpContext& c) {
    mov(rax, (c.Arg(0).Payload() & 16) ? 1 : 0);
    MergeFlags(rax, c.Arg(0).Payload());
}

void Writer::Handle(Op<OpCode::SetCarry>, OpContext& c) {
    Read(c.Arg(0), rax);
    test(rax, rax);
    setnz(al);
    movzx(eax, al);
    shl(rax, 29);
    MergeFlags(rax, 1);
}

void Writer::Handle(Op<OpCode::SetOverflow>, OpContext& c) {
    Read(c.Arg(0), rax);
    test(rax, rax);
    setnz(al);
    movzx(eax, al);
    shl(rax, 28);
    MergeFlags(rax, 2);
}

void Writer::Handle(Op<OpCode::InvertCarry>, OpContext&) { btc(rbx, 29); }

void Writer::Handle(Op<OpCode::CondSet>, OpContext& c) {
    Condition(rax, static_cast<ir::Condition>(c.Arg(0).Payload()));
}

void Writer::Handle(Op<OpCode::LocalCondSet>, OpContext& c) { Handle(Op<OpCode::CondSet>{}, c); }

void Writer::Handle(Op<OpCode::LocalParitySet>, OpContext& c) {
    Read(c.Arg(0), rax, true);
    test(al, al);
    setp(al);
    movzx(eax, al);
    if (c.Arg(1).Payload()) xor_(eax, 1);
}

void Writer::Handle(Op<OpCode::TestFlags>, OpContext& c) {
    auto s = c.Arg(0).Payload();
    mov(rax, PackedFlagMask(s & ~u64{16}));
    and_(rax, rbx);
    test(rax, rax);
    setnz(al);
    movzx(eax, al);
    if (s & 16) {
        test(bl, bl);
        setp(dl);
        movzx(edx, dl);
        or_(rax, rdx);
    }
}

void Writer::Handle(Op<OpCode::TestNotFlags>, OpContext& c) {
    Handle(Op<OpCode::TestFlags>{}, c);
    xor_(eax, 1);
}

void Writer::Handle(Op<OpCode::PublishFCmpFlags>, OpContext& c) {
    Read(c.Arg(0), rax, true);
    mov(r10, rax);
    and_(r10d, 1);
    shl(r10, 29);
    mov(rdx, rax);
    and_(edx, 4);
    shl(rdx, 28);
    or_(r10, rdx);
    and_(eax, 2);
    cmp(eax, 0);
    sete(al);
    movzx(eax, al);
    or_(r10, rax);
    MergeFlags(r10, 95);
}

void Writer::Handle(Op<OpCode::PublishSse42StrFlags>, OpContext& c) {
    Read(c.Arg(0), rax, true);
    mov(r10, 1);
    const auto put = [&](unsigned s, unsigned d) {
        mov(rdx, rax);
        shr(rdx, s);
        and_(edx, 1);
        shl(rdx, d);
        or_(r10, rdx);
    };
    put(26, 29);
    put(27, 28);
    put(25, 30);
    put(24, 31);
    MergeFlags(r10, c.Arg(1).Payload());
}

void Writer::Handle(Op<OpCode::FCmpCondSet>, OpContext& c) {
    Read(c.Arg(0), rax, true);
    const auto cond = static_cast<ir::Condition>(c.Arg(1).Payload());
    u64 table{};
    for (unsigned v = 0; v < 8; ++v) {
        const bool n = v == 1, z = v == 4, carry = !n, overflow = v == 7;
        bool yes = true;
        switch (cond) {
            case ir::Condition::EQ:
                yes = z;
                break;
            case ir::Condition::NE:
                yes = !z;
                break;
            case ir::Condition::CS:
                yes = carry;
                break;
            case ir::Condition::CC:
                yes = !carry;
                break;
            case ir::Condition::MI:
                yes = n;
                break;
            case ir::Condition::PL:
                yes = !n;
                break;
            case ir::Condition::VS:
                yes = overflow;
                break;
            case ir::Condition::VC:
                yes = !overflow;
                break;
            case ir::Condition::HI:
                yes = carry && !z;
                break;
            case ir::Condition::LS:
                yes = !carry || z;
                break;
            case ir::Condition::GE:
                yes = n == overflow;
                break;
            case ir::Condition::LT:
                yes = n != overflow;
                break;
            case ir::Condition::GT:
                yes = !z && n == overflow;
                break;
            case ir::Condition::LE:
                yes = z || n != overflow;
                break;
            default:
                break;
        }
        table |= u64(yes) << v;
    }
    mov(rcx, rax);
    mov(rax, table);
    shr(rax, cl);
    and_(eax, 1);
}
}  // namespace swift::runtime::backend::x64::detail
