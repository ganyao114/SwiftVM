void VisitSystem(const vixl::aarch64::Instruction* instr) override {
    const auto request = StateRequest(instr->GetInstructionBits());
    if (request.kind != runtime::arm64::StateServiceKind::None) {
        RequestStateService(request);
        return;
    }
    if (instr->Mask(SystemHintFMask) == SystemHintFixed) {
        // NOP, YIELD, WFE, WFI, SEV(L), BTI, ... are all treated as NOPs.
        // These hints require no IR operation in the current execution profile.
        return;
    }

    if (instr->Mask(MemBarrierFMask) == MemBarrierFixed) {
        switch (instr->Mask(MemBarrierMask)) {
            case DMB:
            case DSB:
                this->MemoryBarrierTSO();
                break;
            case ISB:
                // End the decoded block so subsequent instructions are fetched
                // after the synchronization point and any SMC invalidation.
                this->MemoryBarrierTSO();
                WritePC(Input{Imm{NextPC()}});
                this->ReturnToDispatcher();
                end_decode_ = true;
                break;
            default:
                Interrupt(InterruptReason::FALLBACK, current_pc);
                break;
        }
        return;
    }

    if (instr->Mask(SystemSysRegFMask) == SystemSysRegFixed) {
        auto sysreg = instr->GetImmSystemRegister();
        bool is_read = instr->Mask(SystemSysRegMask) == MRS;
        auto rt = instr->GetRt();
        switch (sysreg) {
            case NZCV: {
                if (is_read) {
                    auto value = ImmValue(0, Type::U64);
                    const Flag flags[] = {Flag::Overflow, Flag::Carry, Flag::Zero, Flag::Negate};
                    for (u32 i = 0; i < 4; ++i) {
                        auto bit = this->TestFlags(flags[i]).WithView(Type::U8);
                        auto shifted = this->template LslImm<Type::U64>(Widen(bit), Imm{28u + i})
                                               .WithView(Type::U64);
                        value = this->template Or<Type::U64>(value, SingleOperand(shifted))
                                        .WithView(Type::U64);
                    }
                    WriteXRegister(rt, value);
                } else {
                    auto source = ReadXRegister(rt);
                    auto bit = [&](u32 position) {
                        auto shifted = this->template LsrImm<Type::U64>(source, Imm{position})
                                               .WithView(Type::U64);
                        return this->template And<Type::U64>(shifted, SingleOperand(Imm{u64{1}}))
                                .WithView(Type::U64);
                    };
                    // N and Z can be set independently, including N=Z=1.
                    auto n =
                            this->template LslImm<Type::U64>(bit(31), Imm{63u}).WithView(Type::U64);
                    auto z = this->template Xor<Type::U64>(bit(30), SingleOperand(Imm{u64{1}}))
                                     .WithView(Type::U64);
                    n = this->template Sub<Type::U64>(ImmValue(0, Type::U64), SingleOperand(n))
                                .WithView(Type::U64);
                    z = this->template Sub<Type::U64>(z, SingleOperand(Imm{u64{0}}))
                                .WithView(Type::U64);
                    this->SaveFlags(n, Flag::Negate);
                    this->SaveFlags(z, Flag::Zero);
                    this->SetCarry(bit(29));
                    this->SetOverflow(bit(28));
                }
                return;
            }
            case TPIDR_EL0:
            case TPIDRRO_EL0: {
                Uniform uni{u32(offsetof(GuestState, tpidr_el0)), Type::U64};
                if (is_read) {
                    WriteXRegister(rt, this->template LoadUniform<Type::U64>(uni));
                } else {
                    this->StoreUniform(uni, ReadXRegister(rt));
                }
                return;
            }
            case FPCR: {
                Uniform uni{u32(offsetof(GuestState, fpcr)), Type::U32};
                if (is_read) {
                    WriteWRegister(rt, this->template LoadUniform<Type::U32>(uni));
                } else {
                    this->StoreUniform(uni, ReadRegister(rt, Type::U32));
                    control.kind = Control::Kind::Service;
                    control.yes = NextPC();
                    control.service = runtime::ExitValue(runtime::NativeExit::FPEnvironment);
                    end_decode_ = true;
                }
                return;
            }
            case FPSR: {
                Uniform uni{u32(offsetof(GuestState, fpsr)), Type::U32};
                if (is_read) {
                    WriteWRegister(rt, this->template LoadUniform<Type::U32>(uni));
                } else {
                    this->StoreUniform(uni, ReadRegister(rt, Type::U32));
                }
                return;
            }
            case SystemRegisterEncoder<3, 3, 0, 0, 7>::value:
                if (is_read) {
                    // DZP=1: DC ZVA is prohibited. Returning zero would
                    // advertise a supported four-byte zeroing block.
                    WriteXRegister(rt, ImmValue(1u << 4, Type::U64));
                }
                return;
            default:
                Interrupt(InterruptReason::FALLBACK, current_pc);
                return;
        }
    }

    Interrupt(InterruptReason::FALLBACK, current_pc);
}

