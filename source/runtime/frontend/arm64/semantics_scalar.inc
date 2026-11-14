void AddSubHelper(const vixl::aarch64::Instruction* instr, const Input& op2) {
    const bool is64 = instr->GetSixtyFourBits();
    const auto operation = instr->Mask(AddSubOpMask);
    const bool is_sub = (operation == SUB) || (operation == SUBS);
    const bool set_flags = (operation == ADDS) || (operation == SUBS);

    auto rn = ReadRegister(instr->GetRn(), GPRType(is64), instr->GetRnMode());
    auto result = is_sub ? EmitGPR<OpCode::Sub>(is64, rn, SingleOperand(op2))
                         : EmitGPR<OpCode::Add>(is64, rn, SingleOperand(op2));

    if (set_flags) {
        // The neutral subtraction carries borrow. SaveFlags converts that
        // single output to ARM C = no-borrow; N/Z/V remain neutral.
        this->SaveFlags(result, Flag::NZCV);
    }
    WriteGPR(instr->GetRd(), is64, result, instr->GetRdMode());
}

void VisitAddSubImmediate(const vixl::aarch64::Instruction* instr) override {
    u32 imm = instr->GetImmAddSub();
    if (instr->GetShiftAddSub() == 1) {
        imm <<= 12;
    }
    AddSubHelper(instr, Input{Imm{imm}});
}

void VisitAddSubShifted(const vixl::aarch64::Instruction* instr) override {
    bool is64 = instr->GetSixtyFourBits();
    auto rm = ReadRegister(instr->GetRm(), GPRType(is64));
    auto op2 = ShiftOperand(
            is64, rm, static_cast<Shift>(instr->GetShiftDP()), u32(instr->GetImmDPShift()));
    AddSubHelper(instr, Input{op2});
}

void VisitAddSubExtended(const vixl::aarch64::Instruction* instr) override {
    auto rm = ReadXRegister(instr->GetRm());
    auto op2 = ExtendOperand(
            rm, static_cast<Extend>(instr->GetExtendMode()), u32(instr->GetImmExtendShift()));
    AddSubHelper(instr, Input{op2});
}

void VisitAddSubWithCarry(const vixl::aarch64::Instruction* instr) override {
    const bool is64 = instr->GetSixtyFourBits();
    const auto op = instr->Mask(AddSubWithCarryMask);
    const bool is_sub = (op == SBC_w) || (op == SBC_x) || (op == SBCS_w) || (op == SBCS_x);
    const bool set_flags = (op == ADCS_w) || (op == ADCS_x) || (op == SBCS_w) || (op == SBCS_x);

    auto rn = ReadRegister(instr->GetRn(), GPRType(is64));
    auto rm = ReadRegister(instr->GetRm(), GPRType(is64));

    // SBC = a + ~b + C, so both ADC and SBC map onto Adc with the AArch64
    // carry flag (C = NOT borrow) consumed directly.
    Value operand2 = is_sub ? EmitGPR<OpCode::Xor>(is64, rm, SingleOperand(ImmGPR(is64, ~u64{0})))
                            : rm;
    Value result = EmitGPR<OpCode::Adc>(is64, rn, SingleOperand(operand2));
    if (set_flags) this->SaveFlags(result, Flag::NZCV);
    WriteGPR(instr->GetRd(), is64, result);
}

void VisitLogicalImmediate(const vixl::aarch64::Instruction* instr) override {
    const bool is64 = instr->GetSixtyFourBits();
    const Input op2{ImmGPR(is64, instr->GetImmLogical())};
    auto rn = ReadRegister(instr->GetRn(), GPRType(is64));

    Value result{};
    bool set_flags = false;
    switch (instr->Mask(LogicalImmediateMask)) {
        case AND_w_imm:
        case AND_x_imm:
            result = EmitGPR<OpCode::And>(is64, rn, SingleOperand(op2));
            break;
        case ORR_w_imm:
        case ORR_x_imm:
            result = EmitGPR<OpCode::Or>(is64, rn, SingleOperand(op2));
            break;
        case EOR_w_imm:
        case EOR_x_imm:
            result = EmitGPR<OpCode::Xor>(is64, rn, SingleOperand(op2));
            break;
        case ANDS_w_imm:
        case ANDS_x_imm:
            result = EmitGPR<OpCode::And>(is64, rn, SingleOperand(op2));
            set_flags = true;
            break;
        default:
            VIXL_UNREACHABLE();
    }

    if (set_flags) {
        // Host ANDS also yields C = V = 0, matching the AArch64 semantics.
        this->SaveFlags(result, Flag::NZCV);
    }
    WriteGPR(instr->GetRd(), is64, result, instr->GetRdMode());
}

