#include "translator.h"

#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

void JitTranslator::MergeNZCV() {
    if (save_in_nzcv && nzcv_dirty) {
        // Only merge the NZCV bits that SaveFlags actually requested.
        // Bits NOT requested (e.g. C/V when only SF/ZF were saved) keep
        // their existing value in the flags register, so a ClearFlags(CF)
        // between two flag-setting instructions is not overwritten.
        const u64 req = static_cast<u64>(nzcv_requested);
        u64 keep = ~req;
        __ Mrs(ip, NZCV);
        __ And(flags, flags, ForceCast<s64>(keep));
        __ And(ip, ip, static_cast<u32>(req));
        __ Orr(flags, flags, ip);
        nzcv_dirty = false;
        nzcv_requested = {};
    }
}

void JitTranslator::LoadNZCVFromFlags() {
    __ And(ip, flags, static_cast<u64>(HostFlags::NZCV));
    __ Msr(NZCV, ip);
}

void JitTranslator::MergeLogicalFlagsNZ(ir::Flags requested) {
    // Logical flag producers may have only an N or only a Z SaveFlags pseudo
    // (SAHF deliberately writes them independently).  Commit exactly that
    // requested subset: merging both bits lets a later SF-only zero result
    // resurrect an earlier ZF that SAHF already cleared.
    const u64 requested_nz =
            static_cast<u64>(GuestNZCVToHost(requested & ir::Flags::NZ));
    if (!requested_nz) {
        return;
    }
    u64 keep = ~requested_nz;
    __ Mrs(ip, NZCV);
    __ And(flags, flags, ForceCast<s64>(keep));
    __ And(ip, ip, static_cast<u32>(requested_nz));
    __ Orr(flags, flags, ip);
    nzcv_dirty = false;
    nzcv_requested = {};
}

void JitTranslator::SaveLogicalResultFlags(Register& result,
                                           ir::ValueType type,
                                           const PseudoFlags& pseudo) {
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Sxtb(ip, result.W());
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Sxth(ip, result.W());
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Sxtw(ip, result.W());
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Mov(ip, result);
            break;
        default:
            PANIC();
    }
    // NZ from the sign-extended result, with C/V cleared — the x86 logical-op
    // flag shape. Must NOT be spelled `Bics(ip, ip, 0)`: vixl inverts a BIC
    // immediate, and the resulting all-ones is not a legal logical immediate,
    // so LogicalMacro synthesizes it through UseScratchRegisterScope and
    // silently clobbers x16 (vixl's tmp_list_ is {x16, x17}, which this
    // backend does not reserve from the register allocator). Tst is the
    // register form of ANDS and yields the identical NZCV with no scratch.
    __ Tst(ip, ip);
    MergeLogicalFlagsNZ(pseudo.set);
    if (True(pseudo.set & ir::Flags::Parity)) {
        SaveParity(result);
    }
}

void JitTranslator::SaveHostFlags(HostFlags host, ir::Flags guest) {
    // To arm64 host
    HostFlags host_need_saved{};
    if (True(guest & ir::Flags::Negate)) {
        host_need_saved |= HostFlags::N;
    }
    if (True(guest & ir::Flags::Zero)) {
        host_need_saved |= HostFlags::Z;
    }
    if (True(guest & ir::Flags::Carry)) {
        host_need_saved |= HostFlags::C;
    }
    if (True(guest & ir::Flags::Overflow)) {
        host_need_saved |= HostFlags::V;
    }
    if (save_in_nzcv) {
        // Accumulate which NZCV bits were actually requested by guest
        // SaveFlags. MergeNZCV will only merge these bits, preserving
        // any ClearFlags(CF/OF) that happened between flag-setting
        // instructions.
        nzcv_requested |= host_need_saved;
        nzcv_dirty = true;
    } else {
        __ Mrs(ip, NZCV);
        if (host_need_saved != host) {
            __ And(ip, ip, static_cast<u32>(host_need_saved));
        }
        __ Orr(flags, flags, ip);
    }
}

