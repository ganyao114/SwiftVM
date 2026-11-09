void SetLocation(u64 location) {
    if (target.Has(TargetFeature::FaultLocations))
        Emit<OpCode::FaultLocation>(Literal{location});
    else
        StoreUniform(Uniform{layout.pc_offset, Type::U64}, Imm64(location));
}

bool ArmConditionPassed(Cond) { return true; }

bool ThumbConditionPassed() { return true; }

bool VFPConditionPassed(Cond) { return true; }

bool InstructionEnabled(const Instruction& instruction) const {
    const auto opcode = instruction.Code();
    if (opcode == Opcode::Undefined) return false;
    // Generated opcodes retain the ARM/Thumb/VFP/NEON decoder group order.
    const bool vfp = opcode >= Opcode::vfp_VSEL && opcode <= Opcode::vfp_VLDM_a2;
    const bool neon = opcode >= Opcode::asimd_VMOV_imm;
    if ((vfp && !profile.vfp) || (neon && (!profile.neon || !profile.vfp))) return false;
    if (instruction.Thumb() && profile.version < ArchVersion::v4T) return false;
    if (instruction.Thumb() && instruction.Length() == 4 && profile.version < ArchVersion::v6T2 &&
        opcode != Opcode::thumb32_BL_imm && opcode != Opcode::thumb32_BLX_imm)
        return false;
    if (!profile.divide && (opcode == Opcode::arm_SDIV || opcode == Opcode::arm_UDIV ||
                            opcode == Opcode::thumb32_SDIV || opcode == Opcode::thumb32_UDIV))
        return false;
    if ((!profile.crc32 || profile.Version() < 8) &&
        (opcode == Opcode::arm_CRC32 || opcode == Opcode::arm_CRC32C))
        return false;
    switch (opcode) {
        case Opcode::arm_LDA:
        case Opcode::arm_LDAB:
        case Opcode::arm_LDAH:
        case Opcode::arm_LDAEX:
        case Opcode::arm_LDAEXB:
        case Opcode::arm_LDAEXH:
        case Opcode::arm_LDAEXD:
        case Opcode::arm_STL:
        case Opcode::arm_STLB:
        case Opcode::arm_STLH:
        case Opcode::arm_STLEX:
        case Opcode::arm_STLEXB:
        case Opcode::arm_STLEXH:
        case Opcode::arm_STLEXD:
        case Opcode::thumb32_LDA:
        case Opcode::thumb32_STL:
        case Opcode::arm_SEVL:
        case Opcode::thumb16_SEVL:
        case Opcode::thumb32_SEVL:
            return profile.Version() >= 8;
        case Opcode::v8_AESE:
        case Opcode::v8_AESD:
        case Opcode::v8_AESMC:
        case Opcode::v8_AESIMC:
        case Opcode::v8_SHA256H:
        case Opcode::v8_SHA256H2:
        case Opcode::v8_SHA256SU0:
        case Opcode::v8_SHA256SU1:
            return profile.Version() >= 8 && profile.crypto;
        case Opcode::vfp_VSEL:
        case Opcode::vfp_VMAXNM:
        case Opcode::vfp_VMINNM:
        case Opcode::vfp_VRINT_rm:
        case Opcode::vfp_VCVT_rm:
        case Opcode::vfp_VRINTR:
        case Opcode::vfp_VRINTZ:
        case Opcode::vfp_VRINTX:
        case Opcode::v8_VRINTN:
        case Opcode::v8_VRINTX:
        case Opcode::v8_VRINTA:
        case Opcode::v8_VRINTZ:
        case Opcode::v8_VRINTM:
        case Opcode::v8_VRINTP:
        case Opcode::v8_VCVTA:
        case Opcode::v8_VCVTN:
        case Opcode::v8_VCVTP:
        case Opcode::v8_VCVTM:
        case Opcode::v8_VMAXNM:
        case Opcode::v8_VMINNM:
            return profile.Version() >= 8;
        case Opcode::vfp_VFNMS:
        case Opcode::vfp_VFNMA:
        case Opcode::vfp_VFMA:
        case Opcode::vfp_VFMS:
        case Opcode::asimd_VFMA:
        case Opcode::asimd_VFMS:
            return profile.vfpv4;
        case Opcode::vfp_VCVTB:
        case Opcode::vfp_VCVTT:
            return profile.fp16;
        default:
            break;
    }
    return true;
}