void VisitLogicalShifted(const vixl::aarch64::Instruction* instr) override {
    const bool is64 = instr->GetSixtyFourBits();
    const auto type = GPRType(is64);

    auto rn = ReadRegister(instr->GetRn(), type);
    auto op2 = ShiftOperand(is64,
                            ReadRegister(instr->GetRm(), type),
                            static_cast<Shift>(instr->GetShiftDP()),
                            u32(instr->GetImmDPShift()));

    Value result{};
    bool set_flags = false;
    auto invert = [&](Value value) {
        return EmitGPR<OpCode::Xor>(is64, value, SingleOperand(ImmGPR(is64, ~u64{0})));
    };

    switch (instr->Mask(LogicalShiftedMask)) {
        case AND_w:
        case AND_x:
            result = EmitGPR<OpCode::And>(is64, rn, SingleOperand(op2));
            break;
        case BIC_w:
        case BIC_x:
            result = EmitGPR<OpCode::AndNot>(is64, rn, SingleOperand(op2));
            break;
        case ORR_w:
        case ORR_x:
            result = EmitGPR<OpCode::Or>(is64, rn, SingleOperand(op2));
            break;
        case ORN_w:
        case ORN_x:
            result = EmitGPR<OpCode::Or>(is64, rn, SingleOperand(invert(op2)));
            break;
        case EOR_w:
        case EOR_x:
            result = EmitGPR<OpCode::Xor>(is64, rn, SingleOperand(op2));
            break;
        case EON_w:
        case EON_x:
            result = EmitGPR<OpCode::Xor>(is64, rn, SingleOperand(invert(op2)));
            break;
        case ANDS_w:
        case ANDS_x:
            result = EmitGPR<OpCode::And>(is64, rn, SingleOperand(op2));
            set_flags = true;
            break;
        case BICS_w:
        case BICS_x:
            result = EmitGPR<OpCode::AndNot>(is64, rn, SingleOperand(op2));
            set_flags = true;
            break;
        default:
            VIXL_UNREACHABLE();
    }

    if (set_flags) this->SaveFlags(result, Flag::NZCV);
    WriteGPR(instr->GetRd(), is64, result);
}

void VisitMoveWideImmediate(const vixl::aarch64::Instruction* instr) override {
    const auto mov_op = static_cast<MoveWideImmediateOp>(instr->Mask(MoveWideImmediateMask));
    const bool is64 = instr->GetSixtyFourBits() == 1;
    // Shift is limited for W operations.
    VIXL_ASSERT(is64 || (instr->GetShiftMoveWide() < 2));

    const auto type = GPRType(is64);
    const u32 shift = instr->GetShiftMoveWide() * 16;
    const u64 shifted_imm16 = u64(instr->GetImmMoveWide()) << shift;

    Value new_val{};
    switch (mov_op) {
        case MOVN_w:
        case MOVN_x: {
            u64 val = ~shifted_imm16;
            if (!is64) val &= kWRegMask;
            new_val = ImmValue(val, type);
            break;
        }
        case MOVK_w:
        case MOVK_x: {
            auto prev = ReadRegister(instr->GetRd(), type);
            auto imm16 = this->template LoadImm<Type::U16>(Imm{u16(instr->GetImmMoveWide())})
                                 .WithView(Type::U16);
            new_val = EmitGPR<OpCode::BitInsert>(is64, prev, imm16, Imm{u8(shift)}, Imm{u8(16)});
            break;
        }
        case MOVZ_w:
        case MOVZ_x:
            new_val = ImmValue(shifted_imm16, type);
            break;
        default:
            VIXL_UNREACHABLE();
    }

    WriteGPR(instr->GetRd(), is64, new_val);
}