void JitTranslator::ClearFlags(ir::Flags guest) {
    if (True(guest & ir::Flags::NZCV)) {
        // ClearFlags is an independent IR write, not merely an annotation on
        // the preceding flag producer. Flag elimination can delete a dead
        // SaveFlags and DCE can then delete that producer, while a sibling
        // ClearFlags(C/V) remains live (x86 logical ops followed by ADC/SBB).
        //
        // Commit a pending lazy producer before clearing the stored bits. This
        // also prevents a later MergeNZCV from resurrecting a bit cleared here.
        MergeNZCV();
        u64 mask{UINT64_MAX};
        if (True(guest & ir::Flags::Negate)) {
            mask &= ~(u64(1) << HostFlagsBit::N);
        }
        if (True(guest & ir::Flags::Zero)) {
            mask &= ~(u64(1) << HostFlagsBit::Z);
        }
        if (True(guest & ir::Flags::Carry)) {
            mask &= ~(u64(1) << HostFlagsBit::C);
        }
        if (True(guest & ir::Flags::Overflow)) {
            mask &= ~(u64(1) << HostFlagsBit::V);
        }
        __ And(flags, flags, ForceCast<s64>(mask));
    }
    if (True(guest & ir::Flags::Parity)) {
        // Clear Parity: an odd-parity byte makes TestParityFlag read PF = 0.
        __ Mov(ip, 1);
        __ Bfi(flags, ip, HostFlagsBit::ParityByte, 8);
    }
    if (True(guest & ir::Flags::AuxiliaryCarry)) {
        // AF is a single bit (carry into bit 4).
        __ Bfc(flags, HostFlagsBit::AuxiliaryCarry, 1);
    }
}

void JitTranslator::SaveParity(Register& value) {
    __ Bfi(flags, value, HostFlagsBit::ParityByte, 8);
}

void JitTranslator::SaveNZ(Register& value, ir::ValueType type) {
    switch (type) {
        case ir::ValueType::U8:
            __ Sxtb(ip, value);
            break;
        case ir::ValueType::U16:
            __ Sxth(ip, value);
            break;
        case ir::ValueType::U32:
            __ Sxtw(ip, value);
            break;
        case ir::ValueType::U64:
            __ Mov(ip, value);
            break;
        default:
            PANIC();
    }
    // Same reasoning as SaveLogicalResultFlags: Tst avoids the vixl x16
    // scratch that `Bics(ip, ip, 0)` would take.
    __ Tst(ip, ip);
    if (save_in_nzcv) {
        nzcv_dirty = true;
    } else {
        __ Mrs(ip, NZCV);
        __ Orr(flags, flags, ip);
    }
}

// SaveCV / SaveOF set C+V (resp. V) when `value` does not fit in `type` --
// the x86 mul/imul CF/OF shape, computed from the upper half of a widened
// product.
//
// Both used to have a `save_in_nzcv` path that wrote the bits into the HOST
// NZCV register with Msr and then set nzcv_dirty = false.  Every one of those
// three spellings was wrong, and they compounded:
//
//   * nzcv_dirty = false makes MergeNZCV() a no-op, so bits placed in host
//     NZCV were never copied into `flags` -- CF/OF were silently dropped.
//   * Setting nzcv_dirty = true instead would not have helped: the Msr sits
//     inside the Cbz skip, so on the no-overflow path host NZCV still holds
//     the PREVIOUS producer's result and merging it would invent flags.
//   * nzcv_requested was never widened to C|V, so MergeNZCV's mask would have
//     filtered the bits out even if the other two had been right.
//
// The lazy-NZCV representation cannot express "conditionally set two bits", so
// do not try: commit any pending producer up front (MergeNZCV is a no-op when
// nothing is pending -- both call sites already invoke it) and then OR the bits
// straight into the `flags` register, which is what the non-lazy branch and
// EmitMul's inline signed-overflow path have always done.  Nothing here touches
// host NZCV afterwards; Lsr, Cbz and a non-flag-setting Orr all leave it alone.
//
// Reachability, as of this commit: the x86 frontend cannot get here.  Its
// mul/imul lowering (MulWithFlags in frontend/x86/decoder_alu.cc) deliberately
// materialises CF/OF through a separate `t = bad << 63; SaveFlags(t + t, C|V)`
// producer rather than hanging SaveFlags(CV) on the Mul itself, and its only
// ir::Div is the RCL/RCR modulus, which carries no flags.  SaveOF has no caller
// at all.  So this is a latent-bug fix, exercised by the codegen-shape test
// "SaveCV commits x86 CF/OF into the flags register" in tests/main_case.cpp
// (which builds the Mul + SaveFlags(C|V) IR the frontend does not currently
// emit), not by any guest program.
void JitTranslator::SaveCV(Register& value, ir::ValueType type) {
    if (type == ir::ValueType::U64) {
        return;
    }
    MergeNZCV();
    Label pass;
    __ Lsr(ip, value, ir::GetValueSizeByte(type) * 8);
    __ Cbz(ip, &pass);
    __ Orr(flags, flags, 3u << HostFlagsBit::V);
    __ Bind(&pass);
}