void BranchWritePC(Value address) {
    const auto context = next_location.Context();
    if (address.known) {
        control.destination = Literal{
                Location{u64(context) << 32}
                        .WithPC(u32(address.literal) & (current_location.Thumb() ? ~1u : ~3u))
                        .Key()};
        return;
    }
    const auto target_context = Imm64(u64(context) << 32);
    const auto aligned =
            And(address.WithView(Type::U32), Literal{current_location.Thumb() ? ~1u : ~3u});
    control.destination = Or(ZeroExtendWordToLong(aligned), target_context);
}

void BXWritePC(Value address) {
    if (address.known) {
        const bool thumb = address.literal & 1;
        control.destination = Literal{current_location.SetTFlag(thumb)
                                              .SetIT(ITState{})
                                              .WithPC(u32(address.literal) & (thumb ? ~1u : ~3u))
                                              .Key()};
        return;
    }
    const auto thumb = TestBit(address, Literal{0u});
    const auto aligned = Select(thumb,
                                And(address.WithView(Type::U32), Literal{~1u}),
                                And(address.WithView(Type::U32), Literal{~3u}));
    auto context =
            Imm64(current_location.SetTFlag(false).SetIT(ITState{}).Key() & ~UINT64_C(0xffffffff));
    context = Or(context, LslImm(thumb.WithView(Type::U64), Literal{32u}));
    control.destination = Or(ZeroExtendWordToLong(aligned), context);
}

void ALUWritePC(Value value) {
    if (profile.Version() >= 7 && !current_location.Thumb())
        BXWritePC(value);
    else
        BranchWritePC(value);
}

void LoadWritePC(Value value) {
    if (profile.Version() >= 5)
        BXWritePC(value);
    else
        BranchWritePC(value);
}

void UpdateUpperLocationDescriptor() {}

void PushRSB(Location) {}

void SetCheckBit(Value value) { check_bit = value; }

void SetTerm(Term::LinkBlock term) {
    control.kind = Control::Kind::Jump;
    control.yes = term.location.Key();
    end_decode_ = true;
}

void SetTerm(Term::ReturnToDispatch) {
    if (control.kind != Control::Kind::Service) control.kind = Control::Kind::Dispatch;
    end_decode_ = true;
}

void SetTerm(Term::PopRSBHint) { SetTerm(Term::ReturnToDispatch{}); }

void SetTerm(Term::FastDispatchHint) { SetTerm(Term::ReturnToDispatch{}); }

void SetTerm(Term::Interpret) { UndefinedInstruction(); }

template <class T> void SetTerm(Term::CheckHalt<T> term) { SetTerm(term.next); }

void SetTerm(Term::If<Term::LinkBlock, Term::LinkBlock> term) {
    control.kind = Control::Kind::Conditional;
    control.condition = CondPassed(term.condition);
    control.yes = term.yes.location.Key();
    control.no = term.no.location.Key();
    end_decode_ = true;
}

void SetTerm(Term::CheckBit<Term::LinkBlock, Term::LinkBlock> term) {
    control.kind = Control::Kind::Conditional;
    control.condition = check_bit;
    control.yes = term.yes.location.Key();
    control.no = term.no.location.Key();
    end_decode_ = true;
}

