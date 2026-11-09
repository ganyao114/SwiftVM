// Imported A32 instruction semantics.
// Adapted from dynarmic-interp 5a76f693 (MerryMage and contributors), SPDX-License-Identifier:
// 0BSD.

// barrier
/* This file is part of the dynarmic project.
 * Copyright (c) 2019 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

bool arm_DMB(Imm<4> /*option*/) {
    ir.DataMemoryBarrier();
    return true;
}

bool arm_DSB(Imm<4> /*option*/) {
    ir.DataSynchronizationBarrier();
    return true;
}

bool arm_ISB(Imm<4> /*option*/) {
    ir.InstructionSynchronizationBarrier();
    ir.BranchWritePC(ir.Imm32(ir.current_location.PC() + 4));
    ir.SetTerm(Term::ReturnToDispatch{});
    return false;
}

// hint
/* This file is part of the dynarmic project.
 * Copyright (c) 2019 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

bool arm_PLD_imm(bool /*add*/, bool R, Reg /*n*/, Imm<12> /*imm12*/) {
    if (!options.hook_hint_instructions) {
        return true;
    }

    const auto exception = R ? Exception::PreloadData : Exception::PreloadDataWithIntentToWrite;
    return RaiseException(exception);
}

bool arm_PLD_reg(bool /*add*/, bool R, Reg /*n*/, Imm<5> /*imm5*/, ShiftType /*shift*/, Reg /*m*/) {
    if (!options.hook_hint_instructions) {
        return true;
    }

    const auto exception = R ? Exception::PreloadData : Exception::PreloadDataWithIntentToWrite;
    return RaiseException(exception);
}

bool arm_SEV() {
    if (!options.hook_hint_instructions) {
        return true;
    }

    return RaiseException(Exception::SendEvent);
}

bool arm_SEVL() {
    if (!options.hook_hint_instructions) {
        return true;
    }

    return RaiseException(Exception::SendEventLocal);
}

bool arm_WFE() {
    if (!options.hook_hint_instructions) {
        return true;
    }

    return RaiseException(Exception::WaitForEvent);
}

bool arm_WFI() {
    if (!options.hook_hint_instructions) {
        return true;
    }

    return RaiseException(Exception::WaitForInterrupt);
}

bool arm_YIELD() {
    if (!options.hook_hint_instructions) {
        return true;
    }

    return RaiseException(Exception::Yield);
}

// status_register_access
/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

// CPS<effect> <iflags>{, #<mode>}
// CPS #<mode>
bool arm_CPS() { return true; }