void VisitException(const vixl::aarch64::Instruction* instr) override {
    switch (instr->Mask(ExceptionMask)) {
        case SVC:
            // System call: hand over to the host, resuming at the next
            // instruction once the syscall has been serviced. The syscall
            // number / arguments are read from the guest context (x8, x0-x5).
            Interrupt(InterruptReason::SVC, NextPC());
            break;
        case BRK:
            Interrupt(InterruptReason::BRK, current_pc);
            break;
        case HLT:
            Interrupt(InterruptReason::HLT, current_pc);
            break;
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            break;
    }
}

void VisitUnconditionalBranch(const vixl::aarch64::Instruction* instr) override {
    const auto destination = current_pc + s64(instr->GetImmUncondBranch()) * 4;
    if (instr->Mask(UnconditionalBranchMask) == BL) {
        WriteXRegister(30, ImmValue(NextPC(), Type::U64));
        WritePC(Input{Imm{destination}});
        ReturnToDispatcher();
        return;
    }
    BranchImm(destination);
}

void VisitConditionalBranch(const vixl::aarch64::Instruction* instr) override {
    BranchChoice(CondPassed(static_cast<vixl::aarch64::Condition>(instr->GetConditionBranch())),
                 current_pc + s64(instr->GetImmCondBranch()) * 4,
                 NextPC());
}

void VisitCompareBranch(const vixl::aarch64::Instruction* instr) override {
    auto value = ReadRegister(instr->GetRt(), GPRType(instr->GetSixtyFourBits()));
    auto condition =
            (instr->GetInstructionBits() & (1u << 24)) ? TestNotZero(value) : TestZero(value);
    BranchChoice(condition, current_pc + s64(instr->GetImmCmpBranch()) * 4, NextPC());
}

void VisitTestBranch(const vixl::aarch64::Instruction* instr) override {
    const auto bit = (instr->GetImmTestBranchBit5() << 5) | instr->GetImmTestBranchBit40();
    auto condition = TestBit(ReadXRegister(instr->GetRt()), Imm{bit});
    if (X64FP() && instr->Mask(TestBranchMask) == TBZ) {
        BranchChoice(condition, NextPC(), current_pc + s64(instr->GetImmTestBranch()) * 4);
        return;
    }
    if (instr->Mask(TestBranchMask) == TBZ) condition = TestZero(condition);
    BranchChoice(condition, current_pc + s64(instr->GetImmTestBranch()) * 4, NextPC());
}

void VisitUnconditionalBranchToRegister(const vixl::aarch64::Instruction* instr) override {
    const auto opcode = instr->Mask(UnconditionalBranchToRegisterMask);
    if (opcode != RET && opcode != BR && opcode != BLR) throw Rejected{BuildError::Unsupported};
    auto destination = ReadXRegister(instr->GetRn());
    if (opcode == BLR) WriteXRegister(30, ImmValue(NextPC(), Type::U64));
    WritePC(Input{destination});
    ReturnToDispatcher();
}

void VisitPCRelAddressing(const vixl::aarch64::Instruction* instr) override {
    VAddr address;
    if (instr->Mask(PCRelAddressingMask) == ADRP) {
        address = (current_pc & ~VAddr(0xFFF)) + s64(instr->GetImmPCRel()) * s64(kPageSize);
    } else {
        VIXL_ASSERT(instr->Mask(PCRelAddressingMask) == ADR);
        address = current_pc + s64(instr->GetImmPCRel());
    }
    WriteXRegister(instr->GetRd(),
                   this->template LoadImm<Type::U64>(Imm{address}).WithView(Type::U64));
}