Value EmitUnsignedBitfield(bool is64, u32 reg_size, u32 R, u32 S, Value rn) {
    if (S >= R) {
        if (R == 0 && S == reg_size - 1) return rn;
        if (S == reg_size - 1) return EmitGPR<OpCode::LsrImm>(is64, rn, Imm{R});
        const u64 fmask = (UINT64_C(1) << (S - R + 1)) - 1;
        auto shifted = R ? EmitGPR<OpCode::LsrImm>(is64, rn, Imm{R}) : rn;
        return EmitGPR<OpCode::And>(is64, shifted, SingleOperand(ImmGPR(is64, fmask)));
    }
    const u32 shift = reg_size - R;
    if (S + 1 == R) return EmitGPR<OpCode::LslImm>(is64, rn, Imm{shift});
    const u64 fmask = (UINT64_C(1) << (S + 1)) - 1;
    auto masked = EmitGPR<OpCode::And>(is64, rn, SingleOperand(ImmGPR(is64, fmask)));
    return EmitGPR<OpCode::LslImm>(is64, masked, Imm{shift});
}

Value EmitSignedBitfield(bool is64, u32 reg_size, u32 R, u32 S, Value rn) {
    if (S >= R) {
        if (R == 0 && S == reg_size - 1) return rn;
        if (S == reg_size - 1) return EmitGPR<OpCode::AsrImm>(is64, rn, Imm{R});
        if (R == 0 && (S == 7 || S == 15 || S == 31)) {
            return EmitGPR<OpCode::SignExtend>(is64, rn.WithView(ScalarType(S + 1)));
        }
        const u32 lsh = reg_size - 1 - S;
        const u32 rsh = reg_size - (S - R + 1);
        auto shifted = lsh ? EmitGPR<OpCode::LslImm>(is64, rn, Imm{lsh}) : rn;
        return EmitGPR<OpCode::AsrImm>(is64, shifted, Imm{rsh});
    }
    const u32 shift = reg_size - R;
    if (S + 1 == R) return EmitGPR<OpCode::LslImm>(is64, rn, Imm{shift});
    const u32 lsh = reg_size - 1 - S;
    const u32 rsh = R - 1 - S;
    return EmitGPR<OpCode::AsrImm>(is64, EmitGPR<OpCode::LslImm>(is64, rn, Imm{lsh}), Imm{rsh});
}

Value EmitBitfieldInsert(const vixl::aarch64::Instruction* instr,
                         bool is64,
                         u32 reg_size,
                         u64 reg_mask,
                         u32 R,
                         u32 S) {
    const auto type = GPRType(is64);
    if (S >= R && R == 0 && S == reg_size - 1) return ReadRegister(instr->GetRn(), type);
    auto rd = ReadRegister(instr->GetRd(), type);
    if (S >= R) {
        const u32 width = S - R + 1;
        const u64 fmask = (UINT64_C(1) << width) - 1;
        auto kept = EmitGPR<OpCode::And>(is64, rd, SingleOperand(ImmGPR(is64, ~fmask & reg_mask)));
        if (instr->GetRn() == kZeroRegCode) return kept;
        auto rn = ReadRegister(instr->GetRn(), type);
        auto src = R ? EmitGPR<OpCode::LsrImm>(is64, rn, Imm{R}) : rn;
        if (S < reg_size - 1) {
            src = EmitGPR<OpCode::And>(is64, src, SingleOperand(ImmGPR(is64, fmask)));
        }
        return EmitGPR<OpCode::Or>(is64, kept, SingleOperand(src));
    }
    const u32 shift = reg_size - R;
    const u32 width = S + 1;
    const u64 fmask = ((UINT64_C(1) << width) - 1) << shift;
    auto kept = EmitGPR<OpCode::And>(is64, rd, SingleOperand(ImmGPR(is64, ~fmask & reg_mask)));
    if (instr->GetRn() == kZeroRegCode) return kept;
    auto rn = ReadRegister(instr->GetRn(), type);
    auto src = EmitGPR<OpCode::LslImm>(is64, rn, Imm{shift});
    if (S + 1 < R) src = EmitGPR<OpCode::And>(is64, src, SingleOperand(ImmGPR(is64, fmask)));
    return EmitGPR<OpCode::Or>(is64, kept, SingleOperand(src));
}

