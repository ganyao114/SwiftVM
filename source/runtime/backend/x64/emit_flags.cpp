#include "translator.h"
#include "helpers.h"
#include "runtime/common/sse42str_result.h"

namespace swift::runtime::backend::x64 {
using O = ir::OpCode;
using namespace Xbyak;

void Translator::EmitPublishSse42StrFlags(ir::Inst* inst) {
    const auto requested = inst->GetArg<ir::Flags>(1);
    ReadArgument(rax, inst, 0);
    xor_(r8d, r8d);
    u64 mask{};
    const auto publish = [&](ir::Flags flag, u32 source, u32 target) {
        if (!True(requested & flag)) return;
        mask |= u64{1} << target;
        mov(rcx, rax); shr(rcx, source); and_(ecx, 1);
        shl(rcx, target); or_(r8, rcx);
    };
    publish(ir::Flags::Negate, sse42str::kSignBit, 31);
    publish(ir::Flags::Zero, sse42str::kZeroBit, 30);
    publish(ir::Flags::Carry, sse42str::kCarryBit, 29);
    publish(ir::Flags::Overflow, sse42str::kOverflowBit, 28);
    if (True(requested & ir::Flags::AuxiliaryCarry)) mask |= u64{1} << 26;
    if (True(requested & ir::Flags::Parity)) { mask |= 0xff; or_(r8, 1); }
    if (!mask) return;
    // Register masking preserves the unused high flag bits even when
    // clearing N (a 64-bit AND cannot encode that mask as imm32).
    if (flags_resident && flags_residency_active) {
        mov(rcx, ~mask); and_(rbx, rcx); or_(rbx, r8);
        ++statistics.flags_resident_update_sites;
    } else {
        ReadGuestFlags(rdx); mov(rcx, ~mask); and_(rdx, rcx); or_(rdx, r8);
        WriteGuestFlags(rdx);
    }
    return;
}

void Translator::EmitGetFlags(ir::Inst* inst) {
    ReadGuestFlags(rax); Write(inst, rax); return;
}

void Translator::EmitSaveFlags(ir::Inst* inst) {
    if (carry_chain_plan.DefersPublication(inst)) {
        // No observer or boundary precedes the pair's final full NZCV merge.
        // Retain the first native CF and all producer metadata snapshots.
        ++statistics.native_cf_merge_omissions;
        return;
    }
    const auto value = inst->GetArg<ir::Value>(0);
    SaveProducerFlags(value.Def(), inst->GetArg<ir::Flags>(1)); return;
}

void Translator::EmitBranchOnlyFlags(ir::Inst* inst) { EmitSaveFlags(inst); }

void Translator::EmitClearFlags(ir::Inst* inst) {
    const auto requested = inst->GetArg<ir::Flags>(0);
    u64 mask{};
    if (True(requested & ir::Flags::Negate)) mask |= u64{1} << 31;
    if (True(requested & ir::Flags::Zero)) mask |= u64{1} << 30;
    if (True(requested & ir::Flags::Carry)) mask |= u64{1} << 29;
    if (True(requested & ir::Flags::Overflow)) mask |= u64{1} << 28;
    if (True(requested & ir::Flags::AuxiliaryCarry)) mask |= u64{1} << 26;
    const bool parity = True(requested & ir::Flags::Parity);
    if (parity) mask |= 255;
    if (!mask) return; // Positive and unknown request bits have no state effect.
    if (flags_resident && flags_residency_active) {
        mov(rcx, ~mask); and_(rbx, rcx);
        if (parity) or_(rbx, 1);
        ++statistics.flags_resident_update_sites;
        return;
    }
    ReadGuestFlags(rax);
    // A qword AND immediate sign-extends imm32. Materialize the full mask so
    // clearing N (bit 31) cannot clear the unused high 32 bits of State flags.
    mov(rcx, ~mask); and_(rax, rcx);
    if (parity) or_(rax, 1); // Saved byte 1 has odd parity, so guest PF is false.
    WriteGuestFlags(rax);
}

void Translator::EmitFlagTest(ir::Inst* inst, ir::OpCode op) {
    const auto requested = inst->GetArg<ir::Flags>(0);
    u32 nzcv{};
    if (True(requested & ir::Flags::Negate)) nzcv |= u32{1} << 31;
    if (True(requested & ir::Flags::Zero)) nzcv |= u32{1} << 30;
    if (True(requested & ir::Flags::Carry)) nzcv |= u32{1} << 29;
    if (True(requested & ir::Flags::Overflow)) nzcv |= u32{1} << 28;
    const bool parity = True(requested & ir::Flags::Parity);
    const bool auxiliary = True(requested & ir::Flags::AuxiliaryCarry);
    bool initialized{};
    if (nzcv || parity || auxiliary) ReadGuestFlags(rdx);
    if (nzcv) {
        // Multiple requested NZCV bits are an ANY predicate, not ALL. The
        // 32-bit test excludes unused high State bits from an imm32 mask.
        test(edx, nzcv); setne(al); movzx(eax, al); initialized = true;
    }
    if (parity) {
        // Capture native PF immediately after testing only the saved byte.
        test(dl, dl); setp(cl); movzx(ecx, cl);
        if (initialized) and_(eax, ecx); else mov(eax, ecx);
        initialized = true;
    }
    if (auxiliary) {
        mov(rcx, rdx); shr(rcx, 26); and_(ecx, 1);
        if (initialized) and_(eax, ecx); else mov(eax, ecx);
        initialized = true;
    }
    if (!initialized) xor_(eax, eax); // None, Positive-only, or unknown-only.
    if (op == O::TestNotFlags) xor_(eax, 1);
    Write(inst, rax);
}

void Translator::EmitTestFlags(ir::Inst* inst) { EmitFlagTest(inst, O::TestFlags); }

void Translator::EmitTestNotFlags(ir::Inst* inst) { EmitFlagTest(inst, O::TestNotFlags); }

void Translator::EmitSetFlag(ir::Inst* inst, ir::OpCode op) {
    const auto bit = op == O::SetCarry ? 29 : 28;
    ReadArgument(rax, inst, 0); and_(eax, 1); shl(rax, bit);
    if (flags_resident && flags_residency_active) {
        // Bits 28/29 leave mask bit 31 set: imm32 sign extension supplies
        // all-one high bits, preserving the unused packed State bits.
        and_(rbx, ~(1u << bit)); or_(rbx, rax);
        ++statistics.flags_resident_update_sites;
        return;
    }
    ReadGuestFlags(rdx); and_(rdx, ~(1u << bit)); or_(rdx, rax); WriteGuestFlags(rdx); return;
}

void Translator::EmitSetCarry(ir::Inst* inst) { EmitSetFlag(inst, O::SetCarry); }

void Translator::EmitSetOverflow(ir::Inst* inst) { EmitSetFlag(inst, O::SetOverflow); }

void Translator::EmitInvertCarry(ir::Inst* inst) {
    if (flags_resident && flags_residency_active) {
        xor_(rbx, 1u << 29);
        ++statistics.flags_resident_update_sites;
        return;
    }
    ReadGuestFlags(rax); xor_(rax, 1u << 29); WriteGuestFlags(rax); return;
}

void Translator::EmitCondSet(ir::Inst* inst) {
    Condition(inst->GetArg<ir::Cond>(0)); Write(inst, rax); return;
}

void Translator::EmitLocalCondSet(ir::Inst* inst) { EmitCondSet(inst); }

void Translator::EmitFCmpCondSet(ir::Inst* inst) {
    ReadArgument(rdi, inst, 0); mov(esi, static_cast<u32>(inst->GetArg<ir::Cond>(1))); Call(reinterpret_cast<u64>(&CompareCondition)); Write(inst, rax); return;
}

void Translator::EmitPublishFCmpFlags(ir::Inst* inst) {
    // Match PublishCompareFlags exactly. Compact is a truth value, not a
    // one-bit mask; x86 PF is represented by the parity of a saved byte.
    ReadArgument(rax, inst, 0); xor_(r8d, r8d);
    mov(rcx, rax); and_(ecx, 1);
    if (Immediate(inst, 1) != 0) xor_(ecx, 1);
    shl(rcx, 29); or_(r8, rcx);
    mov(rcx, rax); shr(rcx, 2); and_(ecx, 1); shl(rcx, 30); or_(r8, rcx);
    shr(rax, 1); and_(eax, 1); xor_(eax, 1); or_(r8, rax);
    constexpr u64 mask = (u64{15} << 28) | (u64{1} << 26) | 255;
    if (flags_resident && flags_residency_active) {
        mov(rcx, ~mask); and_(rbx, rcx); or_(rbx, r8);
        ++statistics.flags_resident_update_sites;
    } else {
        ReadGuestFlags(rdx); mov(rcx, ~mask); and_(rdx, rcx); or_(rdx, r8); WriteGuestFlags(rdx);
    }
}

void Translator::EmitLocalParitySet(ir::Inst* inst) {
    ReadArgument(rax, inst, 0); test(al, al); if (Immediate(inst, 1)) setnp(al); else setp(al); movzx(eax, al); Write(inst, rax); return;
}

}  // namespace swift::runtime::backend::x64
