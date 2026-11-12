#include "abi_layout.inc"
#include "emit.h"
#include "runtime/arch/x86/flags.h"

namespace swift::runtime::backend::x64::detail {
namespace f = runtime::x86::flags;
using O = OpCode;

void Writer::ImportLinkedFlags() {
    if (!options.linked_flags) {
        ImportPackedFlags();
        return;
    }
    mov(r11, qword[r12 + SVM_X64_CTX_EFLAGS - SVM_X64_CTX_STATE]);
    mov(qword[rsp + allocation.flags_slot + stack_bias], r11);
    flags = {true, false, true, false, true};
}

void Writer::ExportLinkedFlags() {
    if (!options.linked_flags) {
        PublishFlags();
        return;
    }
    SaveNativeFlags(true);
    mov(r11, qword[rsp + allocation.flags_slot + stack_bias]);
    mov(qword[r12 + SVM_X64_CTX_EFLAGS - SVM_X64_CTX_STATE], r11);
}

void Writer::ImportPackedFlags() {
    pushfq();
    pop(rdx);
    and_(rdx, ~0x8c1);
    mov(rax, rbx);
    shr(rax, 24);
    and_(eax, 0xc0);
    or_(rdx, rax);
    mov(rax, rbx);
    shr(rax, 29);
    and_(eax, 1);
    xor_(eax, 1);
    or_(rdx, rax);
    mov(rax, rbx);
    shr(rax, 17);
    and_(eax, 0x800);
    or_(rdx, rax);
    mov(qword[rsp + allocation.flags_slot + stack_bias + flags_stack_bias], rdx);
    flags = {true, false, true, false, true};
}

void Writer::SaveNativeFlags(bool canonical) {
    if (!options.local_flags) return;
    if (!flags.dirty) {
        if (canonical) ImportPackedFlags();
        return;
    }
    if (flags.saved && (!canonical || flags.saved_carry)) return;
    if (flags.native) {
        if (canonical && !flags.carry) {
            cmc();
            flags.carry = true;
        }
        pushfq();
        pop(r11);
        mov(qword[rsp + allocation.flags_slot + stack_bias + flags_stack_bias], r11);
        flags.saved = true;
        flags.saved_carry = flags.carry;
    } else if (canonical && !flags.saved_carry) {
        xor_(qword[rsp + allocation.flags_slot + stack_bias + flags_stack_bias], 1);
        flags.saved_carry = true;
    }
}

void Writer::MaterializeFlags() {
    if (!options.local_flags || !flags.dirty) return;
    SaveNativeFlags();
    mov(rdx, qword[rsp + allocation.flags_slot + stack_bias + flags_stack_bias]);
    PackFlags(rdx, rax, 15);
    if (flags.saved_carry) btc(r10, 29);
    MergeFlags(r10, 15);
    flags = {};
}

void Writer::EnsureNativeFlags() {
    if (!flags.dirty) ImportPackedFlags();
    if (!flags.native) {
        push(qword[rsp + allocation.flags_slot + stack_bias]);
        popfq();
        flags.native = true;
        flags.carry = flags.saved_carry;
    }
}

void Writer::PrepareFlags(OpContext& c) {
    if (!options.local_flags) return;
    const auto op = c.operation.opcode;
    const bool arithmetic =
            op == O::Add || op == O::Sub || op == O::Neg || op == O::Adc || op == O::Sbb;
    const bool logical =
            op == O::And || op == O::Or || op == O::Xor || op == O::AndNot || op == O::Not;
    const bool native_output = c.operation.flag_outputs && !(c.operation.flag_outputs & ~15u) &&
                               (c.width == 32 || c.width == 64) &&
                               allocation.homes[c.node.value].auxiliary_offset == UINT32_MAX &&
                               (arithmetic || logical);
    defer_flags = native_output && c.operation.flag_outputs == 15;
    merge_native_flags = native_output && !defer_flags;
    if (native_output) {
        if (merge_native_flags || op == O::Adc || op == O::Sbb) {
            SaveNativeFlags(true);
            flags.native = false;
        } else
            flags = {};
        return;
    }
    if (op == O::Nop || op == O::UniformBarrier || op == O::XchgBarrier ||
        op == O::BranchOnlyEdges || op == O::VecLoadConst || op == O::VecSharedZero ||
        op == O::MemoryBarrierTSO || op == O::MemoryStoreFence || op == O::MemoryLoadFence ||
        op == O::CpuYield)
        return;
    if ((op == O::BitCast || op == O::GetResult || op == O::GetUniformAddress ||
         op == O::GetLocation || op == O::SetLocation) &&
        (c.width >= 32 || c.width == 0))
        return;
    if (op == O::InvertCarry || op == O::GuestSafepoint || op == O::GuestDispatchSafepoint) return;
    if (op == O::CondSet || op == O::LocalCondSet || op == O::CondSelect) return;
    if (op == O::TestFlags || op == O::TestNotFlags) {
        const auto mask = c.Arg(0).Payload();
        if (mask && mask <= 8 && !(mask & (mask - 1))) return;
    }
    const auto effects = EffectsFor(op, c.operation.Metadata());
    if (HasAnyEffect(effects, Effect::ReadFlags | Effect::WriteFlags)) {
        MaterializeFlags();
        return;
    }
    if ((op == O::LoadUniform && c.width >= 8) ||
        (op == O::StoreUniform && TypeBits(View(c.Arg(1))) >= 32) ||
        (op == O::LoadImm && c.width >= 32))
        return;
    SaveNativeFlags();
    flags.native = false;
}

unsigned Writer::NativeCondition(ir::Condition condition) {
    EnsureNativeFlags();
    if ((condition == ir::Condition::HI || condition == ir::Condition::LS) && !flags.carry) {
        cmc();
        flags.carry = true;
    }
    switch (condition) {
        case ir::Condition::EQ:
            return 4;
        case ir::Condition::NE:
            return 5;
        case ir::Condition::CS:
            return flags.carry ? 3 : 2;
        case ir::Condition::CC:
            return flags.carry ? 2 : 3;
        case ir::Condition::MI:
            return 8;
        case ir::Condition::PL:
            return 9;
        case ir::Condition::VS:
            return 0;
        case ir::Condition::VC:
            return 1;
        case ir::Condition::HI:
            return 7;
        case ir::Condition::LS:
            return 6;
        case ir::Condition::GE:
            return 13;
        case ir::Condition::LT:
            return 12;
        case ir::Condition::GT:
            return 15;
        case ir::Condition::LE:
            return 14;
        default:
            return 16;
    }
}

void Writer::NativeJump(unsigned condition, Label& target) {
    switch (condition) {
        case 0:
            jo(target, T_NEAR);
            break;
        case 1:
            jno(target, T_NEAR);
            break;
        case 2:
            jc(target, T_NEAR);
            break;
        case 3:
            jnc(target, T_NEAR);
            break;
        case 4:
            je(target, T_NEAR);
            break;
        case 5:
            jne(target, T_NEAR);
            break;
        case 6:
            jbe(target, T_NEAR);
            break;
        case 7:
            ja(target, T_NEAR);
            break;
        case 8:
            js(target, T_NEAR);
            break;
        case 9:
            jns(target, T_NEAR);
            break;
        case 12:
            jl(target, T_NEAR);
            break;
        case 13:
            jge(target, T_NEAR);
            break;
        case 14:
            jle(target, T_NEAR);
            break;
        case 15:
            jg(target, T_NEAR);
            break;
        default:
            jmp(target, T_NEAR);
            break;
    }
}

void Writer::BranchTo(BlockId target) {
    if (options.local_flags && allocation.flags_input[target.value]) SaveNativeFlags(true);
    jmp(blocks[target.value], T_NEAR);
}

void Writer::Handle(Op<OpCode::ReadFPStatus>, OpContext& c) {
    if (Linked()) {
        stmxcsr(dword[r12 - 40]);
        mov(eax, dword[r12 - 40]);
    } else {
        stmxcsr(dword[rsp + mxcsr_slot + 4]);
        mov(eax, dword[rsp + mxcsr_slot + 4]);
    }
    Read(c.Arg(0), rdx);
    or_(eax, edx);
    and_(eax, 63);
}

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

void Writer::PackFlags(const Reg64& native, const Reg64& result, u64 selected) {
    // Liveness already names the required flags. A named snapshot still asks
    // for all flags below, but ordinary ARM NZCV writes need no parity or AF.
    const auto zs = ((selected & 4) ? 0x40 : 0) | ((selected & 8) ? 0x80 : 0);
    if (zs) {
        mov(r10d, native.cvt32());
        and_(r10d, zs);
        shl(r10d, 24);
    } else
        xor_(r10d, r10d);
    if (selected & 16) {
        mov(r11d, result.cvt32());
        and_(r11d, 255);
        or_(r10d, r11d);
    }
    const auto put = [&](u64 flag, unsigned src, unsigned shift) {
        if (!(selected & flag)) return;
        mov(r11d, native.cvt32());
        and_(r11d, 1u << src);
        shl(r11d, shift);
        or_(r10d, r11d);
    };
    put(1, 0, 29);
    put(2, 11, 17);
    put(64, 4, 22);
}

void Writer::CaptureFlags(OpContext& c) {
    const auto off = allocation.homes[c.node.value].auxiliary_offset;
    if (!c.operation.flag_outputs && off == UINT32_MAX) return;
    if (options.local_flags && defer_flags) {
        const bool subtract = c.operation.opcode == OpCode::Sub ||
                              c.operation.opcode == OpCode::Sbb ||
                              c.operation.opcode == OpCode::Neg;
        flags = {true,
                 true,
                 false,
                 subtract && c.operation.Metadata().profile == FlagProfile::ArithmeticRaw,
                 false};
        return;
    }
    if (options.local_flags && merge_native_flags) {
        const auto selected = c.operation.flag_outputs;
        const unsigned mask = ((selected & 1) ? 1u : 0u) | ((selected & 2) ? 0x800u : 0u) |
                              ((selected & 4) ? 0x40u : 0u) | ((selected & 8) ? 0x80u : 0u);
        const auto op = c.operation.opcode;
        const bool raw_subtract = (op == O::Sub || op == O::Sbb || op == O::Neg) &&
                                  c.operation.Metadata().profile == FlagProfile::ArithmeticRaw;
        pushfq();
        pop(rdx);
        if ((selected & 1) && !raw_subtract) xor_(edx, 1);
        and_(edx, mask);
        mov(r11, qword[rsp + allocation.flags_slot]);
        and_(r11, static_cast<std::int32_t>(~mask));
        or_(r11, rdx);
        mov(qword[rsp + allocation.flags_slot], r11);
        flags = {true, false, true, false, true};
        return;
    }
    pushfq();
    pop(rdx);
    PackFlags(
            rdx, c.destination, off == UINT32_MAX ? c.operation.flag_outputs : FlagBits(Flag::All));
    if (c.operation.opcode == OpCode::And || c.operation.opcode == OpCode::Or ||
        c.operation.opcode == OpCode::Xor || c.operation.opcode == OpCode::Not ||
        c.operation.opcode == OpCode::AndNot) {
        mov(r11, ~f::Auxiliary);
        and_(r10, r11);
    }
    if (off != UINT32_MAX) mov(qword[rsp + off], r10);
    if (c.operation.Metadata().profile == FlagProfile::ArithmeticRaw &&
        (c.operation.opcode == OpCode::Sub || c.operation.opcode == OpCode::Sbb ||
         c.operation.opcode == OpCode::Neg))
        btc(r10, 29);
    MergeFlags(r10, c.operation.flag_outputs);
}

void Writer::Condition(const Reg64& out, ir::Condition c) {
    if (options.local_flags) {
        const auto condition = NativeCondition(c);
        if (condition == 16)
            mov(out, 1);
        else {
            db(0x0f);
            db(0x90 + condition);
            db(0xc0);
            movzx(out.cvt32(), al);
        }
        return;
    }
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

void Writer::Handle(Op<OpCode::InvertCarry>, OpContext&) {
    if (options.local_flags && flags.dirty) {
        flags.carry = !flags.carry;
        flags.saved_carry = !flags.saved_carry;
    } else
        btc(rbx, 29);
}

void Writer::Handle(Op<OpCode::CondSet>, OpContext& c) {
    Condition(rax, static_cast<ir::Condition>(c.Arg(0).Payload()));
    c.scalar_normalized = true;
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
    if (NativeFlagTest(c, false)) return;
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
    if (NativeFlagTest(c, true)) return;
    Handle(Op<OpCode::TestFlags>{}, c);
    xor_(eax, 1);
}

bool Writer::NativeFlagTest(OpContext& c, bool inverse) {
    if (!options.local_flags) return false;
    ir::Condition condition;
    switch (c.Arg(0).Payload()) {
        case 1:
            condition = ir::Condition::CS;
            break;
        case 2:
            condition = ir::Condition::VS;
            break;
        case 4:
            condition = ir::Condition::EQ;
            break;
        case 8:
            condition = ir::Condition::MI;
            break;
        default:
            return false;
    }
    if (inverse) condition = static_cast<ir::Condition>(unsigned(condition) ^ 1);
    Condition(rax, condition);
    c.scalar_normalized = true;
    return true;
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