void JitTranslator::SaveOF(Register& value, ir::ValueType type) {
    if (type == ir::ValueType::U64) {
        return;
    }
    MergeNZCV();
    Label pass;
    __ Lsr(ip, value, ir::GetValueSizeByte(type) * 8);
    __ Cbz(ip, &pass);
    __ Orr(flags, flags, 1u << HostFlagsBit::V);
    __ Bind(&pass);
}

void JitTranslator::SaveAuxiliaryCarry(Register &left, const Operand &right, Register &result) {
    // AF = carry into bit 4 = bit4(left) ^ bit4(right) ^ bit4(result). This holds
    // for add/adc/sub/sbb alike (result already reflects any carry-in). Only the
    // three bit-4s matter, so fold the whole values together and extract bit 4.
    auto tmp = context.GetTmpX();
    __ Eor(tmp, left.X(), Operand{result.X()});
    if (right.IsImmediate()) {
        if ((right.GetImmediate() >> 4) & 1) {
            __ Eor(tmp, tmp, 1u << 4);
        }
    } else {
        // Plain or shifted register: materialize its effective bits (.X view keeps
        // bit 4 correct for the W-width shift forms too); only bit 4 survives.
        auto reg = right.GetRegister().X();
        auto shift = right.IsShiftedRegister() ? right.GetShift() : LSL;
        auto amount = right.IsShiftedRegister() ? right.GetShiftAmount() : 0;
        __ Eor(tmp, tmp, Operand{reg, shift, amount});
    }
    __ Ubfx(tmp, tmp, 4, 1);
    __ Bfi(flags, tmp, HostFlagsBit::AuxiliaryCarry, 1);
}

void JitTranslator::GetParityFlag(const Register& result) {
    __ Ubfx(result.W(), flags, HostFlagsBit::ParityByte, 8);
    __ Eor(result.W(), result.W(), Operand{result.W(), LSR, 4});
    __ Eor(result.W(), result.W(), Operand{result.W(), LSR, 2});
    __ Eor(result.W(), result.W(), Operand{result.W(), LSR, 1});
}

void JitTranslator::TestParityFlag(const Register& result) {
    GetParityFlag(result);
    __ And(result.W(), result.W(), 1);
    // x86 PF is set on even parity
    __ Eor(result.W(), result.W(), 1);
}

void JitTranslator::TestAuxiliaryCarry(const Register& result) {
    // AF is stored as a single bit (the carry into bit 4) at AuxiliaryCarry.
    __ Ubfx(result, flags, HostFlagsBit::AuxiliaryCarry, 1);
}

JitTranslator::PseudoFlags JitTranslator::GetPseudoFlags(ir::Inst* inst) {
    ir::Flags result_set{};
    ir::Flags result_clear{};
    if (auto pseudos = inst->GetPseudoOperations(); !pseudos.empty()) {
        for (auto& pseudo : pseudos) {
            if (pseudo->GetOp() == ir::OpCode::SaveFlags) {
                auto guest_flags = pseudo->GetArg<ir::Flags>(1);
                result_set |= guest_flags;
            } else if (pseudo->GetOp() == ir::OpCode::ClearFlags) {
                auto guest_flags = pseudo->GetArg<ir::Flags>(0);
                result_clear |= guest_flags;
            }
        }
    }
    return {result_set, result_clear};
}