void VisitBitfield(const vixl::aarch64::Instruction* instr) override {
    const bool is64 = instr->GetSixtyFourBits();
    const u32 reg_size = is64 ? 64u : 32u;
    const u64 reg_mask = is64 ? kXRegMask : kWRegMask;
    const u32 R = u32(instr->GetImmR());
    const u32 S = u32(instr->GetImmS());
    if (((instr->GetInstructionBits() >> 22) & 1u) != u32(is64) || R >= reg_size || S >= reg_size) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }

    const auto type = GPRType(is64);
    Value result;
    switch (instr->Mask(BitfieldMask)) {
        case UBFM_w:
        case UBFM_x:
            result = EmitUnsignedBitfield(is64, reg_size, R, S, ReadRegister(instr->GetRn(), type));
            break;
        case SBFM_w:
        case SBFM_x:
            result = EmitSignedBitfield(is64, reg_size, R, S, ReadRegister(instr->GetRn(), type));
            break;
        case BFM_w:
        case BFM_x:
            result = EmitBitfieldInsert(instr, is64, reg_size, reg_mask, R, S);
            break;
        default:
            VIXL_UNIMPLEMENTED();
    }

    WriteGPR(instr->GetRd(), is64, result);
}

void VisitExtract(const vixl::aarch64::Instruction* instr) override {
    const bool is64 = instr->Mask(ExtractMask) == EXTR_x;
    const auto type = GPRType(is64);
    const u32 lsb = instr->GetImmS();

    auto rm = ReadRegister(instr->GetRm(), type);
    auto rn = ReadRegister(instr->GetRn(), type);

    Value result;
    if (lsb == 0) {
        result = rm;
    } else if (instr->GetRn() == instr->GetRm()) {
        // ROR alias.
        result = EmitGPR<OpCode::RorImm>(is64, rm, Imm{lsb});
    } else {
        auto lo = EmitGPR<OpCode::LsrImm>(is64, rm, Imm{lsb});
        auto hi = EmitGPR<OpCode::LslImm>(is64, rn, Imm{(is64 ? 64u : 32u) - lsb});
        result = EmitGPR<OpCode::Or>(is64, lo, SingleOperand(hi));
    }

    WriteGPR(instr->GetRd(), is64, result);
}

void VisitDataProcessing2Source(const vixl::aarch64::Instruction* instr) override {
    const auto op = instr->Mask(DataProcessing2SourceMask);
    const bool ieee = op == CRC32B || op == CRC32H || op == CRC32W || op == CRC32X;
    const bool castagnoli = op == CRC32CB || op == CRC32CH || op == CRC32CW || op == CRC32CX;
    if (ieee || castagnoli) {
        const u32 data_bits = 8u << ((op >> 10) & 3);
        // The accumulator and destination are always W registers, even for
        // CRC32X/CX. Both sources must be read before an aliased Rd is written.
        auto accumulator = ReadWRegister(instr->GetRn());
        auto data = data_bits == 64 ? ReadXRegister(instr->GetRm()) : ReadWRegister(instr->GetRm());
        auto result = castagnoli
                              ? this->Crc32c(accumulator, data, Imm{data_bits}).WithView(Type::U32)
                              : this->Crc32(accumulator, data, Imm{data_bits}).WithView(Type::U32);
        WriteWRegister(instr->GetRd(), result);
        return;
    }
    const bool is64 = instr->GetSixtyFourBits();
    const auto type = GPRType(is64);
    const auto stype = is64 ? Type::S64 : Type::S32;
    const u32 width = is64 ? 64 : 32;

    auto rn = ReadRegister(instr->GetRn(), type);
    auto rm = ReadRegister(instr->GetRm(), type);
    auto shift_amount = [&] {
        return this->template And<Type::U32>(rm, SingleOperand(Imm{width - 1}))
                .WithView(Type::U32);
    };

    Value result{};
    switch (op) {
        case UDIV_w:
        case UDIV_x:
            // Div selects a safe denominator and returns ARM's zero result
            // explicitly; no host integer-division behavior is assumed.
            result = is64 ? this->template Div<Type::U64>(rn, SingleOperand(rm)).WithView(type)
                          : this->template Div<Type::U32>(rn, SingleOperand(rm)).WithView(type);
            break;
        case SDIV_w:
        case SDIV_x: {
            auto l = rn.SetCastType(stype);
            auto r = rm.SetCastType(stype);
            result = is64 ? this->template Div<Type::U64>(l, SingleOperand(r)).WithView(type)
                          : this->template Div<Type::U32>(l, SingleOperand(r)).WithView(type);
            break;
        }
        case LSLV_w:
        case LSLV_x:
            result = EmitGPR<OpCode::LslValue>(is64, rn, shift_amount());
            break;
        case LSRV_w:
        case LSRV_x:
            result = EmitGPR<OpCode::LsrValue>(is64, rn, shift_amount());
            break;
        case ASRV_w:
        case ASRV_x:
            result = EmitGPR<OpCode::AsrValue>(is64, rn.SetCastType(stype), shift_amount());
            break;
        case RORV_w:
        case RORV_x:
            result = EmitGPR<OpCode::RorValue>(is64, rn, shift_amount());
            break;
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }

    WriteGPR(instr->GetRd(), is64, result);
}