void CallSupervisor(Value immediate) {
    StoreUniform(Uniform{layout.service_word_offset, Type::U32}, immediate.WithView(Type::U32));
    StoreUniform(Uniform{layout.service_pc_offset, Type::U64}, Imm64(current_pc));
    control.kind = Control::Kind::Service;
    control.yes = next_location.Key();
    control.service = runtime::ExitValue(runtime::NativeExit::Syscall);
    end_decode_ = true;
}

bool RaiseException(Exception exception) {
    const auto kind = exception == Exception::Breakpoint
                              ? runtime::GuestExceptionKind::Breakpoint
                              : runtime::GuestExceptionKind::IllegalInstruction;
    StoreUniform(Uniform{layout.service_pc_offset, Type::U64}, Imm64(current_pc));
    control.kind = Control::Kind::Service;
    control.yes = current_pc;
    control.service = runtime::SemanticExceptionExit(kind, current_instruction_size);
    end_decode_ = true;
    return false;
}

void ExceptionRaised(Exception exception) { RaiseException(exception); }

bool UndefinedInstruction() { return RaiseException(Exception::UndefinedInstruction); }

bool UnpredictableInstruction() { return RaiseException(Exception::UnpredictableInstruction); }

bool InterpretThisInstruction() { throw Rejected{BuildError::Unsupported}; }

bool DecodeError() { return RaiseException(Exception::DecodeError); }

// Imported A32 instruction semantics.
// Adapted from dynarmic-interp 5a76f693 (MerryMage and contributors), SPDX-License-Identifier:
// 0BSD.

// a32_branch
/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

// B <label>
bool arm_B(Cond cond, Imm<24> imm24) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = bits::sign_extend<26, u32>(imm24.ZeroExtend() << 2) + 8;
    const auto new_location = ir.current_location.AdvancePC(imm32);
    ir.SetTerm(Term::LinkBlock{new_location});
    return false;
}

// BL <label>
bool arm_BL(Cond cond, Imm<24> imm24) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    ir.PushRSB(ir.current_location.AdvancePC(4));
    ir.SetRegister(Reg::LR, ir.Imm32(ir.current_location.PC() + 4));

    const u32 imm32 = bits::sign_extend<26, u32>(imm24.ZeroExtend() << 2) + 8;
    const auto new_location = ir.current_location.AdvancePC(imm32);
    ir.SetTerm(Term::LinkBlock{new_location});
    return false;
}

// BLX <label>
bool arm_BLX_imm(bool H, Imm<24> imm24) {
    ir.PushRSB(ir.current_location.AdvancePC(4));
    ir.SetRegister(Reg::LR, ir.Imm32(ir.current_location.PC() + 4));

    const u32 imm32 = bits::sign_extend<26, u32>((imm24.ZeroExtend() << 2)) + (H ? 2 : 0) + 8;
    const auto new_location = ir.current_location.AdvancePC(imm32).SetTFlag(true);
    ir.SetTerm(Term::LinkBlock{new_location});
    return false;
}

// BLX <Rm>
bool arm_BLX_reg(Cond cond, Reg m) {
    if (m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    ir.PushRSB(ir.current_location.AdvancePC(4));
    ir.BXWritePC(ir.GetRegister(m));
    ir.SetRegister(Reg::LR, ir.Imm32(ir.current_location.PC() + 4));
    ir.SetTerm(Term::FastDispatchHint{});
    return false;
}

// BX <Rm>
bool arm_BX(Cond cond, Reg m) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    ir.BXWritePC(ir.GetRegister(m));
    if (m == Reg::R14) {
        ir.SetTerm(Term::PopRSBHint{});
    } else {
        ir.SetTerm(Term::FastDispatchHint{});
    }

    return false;
}

bool arm_BXJ(Cond cond, Reg m) {
    // Jazelle not supported
    return arm_BX(cond, m);
}

// thumb32_branch
/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