// MRS<c> <Rd>, <spec_reg>
bool arm_MRS(Cond cond, Reg d) {
    if (d == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    ir.SetRegister(d, ir.GetCpsr());
    return true;
}

// MSR<c> <spec_reg>, #<const>
bool arm_MSR_imm(Cond cond, unsigned mask, int rotate, Imm<8> imm8) {
    assert(mask != 0);

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const bool write_nzcvq = bits::get_bit<3>(mask);
    const bool write_g = bits::get_bit<2>(mask);
    const bool write_e = bits::get_bit<1>(mask);
    const u32 imm32 = ArmExpandImm(rotate, imm8);
    if (write_e && (imm32 & 0x200)) throw Rejected{BuildError::Unsupported};

    if (write_nzcvq) {
        ir.SetCpsrNZCVQ(ir.Imm32(imm32 & 0xF8000000));
    }

    if (write_g) {
        ir.SetGEFlagsCompressed(ir.Imm32(imm32 & 0x000F0000));
    }

    if (write_e) {
        const bool E = (imm32 & 0x00000200) != 0;
        if (E) throw Rejected{BuildError::Unsupported};
        if (E != ir.current_location.EFlag()) {
            ir.SetTerm(Term::LinkBlock{ir.current_location.AdvancePC(4).SetEFlag(E)});
            return false;
        }
    }

    return true;
}

// MSR<c> <spec_reg>, <Rn>
bool arm_MSR_reg(Cond cond, unsigned mask, Reg n) {
    if (mask == 0) {
        return UnpredictableInstruction();
    }

    if (n == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const bool write_nzcvq = bits::get_bit<3>(mask);
    const bool write_g = bits::get_bit<2>(mask);
    const bool write_e = bits::get_bit<1>(mask);
    const auto value = ir.GetRegister(n);

    if (!write_e) {
        if (write_nzcvq) {
            ir.SetCpsrNZCVQ(ir.And(value, ir.Imm32(0xF8000000)));
        }

        if (write_g) {
            ir.SetGEFlagsCompressed(ir.And(value, ir.Imm32(0x000F0000)));
        }
    } else {
        ir.RequestStateService({.kind = runtime::arm32::StateServiceKind::WriteCPSR,
                                .first_register = static_cast<u8>(n),
                                .offset_words = static_cast<u8>(mask)});
        return false;
    }
    return true;
}

// RFE{<amode>} <Rn>{!}
bool arm_RFE() { return UndefinedInstruction(); }

// SETEND <endian_specifier>
bool arm_SETEND(bool E) {
    if (E) throw Rejected{BuildError::Unsupported};
    ir.SetTerm(Term::LinkBlock{ir.current_location.AdvancePC(4).SetEFlag(E)});
    return false;
}

// SRS{<amode>} SP{!}, #<mode>
bool arm_SRS() { return UndefinedInstruction(); }

// thumb32_control
/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

bool thumb32_BXJ(Reg m) {
    if (m == Reg::PC) {
        return UnpredictableInstruction();
    }

    return thumb16_BX(m);
}

bool thumb32_CLREX() {
    ir.ClearExclusive();
    return true;
}

bool thumb32_DMB(Imm<4> /*option*/) {
    ir.DataMemoryBarrier();
    return true;
}

bool thumb32_DSB(Imm<4> /*option*/) {
    ir.DataSynchronizationBarrier();
    return true;
}

bool thumb32_ISB(Imm<4> /*option*/) {
    ir.InstructionSynchronizationBarrier();
    ir.UpdateUpperLocationDescriptor();
    ir.BranchWritePC(ir.Imm32(ir.current_location.PC() + 4));
    ir.SetTerm(Term::ReturnToDispatch{});
    return false;
}

bool thumb32_NOP() { return thumb16_NOP(); }

bool thumb32_SEV() { return thumb16_SEV(); }

bool thumb32_SEVL() { return thumb16_SEVL(); }

bool thumb32_UDF() { return thumb16_UDF(); }

bool thumb32_WFE() { return thumb16_WFE(); }

bool thumb32_WFI() { return thumb16_WFI(); }

bool thumb32_YIELD() { return thumb16_YIELD(); }

bool thumb32_MSR_reg(bool write_spsr, Reg n, Imm<4> mask) {
    if (mask == 0) {
        return UnpredictableInstruction();
    }

    if (n == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (write_spsr) {
        return UndefinedInstruction();
    }

    const bool write_nzcvq = mask.Bit<3>();
    const bool write_g = mask.Bit<2>();
    const bool write_e = mask.Bit<1>();
    const auto value = ir.GetRegister(n);

    if (!write_e) {
        if (write_nzcvq) {
            ir.SetCpsrNZCVQ(ir.And(value, ir.Imm32(0xF8000000)));
        }

        if (write_g) {
            ir.SetGEFlagsCompressed(ir.And(value, ir.Imm32(0x000F0000)));
        }
    } else {
        ir.RequestStateService({.kind = runtime::arm32::StateServiceKind::WriteCPSR,
                                .first_register = static_cast<u8>(n),
                                .offset_words = mask.ZeroExtend<u8>()});
        return false;
    }
    return true;
}

bool thumb32_MRS_reg(bool read_spsr, Reg d) {
    if (d == Reg::R15) {
        return UnpredictableInstruction();
    }

    // TODO: Revisit when implementing more than user mode.

    if (read_spsr) {
        return UndefinedInstruction();
    }

    ir.SetRegister(d, ir.GetCpsr());
    return true;
}

// a32_exception_generating
/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

// BKPT #<imm16>
bool arm_BKPT(Cond cond, Imm<12> /*imm12*/, Imm<4> /*imm4*/) {
    if (cond != Cond::AL && !options.define_unpredictable_behaviour) {
        return UnpredictableInstruction();
    }
    // UNPREDICTABLE: The instruction executes conditionally.

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    return RaiseException(Exception::Breakpoint);
}

// SVC<c> #<imm24>
bool arm_SVC(Cond cond, Imm<24> imm24) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = imm24.ZeroExtend();
    ir.PushRSB(ir.current_location.AdvancePC(4));
    ir.BranchWritePC(ir.Imm32(ir.current_location.PC() + 4));
    ir.CallSupervisor(ir.Imm32(imm32));
    ir.SetTerm(Term::CheckHalt{Term::PopRSBHint{}});
    return false;
}

// UDF<c> #<imm16>
bool arm_UDF() { return UndefinedInstruction(); }

bool thumb16_SEV() {
    if (!options.hook_hint_instructions) {
        return true;
    }
    return RaiseException(Exception::SendEvent);
}

bool thumb16_SEVL() {
    if (!options.hook_hint_instructions) {
        return true;
    }
    return RaiseException(Exception::SendEventLocal);
}

bool thumb16_WFE() {
    if (!options.hook_hint_instructions) {
        return true;
    }
    return RaiseException(Exception::WaitForEvent);
}

bool thumb16_WFI() {
    if (!options.hook_hint_instructions) {
        return true;
    }
    return RaiseException(Exception::WaitForInterrupt);
}

bool thumb16_YIELD() {
    if (!options.hook_hint_instructions) {
        return true;
    }
    return RaiseException(Exception::Yield);
}

bool thumb16_NOP() { return true; }

bool thumb16_SETEND(bool E) {
    if (E) throw Rejected{BuildError::Unsupported};
    if (ir.current_location.IT().IsInITBlock()) {
        return UnpredictableInstruction();
    }

    if (E == ir.current_location.EFlag()) {
        return true;
    }

    ir.SetTerm(Term::LinkBlock{ir.current_location.AdvancePC(2).SetEFlag(E).AdvanceIT()});
    return false;
}

bool thumb16_CPS(bool, bool, bool, bool) { return true; }

bool thumb16_BKPT(Imm<8> /*imm8*/) { return RaiseException(Exception::Breakpoint); }

bool thumb16_UDF() { return UndefinedInstruction(); }

// User CP15 accesses are architectural state, not a second helper decoder.
bool arm_NOP() { return true; }

bool arm_MRC(Cond cond,
             size_t opc1,
             CoprocReg crn,
             Reg t,
             size_t coprocessor,
             size_t opc2,
             CoprocReg crm) {
    if (cond == Cond::NV || coprocessor != 15 || opc1 != 0 || crn != CoprocReg::C13 ||
        crm != CoprocReg::C0 || (opc2 != 2 && opc2 != 3))
        return UndefinedInstruction();
    const auto value = LoadUniform(
            Uniform{opc2 == 2 ? layout.tpidrurw_offset : layout.tpidruro_offset, Type::U32});
    if (t == Reg::PC)
        SetCpsrNZCVRaw(value);
    else
        SetRegister(t, value);
    return true;
}

bool arm_MCR(Cond cond,
             size_t opc1,
             CoprocReg crn,
             Reg t,
             size_t coprocessor,
             size_t opc2,
             CoprocReg crm) {
    if (cond == Cond::NV) return UndefinedInstruction();
    if (t == Reg::PC) return UnpredictableInstruction();
    if (coprocessor == 15 && opc1 == 0 && crn == CoprocReg::C13 && crm == CoprocReg::C0 &&
        opc2 == 2) {
        StoreUniform(Uniform{layout.tpidrurw_offset, Type::U32}, GetRegister(t));
        return true;
    }
    if (coprocessor == 15 && opc1 == 0 && crn == CoprocReg::C7) {
        if (crm == CoprocReg::C10 && (opc2 == 4 || opc2 == 5)) {
            DataMemoryBarrier();
            return true;
        }
        if (crm == CoprocReg::C5 && opc2 == 4) {
            InstructionSynchronizationBarrier();
            SetTerm(Term::LinkBlock{next_location});
            return false;
        }
    }
    return UndefinedInstruction();
}

bool arm_CDP(Cond, size_t, CoprocReg, CoprocReg, size_t, size_t, CoprocReg) {
    return UndefinedInstruction();
}

bool arm_LDC(Cond, bool, bool, bool, bool, Reg, CoprocReg, size_t, Imm<8>) {
    return UndefinedInstruction();
}

bool arm_STC(Cond, bool, bool, bool, bool, Reg, CoprocReg, size_t, Imm<8>) {
    return UndefinedInstruction();
}

bool arm_MCRR(Cond, Reg, Reg, size_t, size_t, CoprocReg) { return UndefinedInstruction(); }

bool arm_MRRC(Cond, Reg, Reg, size_t, size_t, CoprocReg) { return UndefinedInstruction(); }

bool thumb32_MRC(bool two,
                 size_t opc1,
                 CoprocReg crn,
                 Reg t,
                 size_t coprocessor,
                 size_t opc2,
                 CoprocReg crm) {
    return two ? UndefinedInstruction() : arm_MRC(Cond::AL, opc1, crn, t, coprocessor, opc2, crm);
}

bool thumb32_MCR(bool two,
                 size_t opc1,
                 CoprocReg crn,
                 Reg t,
                 size_t coprocessor,
                 size_t opc2,
                 CoprocReg crm) {
    return two ? UndefinedInstruction() : arm_MCR(Cond::AL, opc1, crn, t, coprocessor, opc2, crm);
}

bool thumb32_CDP(bool, size_t, CoprocReg, CoprocReg, size_t, size_t, CoprocReg) {
    return UndefinedInstruction();
}

bool thumb32_LDC(bool, bool, bool, bool, bool, Reg, CoprocReg, size_t, Imm<8>) {
    return UndefinedInstruction();
}

bool thumb32_STC(bool, bool, bool, bool, bool, Reg, CoprocReg, size_t, Imm<8>) {
    return UndefinedInstruction();
}

bool thumb32_MCRR(bool, Reg, Reg, size_t, size_t, CoprocReg) { return UndefinedInstruction(); }

bool thumb32_MRRC(bool, Reg, Reg, size_t, size_t, CoprocReg) { return UndefinedInstruction(); }