void EmitWidenedMulAccumulate64(Value lhs, Value rhs, u8 ra_code, u8 rd_code, bool is_sub) {
    auto mul = this->template Mul<Type::U64>(lhs, SingleOperand(rhs)).WithView(Type::U64);
    Value result;
    if (ra_code == kZeroRegCode) {
        result = is_sub ? this->template Neg<Type::U64>(mul).WithView(Type::U64) : mul;
    } else {
        auto ra = ReadXRegister(ra_code);
        result = (is_sub ? this->template Sub<Type::U64>(ra, SingleOperand(mul))
                         : this->template Add<Type::U64>(ra, SingleOperand(mul)))
                         .WithView(Type::U64);
    }
    WriteXRegister(rd_code, result);
}

void VisitDataProcessing3Source(const vixl::aarch64::Instruction* instr) override {
    const bool is64 = instr->GetSixtyFourBits();
    const auto type = GPRType(is64);
    const u8 ra_code = instr->GetRa();
    const auto op = instr->Mask(DataProcessing3SourceMask);

    switch (op) {
        case MADD_w:
        case MADD_x: {
            auto rn = ReadRegister(instr->GetRn(), type);
            auto rm = ReadRegister(instr->GetRm(), type);
            auto mul = EmitGPR<OpCode::Mul>(is64, rn, SingleOperand(rm));
            Value result = mul;
            if (ra_code != kZeroRegCode) {
                auto ra = ReadRegister(ra_code, type);
                result = EmitGPR<OpCode::Add>(is64, ra, SingleOperand(mul));
            }
            WriteGPR(instr->GetRd(), is64, result);
            return;
        }
        case MSUB_w:
        case MSUB_x: {
            auto rn = ReadRegister(instr->GetRn(), type);
            auto rm = ReadRegister(instr->GetRm(), type);
            auto mul = EmitGPR<OpCode::Mul>(is64, rn, SingleOperand(rm));
            Value result;
            if (ra_code == kZeroRegCode) {
                result = EmitGPR<OpCode::Neg>(is64, mul);
            } else {
                auto ra = ReadRegister(ra_code, type);
                result = EmitGPR<OpCode::Sub>(is64, ra, SingleOperand(mul));
            }
            WriteGPR(instr->GetRd(), is64, result);
            return;
        }
        case UMADDL_x:
        case UMSUBL_x: {
            auto l = this->ZeroExtend64(ReadWRegister(instr->GetRn())).WithView(Type::U64);
            auto r = this->ZeroExtend64(ReadWRegister(instr->GetRm())).WithView(Type::U64);
            EmitWidenedMulAccumulate64(l, r, ra_code, instr->GetRd(), op == UMSUBL_x);
            return;
        }
        case SMADDL_x:
        case SMSUBL_x: {
            auto l = SignExtendValue(ReadWRegister(instr->GetRn()), 32);
            auto r = SignExtendValue(ReadWRegister(instr->GetRm()), 32);
            EmitWidenedMulAccumulate64(l, r, ra_code, instr->GetRd(), op == SMSUBL_x);
            return;
        }
        case SMULH_x:
        case UMULH_x: {
            auto left = ReadXRegister(instr->GetRn());
            auto right = ReadXRegister(instr->GetRm());
            const u32 is_signed = op == SMULH_x ? 1u : 0u;
            WriteXRegister(instr->GetRd(),
                           this->template MulHigh<Type::U64>(left, right, Imm{is_signed}));
            return;
        }
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }
}