// BL <label>
bool thumb32_BL_imm(Imm<1> S, Imm<10> hi, Imm<1> j1, Imm<1> j2, Imm<11> lo) {
    const Imm<1> i1{j1 == S};
    const Imm<1> i2{j2 == S};

    if (ir.current_location.IT().IsInITBlock() && !ir.current_location.IT().IsLastInITBlock()) {
        return UnpredictableInstruction();
    }

    ir.PushRSB(ir.current_location.AdvancePC(4).AdvanceIT());
    ir.SetRegister(Reg::LR, ir.Imm32((ir.current_location.PC() + 4) | 1));

    const s32 imm32 = static_cast<s32>((concatenate(S, i1, i2, hi, lo).SignExtend<u32>() << 1) + 4);
    const auto new_location = ir.current_location.AdvancePC(imm32).AdvanceIT();
    ir.SetTerm(Term::LinkBlock{new_location});
    return false;
}

// BLX <label>
bool thumb32_BLX_imm(Imm<1> S, Imm<10> hi, Imm<1> j1, Imm<1> j2, Imm<11> lo) {
    const Imm<1> i1{j1 == S};
    const Imm<1> i2{j2 == S};

    if (ir.current_location.IT().IsInITBlock() && !ir.current_location.IT().IsLastInITBlock()) {
        return UnpredictableInstruction();
    }

    if (lo.Bit<0>()) {
        return UnpredictableInstruction();
    }

    ir.PushRSB(ir.current_location.AdvancePC(4).AdvanceIT());
    ir.SetRegister(Reg::LR, ir.Imm32((ir.current_location.PC() + 4) | 1));

    const s32 imm32 = static_cast<s32>(concatenate(S, i1, i2, hi, lo).SignExtend<u32>() << 1);
    const auto new_location =
            ir.current_location.SetPC(ir.AlignPC(4) + imm32).SetTFlag(false).AdvanceIT();
    ir.SetTerm(Term::LinkBlock{new_location});
    return false;
}

bool thumb32_B(Imm<1> S, Imm<10> hi, Imm<1> j1, Imm<1> j2, Imm<11> lo) {
    const Imm<1> i1{j1 == S};
    const Imm<1> i2{j2 == S};

    if (ir.current_location.IT().IsInITBlock() && !ir.current_location.IT().IsLastInITBlock()) {
        return UnpredictableInstruction();
    }

    const s32 imm32 = static_cast<s32>((concatenate(S, i1, i2, hi, lo).SignExtend<u32>() << 1) + 4);
    const auto new_location = ir.current_location.AdvancePC(imm32).AdvanceIT();
    ir.SetTerm(Term::LinkBlock{new_location});
    return false;
}

bool thumb32_B_cond(Imm<1> S, Cond cond, Imm<6> hi, Imm<1> i1, Imm<1> i2, Imm<11> lo) {
    if (ir.current_location.IT().IsInITBlock()) {
        return UnpredictableInstruction();
    }

    // Note: i1 and i2 were not inverted from encoding and are opposite compared to the other B
    // instructions.
    const s32 imm32 = static_cast<s32>((concatenate(S, i2, i1, hi, lo).SignExtend<u32>() << 1) + 4);
    const auto then_location = ir.current_location.AdvancePC(imm32).AdvanceIT();
    const auto else_location = ir.current_location.AdvancePC(4).AdvanceIT();
    ir.SetTerm(Term::If{cond, Term::LinkBlock{then_location}, Term::LinkBlock{else_location}});
    return false;
}

bool thumb16_IT(Imm<8> imm8) {
    assert((imm8.Bits<0, 3>() != 0b0000));
    if (imm8.Bits<4, 7>() == 0b1111 ||
        (imm8.Bits<4, 7>() == 0b1110 && bits::count_ones(imm8.Bits<0, 3>()) != 1)) {
        return UnpredictableInstruction();
    }
    if (ir.current_location.IT().IsInITBlock()) {
        return UnpredictableInstruction();
    }

    const auto next_location =
            ir.current_location.AdvancePC(2).SetIT(ITState{imm8.ZeroExtend<u8>()});
    ir.SetTerm(Term::LinkBlockFast{next_location});
    return false;
}