void JitTranslator::EmitSaveFlags(ir::Inst* inst) {
    // Multiple SaveFlags may appear in one flush window (e.g. the x86 frontend
    // emits separate PF/AF and NZCV saves for narrow ALU ops); merge them.
    flags_set |= inst->GetArg<ir::Flags>(1);
}

void JitTranslator::EmitClearFlags(ir::Inst* inst) {
    // See EmitSaveFlags: merge instead of asserting on a pending window.
    flags_clear |= inst->GetArg<ir::Flags>(0);
}

void JitTranslator::EmitSetCarry(ir::Inst* inst) {
    // Set guest CF directly in the flags register from a computed 0/1 value.
    // Merge pending NZCV first so no later merge clobbers the bit we write
    // (MergeNZCV leaves nzcv_dirty=false; Bfi does not set it).
    MergeNZCV();
    // A preceding ClearFlags may still be queued in the lazy flag window.
    // Apply it before inserting CF, otherwise the next flush clears the bit
    // just written (RDRAND/RDSEED are the minimal reproducer).
    FlushFlags();
    auto bit = context.R(inst->GetArg<ir::Value>(0));
    __ Bfi(flags, bit.X(), HostFlagsBit::C, 1);
}

void JitTranslator::EmitSetOverflow(ir::Inst* inst) {
    MergeNZCV();
    FlushFlags();
    auto bit = context.R(inst->GetArg<ir::Value>(0));
    __ Bfi(flags, bit.X(), HostFlagsBit::V, 1);
}

void JitTranslator::FlushFlags() {
    if (flags_clear != ir::Flags::None) {
        ClearFlags(flags_clear);
    }

    flags_set = ir::Flags::None;
    flags_clear = ir::Flags::None;
}

void JitTranslator::EmitTestBit(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto bit = inst->GetArg<ir::Imm>(1).Get();
    if (ir::GetValueSizeByte(value.Type()) == 8) {
        __ Ubfx(context.X(ir::Value{inst}), context.X(value), bit, 1);
    } else {
        __ Ubfx(context.W(ir::Value{inst}), context.W(value), bit, 1);
    }
}

void JitTranslator::EmitGetFlags(ir::Inst* inst) {
    MergeNZCV();
    __ Mov(context.R(ir::Value{inst}), flags);
}

void JitTranslator::EmitTestFlags(ir::Inst* inst) {
    auto test = inst->GetArg<ir::Flags>(0);
    auto result = context.W(ir::Value{inst});
    auto nzcv_mask = static_cast<u32>(GuestNZCVToHost(test));
    bool first{true};
    if (nzcv_mask) {
        if (save_in_nzcv && nzcv_dirty) {
            __ Mrs(ip, NZCV);
            __ Tst(ip, nzcv_mask);
        } else {
            __ Tst(flags, nzcv_mask);
        }
        __ Cset(result, ne);
        first = false;
    }
    if (True(test & ir::Flags::Parity)) {
        TestParityFlag(ip);
        if (first) {
            __ Mov(result, ip.W());
        } else {
            __ And(result, result, ip.W());
        }
        first = false;
    }
    if (True(test & ir::Flags::AuxiliaryCarry)) {
        TestAuxiliaryCarry(ip);
        if (first) {
            __ Mov(result, ip.W());
        } else {
            __ And(result, result, ip.W());
        }
        first = false;
    }
    if (first) {
        __ Mov(result, 0);
    }
}

void JitTranslator::EmitTestNotFlags(ir::Inst* inst) {
    auto test = inst->GetArg<ir::Flags>(0);
    auto nzcv_mask = static_cast<u32>(GuestNZCVToHost(test));
    if (nzcv_mask && !True(test & (ir::Flags::Parity | ir::Flags::AuxiliaryCarry))) {
        auto result = context.W(ir::Value{inst});
        if (save_in_nzcv && nzcv_dirty) {
            __ Mrs(ip, NZCV);
            __ Tst(ip, nzcv_mask);
        } else {
            __ Tst(flags, nzcv_mask);
        }
        __ Cset(result, eq);
    } else {
        EmitTestFlags(inst);
        auto result = context.W(ir::Value{inst});
        __ Eor(result, result, 1);
        __ And(result, result, 1);
    }
}

}  // namespace swift::runtime::backend::arm64