void VisitConditionalSelect(const vixl::aarch64::Instruction* instr) override {
    const bool is64 = instr->GetSixtyFourBits();
    const auto type = GPRType(is64);
    const auto cond = static_cast<runtime::ir::Condition>(instr->GetCondition());

    auto rn = ReadRegister(instr->GetRn(), type);
    auto rm = ReadRegister(instr->GetRm(), type);

    Value false_val{};
    switch (instr->Mask(ConditionalSelectMask)) {
        case CSEL_w:
        case CSEL_x:
            false_val = rm;
            break;
        case CSINC_w:
        case CSINC_x:
            false_val = EmitGPR<OpCode::Add>(is64, rm, SingleOperand(Imm{u32(1)}));
            break;
        case CSINV_w:
        case CSINV_x:
            false_val = EmitGPR<OpCode::Xor>(is64, rm, SingleOperand(ImmGPR(is64, ~u64{0})));
            break;
        case CSNEG_w:
        case CSNEG_x:
            false_val = EmitGPR<OpCode::Sub>(is64, ImmValue(0, type), SingleOperand(rm));
            break;
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }

    WriteGPR(instr->GetRd(), is64, this->CondSelect(cond, rn, false_val).WithView(type));
}

void EmitImmediateNzcvFallback(bool is64, u32 nzcv) {
    const auto type = GPRType(is64);
    const bool n = (nzcv & 8) != 0;
    const bool z = (nzcv & 4) != 0;
    const u8 c = (nzcv >> 1) & 1;
    const u8 v = nzcv & 1;
    auto fallback = EmitGPR<OpCode::Sub>(is64, ImmValue(1, type), SingleOperand(Imm{0}));
    this->SaveFlags(fallback, (Flag::Negate | Flag::Zero));
    this->SetCarry(ImmValue(c, Type::U8));
    if (n) {
        const auto sign_imm = is64 ? Imm{u64(1) << 63} : Imm{u32(1) << 31};
        auto negative = EmitGPR<OpCode::Sub>(is64, ImmValue(0, type), SingleOperand(sign_imm));
        this->SaveFlags(negative, Flag::Negate);
        this->SetCarry(ImmValue(c, Type::U8));
    }
    if (z) {
        auto zero = EmitGPR<OpCode::Sub>(is64, ImmValue(0, type), SingleOperand(Imm{0}));
        this->SaveFlags(zero, Flag::Zero);
        this->SetCarry(ImmValue(c, Type::U8));
    }
    this->SetOverflow(ImmValue(v, Type::U8));
}

void ConditionalCompareHelper(const vixl::aarch64::Instruction* instr, const Input& op2) {
    const bool is64 = instr->GetSixtyFourBits();
    const auto type = GPRType(is64);
    auto rn = ReadRegister(instr->GetRn(), type);

    auto compare =
            this->Goto(CondPassed(static_cast<vixl::aarch64::Condition>(instr->GetCondition())));

    if (target.mode == TargetMode::Known && target.target == 0x8664)
        Emit<OpCode::RestoreFlags>(ImmValue(u64(instr->GetNzcv()) << 28, Type::U64), Flag::NZCV);
    else
        EmitImmediateNzcvFallback(is64, instr->GetNzcv());
    auto done = this->Goto(Value{ImmValue(1, Type::U8)});

    this->BindLabel(compare);
    const bool is_ccmp = instr->Mask(ConditionalCompareMask) == CCMP;
    auto result = is_ccmp ? EmitGPR<OpCode::Sub>(is64, rn, SingleOperand(op2))
                          : EmitGPR<OpCode::Add>(is64, rn, SingleOperand(op2));
    this->SaveFlags(result, Flag::NZCV);
    this->BindLabel(done);
}