bool thumb16_CBZ_CBNZ(bool nonzero, Imm<1> i, Imm<5> imm5, Reg n) {
    if (ir.current_location.IT().IsInITBlock()) {
        return UnpredictableInstruction();
    }

    const u32 imm = concatenate(i, imm5, Imm<1>{0}).ZeroExtend();
    const Value rn = ir.GetRegister(n);

    ir.SetCheckBit(ir.IsZero(rn));

    const auto [cond_pass, cond_fail] = [this, imm, nonzero] {
        const auto skip = Term::LinkBlock{ir.current_location.AdvancePC(2).AdvanceIT()};
        const auto branch = Term::LinkBlock{ir.current_location.AdvancePC(imm + 4).AdvanceIT()};

        if (nonzero) {
            return std::make_pair(skip, branch);
        } else {
            return std::make_pair(branch, skip);
        }
    }();

    ir.SetTerm(Term::CheckBit{cond_pass, cond_fail});
    return false;
}

bool thumb16_BX(Reg m) {
    if (ir.current_location.IT().IsInITBlock() && !ir.current_location.IT().IsLastInITBlock()) {
        return UnpredictableInstruction();
    }

    ir.UpdateUpperLocationDescriptor();
    ir.BXWritePC(ir.GetRegister(m));
    if (m == Reg::R14)
        ir.SetTerm(Term::PopRSBHint{});
    else
        ir.SetTerm(Term::FastDispatchHint{});
    return false;
}

bool thumb16_BLX_reg(Reg m) {
    if (ir.current_location.IT().IsInITBlock() && !ir.current_location.IT().IsLastInITBlock()) {
        return UnpredictableInstruction();
    }

    ir.PushRSB(ir.current_location.AdvancePC(2).AdvanceIT());
    ir.UpdateUpperLocationDescriptor();
    ir.BXWritePC(ir.GetRegister(m));
    ir.SetRegister(Reg::LR, ir.Imm32((ir.current_location.PC() + 2) | 1));
    ir.SetTerm(Term::FastDispatchHint{});
    return false;
}

bool thumb16_SVC(Imm<8> imm8) {
    const u32 imm32 = imm8.ZeroExtend();
    ir.PushRSB(ir.current_location.AdvancePC(2).AdvanceIT());
    ir.UpdateUpperLocationDescriptor();
    ir.BranchWritePC(ir.Imm32(ir.current_location.PC() + 2));
    ir.CallSupervisor(ir.Imm32(imm32));
    ir.SetTerm(Term::CheckHalt{Term::PopRSBHint{}});
    return false;
}

bool thumb16_B_t1(Cond cond, Imm<8> imm8) {
    if (ir.current_location.IT().IsInITBlock()) {
        return UnpredictableInstruction();
    }

    if (cond == Cond::AL) {
        return thumb16_UDF();
    }

    const s32 imm32 = static_cast<s32>((imm8.SignExtend<u32>() << 1) + 4);
    const auto then_location = ir.current_location.AdvancePC(imm32).AdvanceIT();
    const auto else_location = ir.current_location.AdvancePC(2).AdvanceIT();

    ir.SetTerm(Term::If{cond, Term::LinkBlock{then_location}, Term::LinkBlock{else_location}});
    return false;
}

bool thumb16_B_t2(Imm<11> imm11) {
    if (ir.current_location.IT().IsInITBlock() && !ir.current_location.IT().IsLastInITBlock()) {
        return UnpredictableInstruction();
    }

    const s32 imm32 = static_cast<s32>((imm11.SignExtend<u32>() << 1) + 4);
    const auto next_location = ir.current_location.AdvancePC(imm32).AdvanceIT();

    ir.SetTerm(Term::LinkBlock{next_location});
    return false;
}