void VisitConditionalCompareImmediate(const vixl::aarch64::Instruction* instr) override {
    u32 imm = u32(instr->GetImmCondCmp());
    ConditionalCompareHelper(instr, Input{Imm{imm}});
}

void VisitConditionalCompareRegister(const vixl::aarch64::Instruction* instr) override {
    bool is64 = instr->GetSixtyFourBits();
    auto rm = ReadRegister(u8(instr->GetRm()), GPRType(is64));
    ConditionalCompareHelper(instr, Input{rm});
}

Value EmitByteSwapInPairs64(Value x) {
    auto hi = this->template LsrImm<Type::U64>(
                          this->template And<Type::U64>(
                                      x, SingleOperand(Imm{u64(0xFF00FF00FF00FF00)}))
                                  .WithView(Type::U64),
                          Imm{8})
                      .WithView(Type::U64);
    auto lo = this->template LslImm<Type::U64>(
                          this->template And<Type::U64>(
                                      x, SingleOperand(Imm{u64(0x00FF00FF00FF00FF)}))
                                  .WithView(Type::U64),
                          Imm{8})
                      .WithView(Type::U64);
    return this->template Or<Type::U64>(hi, SingleOperand(lo)).WithView(Type::U64);
}

Value EmitBitReverse(Value rn, bool is64) {
    auto x = Widen(rn);
    auto swap_stage = [&](Value v, u64 hi_mask, u64 lo_mask, u32 shift) {
        auto hi = this->template LsrImm<Type::U64>(
                              this->template And<Type::U64>(v, SingleOperand(Imm{hi_mask}))
                                      .WithView(Type::U64),
                              Imm{shift})
                          .WithView(Type::U64);
        auto lo = this->template LslImm<Type::U64>(
                              this->template And<Type::U64>(v, SingleOperand(Imm{lo_mask}))
                                      .WithView(Type::U64),
                              Imm{shift})
                          .WithView(Type::U64);
        return this->template Or<Type::U64>(hi, SingleOperand(lo)).WithView(Type::U64);
    };
    x = swap_stage(x, 0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL, 1);
    x = swap_stage(x, 0xCCCCCCCCCCCCCCCCULL, 0x3333333333333333ULL, 2);
    x = swap_stage(x, 0xF0F0F0F0F0F0F0F0ULL, 0x0F0F0F0F0F0F0F0FULL, 4);
    return is64 ? this->template ByteSwap<Type::U64>(x, Imm{64u})
                : this->template ByteSwap<Type::U32>(x.WithView(Type::U32), Imm{32u});
}

void VisitDataProcessing1Source(const vixl::aarch64::Instruction* instr) override {
    // Pointer-authentication instructions share this visitor but set opcode2
    // (bit 16). Matching only opcode[5:0] aliases PACIA..AUTIA with RBIT..CLZ.
    // Reject all unsupported full encodings before reading or writing state.
    const u32 full_op = instr->Mask(DataProcessing1SourceMask) & ~u32(SixtyFourBits);
    switch (full_op) {
        case RBIT:
        case REV16:
        case REV:
        case (REV_x & ~u32(SixtyFourBits)):
        case CLZ:
        case CLS:
            break;
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }
    const bool is64 = instr->GetSixtyFourBits();
    const auto type = GPRType(is64);
    auto rn = ReadRegister(instr->GetRn(), type);
    const auto rd = u8(instr->GetRd());
    const u32 opcode = u32(instr->GetInstructionBits() >> 10) & 0x3F;

    switch (opcode) {
        case 0x00:  // RBIT
            WriteGPR(rd, is64, EmitBitReverse(rn, is64));
            break;
        case 0x01:  // REV16
            if (is64) {
                WriteXRegister(rd, EmitByteSwapInPairs64(rn));
            } else {
                auto swapped = this->template ByteSwap<Type::U32>(rn, Imm{32u});
                WriteWRegister(rd, this->template RorImm<Type::U32>(swapped, Imm{16u}));
            }
            break;
        case 0x02:  // REV (32-bit) / REV32 (64-bit)
            if (is64) {
                auto swapped = this->template ByteSwap<Type::U64>(rn, Imm{64u});
                WriteXRegister(rd, this->template RorImm<Type::U64>(swapped, Imm{32u}));
            } else {
                WriteWRegister(rd, this->template ByteSwap<Type::U32>(rn, Imm{32u}));
            }
            break;
        case 0x03:  // REV (64-bit only)
            if (!is64) {
                Interrupt(InterruptReason::ILL_CODE, current_pc);
                return;
            }
            WriteXRegister(rd, this->template ByteSwap<Type::U64>(rn, Imm{64u}));
            break;
        case 0x04:  // CLZ
            WriteGPR(rd,
                     is64,
                     is64 ? this->CountLeadingZeros64(rn) : this->CountLeadingZeros32(rn));
            break;
        case 0x05: {  // CLS
            const u32 width = is64 ? 64 : 32;
            auto sign = this->AsrImm(rn, Imm{width - 1}).WithView(type);
            auto normalized = this->Xor(rn, SingleOperand(sign)).WithView(type);
            auto leading = is64 ? this->CountLeadingZeros64(normalized)
                                : this->CountLeadingZeros32(normalized);
            auto count = this->Sub(leading, SingleOperand(Imm{u64{1}})).WithView(type);
            WriteGPR(rd, is64, count);
            break;
        }
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }
}

void VisitRotateRightIntoFlags(const vixl::aarch64::Instruction* instr) override {
    if (instr->Mask(RotateRightIntoFlagsMask) != RMIF) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const u32 mask = instr->GetNzcv();
    if (!mask) return;
    auto rotated = ReadXRegister(instr->GetRn());
    const u32 shift = instr->GetImmRMIFRotation();
    if (shift) rotated = this->template RorImm<Type::U64>(rotated, Imm{shift}).WithView(Type::U64);
    auto value = this->template And<Type::U64>(rotated, SingleOperand(Imm{u64(mask)}))
                         .WithView(Type::U64);
    const Flag flags[] = {Flag::Overflow, Flag::Carry, Flag::Zero, Flag::Negate};
    for (u32 bit = 0; bit < 4; ++bit) {
        if (mask & (1u << bit)) continue;
        auto old = this->TestFlags(flags[bit]).WithView(Type::U8);
        auto packed = this->template LslImm<Type::U64>(Widen(old), Imm{bit}).WithView(Type::U64);
        value = this->template Or<Type::U64>(value, SingleOperand(packed)).WithView(Type::U64);
    }
    WriteNZCV(value);
}

void VisitEvaluateIntoFlags(const vixl::aarch64::Instruction* instr) override {
    const auto op = instr->Mask(EvaluateIntoFlagsMask);
    if (op != SETF8 && op != SETF16) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const u32 bits = op == SETF8 ? 8 : 16;
    auto source = ReadWRegister(instr->GetRn());
    auto sign = this->TestBit(source, Imm{bits - 1}).WithView(Type::U8);
    auto overflow = this->TestBit(source, Imm{bits}).WithView(Type::U8);
    overflow = this->template Xor<Type::U8>(sign, SingleOperand(overflow)).WithView(Type::U8);
    auto n = this->template LslImm<Type::U64>(Widen(sign), Imm{63u}).WithView(Type::U64);
    n = this->template Sub<Type::U64>(ImmValue(0, Type::U64), SingleOperand(n)).WithView(Type::U64);
    auto z = this->template And<Type::U32>(source, SingleOperand(Imm{(1u << bits) - 1}))
                     .WithView(Type::U32);
    z = this->template Sub<Type::U32>(z, SingleOperand(Imm{u32{0}})).WithView(Type::U32);
    this->SaveFlags(n, Flag::Negate);
    this->SaveFlags(z, Flag::Zero);
    this->SetOverflow(overflow);
}
