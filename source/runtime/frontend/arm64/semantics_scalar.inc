void AddSubHelper(const vixl::aarch64::Instruction* instr, const Input& op2) {
    bool is64 = instr->GetSixtyFourBits();
    auto operation = instr->Mask(AddSubOpMask);
    bool is_sub = (operation == SUB) || (operation == SUBS);
    bool set_flags = (operation == ADDS) || (operation == SUBS);

    auto rn = ReadRegister(instr->GetRn(), GPRType(is64), instr->GetRnMode());

    Value result;
    if (is64) {
        result = is_sub ? this->template Sub<Type::U64>(rn, SingleOperand(op2)).WithView(Type::U64)
                        : this->template Add<Type::U64>(rn, SingleOperand(op2)).WithView(Type::U64);
    } else {
        result = is_sub ? this->template Sub<Type::U32>(rn, SingleOperand(op2)).WithView(Type::U32)
                        : this->template Add<Type::U32>(rn, SingleOperand(op2)).WithView(Type::U32);
    }

    if (set_flags) {
        // The neutral subtraction carries borrow. SaveFlags converts that
        // single output to ARM C = no-borrow; N/Z/V remain neutral.
        this->SaveFlags(result, Flag::NZCV);
    }

    if (is64) {
        WriteXRegister(instr->GetRd(), result, instr->GetRdMode());
    } else {
        WriteWRegister(instr->GetRd(), result, instr->GetRdMode());
    }
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
    bool is64 = instr->GetSixtyFourBits();
    auto op = instr->Mask(AddSubWithCarryMask);
    bool is_sub = (op == SBC_w) || (op == SBC_x) || (op == SBCS_w) || (op == SBCS_x);
    bool set_flags = (op == ADCS_w) || (op == ADCS_x) || (op == SBCS_w) || (op == SBCS_x);

    auto rn = ReadRegister(instr->GetRn(), GPRType(is64));
    auto rm = ReadRegister(instr->GetRm(), GPRType(is64));

    // SBC = a + ~b + C, so both ADC and SBC map onto Adc with the AArch64
    // carry flag (C = NOT borrow) consumed directly.
    Value operand2 = rm;
    if (is_sub) {
        operand2 = is64 ? this->template Xor<Type::U64>(rm, SingleOperand(Imm{u64(-1)}))
                                   .WithView(Type::U64)
                        : this->template Xor<Type::U32>(rm, SingleOperand(Imm{u32(-1)}))
                                   .WithView(Type::U32);
    }

    Value result =
            is64 ? this->template Adc<Type::U64>(rn, SingleOperand(operand2)).WithView(Type::U64)
                 : this->template Adc<Type::U32>(rn, SingleOperand(operand2)).WithView(Type::U32);
    if (set_flags) {
        this->SaveFlags(result, Flag::NZCV);
    }
    if (is64) {
        WriteXRegister(instr->GetRd(), result);
    } else {
        WriteWRegister(instr->GetRd(), result);
    }
}

void VisitLogicalImmediate(const vixl::aarch64::Instruction* instr) override {
    bool is64 = instr->GetSixtyFourBits();
    u64 imm = instr->GetImmLogical();
    Input op2 = is64 ? Input{Imm{imm}} : Input{Imm{u32(imm)}};

    auto rn = ReadRegister(instr->GetRn(), GPRType(is64));

    auto type = GPRType(is64);
    Value result{};
    bool set_flags = false;
    switch (instr->Mask(LogicalImmediateMask)) {
        case AND_w_imm:
        case AND_x_imm:
            result = is64 ? this->template And<Type::U64>(rn, SingleOperand(op2)).WithView(type)
                          : this->template And<Type::U32>(rn, SingleOperand(op2)).WithView(type);
            break;
        case ORR_w_imm:
        case ORR_x_imm:
            result = is64 ? this->template Or<Type::U64>(rn, SingleOperand(op2)).WithView(type)
                          : this->template Or<Type::U32>(rn, SingleOperand(op2)).WithView(type);
            break;
        case EOR_w_imm:
        case EOR_x_imm:
            result = is64 ? this->template Xor<Type::U64>(rn, SingleOperand(op2)).WithView(type)
                          : this->template Xor<Type::U32>(rn, SingleOperand(op2)).WithView(type);
            break;
        case ANDS_w_imm:
        case ANDS_x_imm:
            result = is64 ? this->template And<Type::U64>(rn, SingleOperand(op2)).WithView(type)
                          : this->template And<Type::U32>(rn, SingleOperand(op2)).WithView(type);
            set_flags = true;
            break;
        default:
            VIXL_UNREACHABLE();
    }

    if (set_flags) {
        // Host ANDS also yields C = V = 0, matching the AArch64 semantics.
        this->SaveFlags(result, Flag::NZCV);
    }
    if (is64) {
        WriteXRegister(instr->GetRd(), result, instr->GetRdMode());
    } else {
        WriteWRegister(instr->GetRd(), result, instr->GetRdMode());
    }
}

void VisitLogicalShifted(const vixl::aarch64::Instruction* instr) override {
    bool is64 = instr->GetSixtyFourBits();
    auto type = GPRType(is64);

    auto rn = ReadRegister(instr->GetRn(), type);
    auto op2 = ShiftOperand(is64,
                            ReadRegister(instr->GetRm(), type),
                            static_cast<Shift>(instr->GetShiftDP()),
                            u32(instr->GetImmDPShift()));

    Value result{};
    bool set_flags = false;
    auto and_ = [&] {
        return is64 ? this->template And<Type::U64>(rn, SingleOperand(op2)).WithView(type)
                    : this->template And<Type::U32>(rn, SingleOperand(op2)).WithView(type);
    };
    auto bic_ = [&] {
        return is64 ? this->template AndNot<Type::U64>(rn, SingleOperand(op2)).WithView(type)
                    : this->template AndNot<Type::U32>(rn, SingleOperand(op2)).WithView(type);
    };
    auto or_ = [&](const Input& o) {
        return is64 ? this->template Or<Type::U64>(rn, SingleOperand(o)).WithView(type)
                    : this->template Or<Type::U32>(rn, SingleOperand(o)).WithView(type);
    };
    auto xor_ = [&](const Input& o) {
        return is64 ? this->template Xor<Type::U64>(rn, SingleOperand(o)).WithView(type)
                    : this->template Xor<Type::U32>(rn, SingleOperand(o)).WithView(type);
    };
    auto not_ = [&](Value value) {
        return is64 ? this->template Xor<Type::U64>(value, SingleOperand(Imm{u64(-1)}))
                               .WithView(type)
                    : this->template Xor<Type::U32>(value, SingleOperand(Imm{u32(-1)}))
                               .WithView(type);
    };

    switch (instr->Mask(LogicalShiftedMask)) {
        case AND_w:
        case AND_x:
            result = and_();
            break;
        case BIC_w:
        case BIC_x:
            result = bic_();
            break;
        case ORR_w:
        case ORR_x:
            result = or_(Input{op2});
            break;
        case ORN_w:
        case ORN_x:
            result = or_(Input{not_(op2)});
            break;
        case EOR_w:
        case EOR_x:
            result = xor_(Input{op2});
            break;
        case EON_w:
        case EON_x:
            result = xor_(Input{not_(op2)});
            break;
        case ANDS_w:
        case ANDS_x:
            result = and_();
            set_flags = true;
            break;
        case BICS_w:
        case BICS_x:
            result = bic_();
            set_flags = true;
            break;
        default:
            VIXL_UNREACHABLE();
    }

    if (set_flags) {
        this->SaveFlags(result, Flag::NZCV);
    }
    if (is64) {
        WriteXRegister(instr->GetRd(), result);
    } else {
        WriteWRegister(instr->GetRd(), result);
    }
}

void VisitMoveWideImmediate(const vixl::aarch64::Instruction* instr) override {
    auto mov_op = static_cast<MoveWideImmediateOp>(instr->Mask(MoveWideImmediateMask));

    bool is64 = instr->GetSixtyFourBits() == 1;
    // Shift is limited for W operations.
    VIXL_ASSERT(is64 || (instr->GetShiftMoveWide() < 2));

    auto type = GPRType(is64);
    u32 shift = instr->GetShiftMoveWide() * 16;
    u64 shifted_imm16 = u64(instr->GetImmMoveWide()) << shift;

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
            new_val = is64 ? this->template BitInsert<Type::U64>(
                                         prev, imm16, Imm{u8(shift)}, Imm{u8(16)})
                                      .WithView(type)
                           : this->template BitInsert<Type::U32>(
                                         prev, imm16, Imm{u8(shift)}, Imm{u8(16)})
                                      .WithView(type);
            break;
        }
        case MOVZ_w:
        case MOVZ_x:
            new_val = ImmValue(shifted_imm16, type);
            break;
        default:
            VIXL_UNREACHABLE();
    }

    if (is64) {
        WriteXRegister(instr->GetRd(), new_val);
    } else {
        WriteWRegister(instr->GetRd(), new_val);
    }
}

void VisitBitfield(const vixl::aarch64::Instruction* instr) override {
    bool is64 = instr->GetSixtyFourBits();
    u32 reg_size = is64 ? 64 : 32;
    u64 reg_mask = is64 ? kXRegMask : kWRegMask;
    int R = instr->GetImmR();
    int S = instr->GetImmS();
    int diff = S - R;
    u64 mask;
    if (diff >= 0) {
        mask = ~UINT64_C(0) >> (64 - (diff + 1));
        mask = (static_cast<unsigned>(diff) < (reg_size - 1)) ? mask : reg_mask;
    } else {
        mask = ~UINT64_C(0) >> (64 - (S + 1));
        mask = vixl::RotateRight(mask, R, reg_size);
        diff += reg_size;
    }

    // inzero indicates if the extracted bitfield is inserted into the
    // destination register value or in zero. If extend is true, the sign of
    // the extracted bitfield is extended.
    bool inzero = false;
    bool extend = false;
    switch (instr->Mask(BitfieldMask)) {
        case BFM_x:
        case BFM_w:
            break;
        case SBFM_x:
        case SBFM_w:
            inzero = true;
            extend = true;
            break;
        case UBFM_x:
        case UBFM_w:
            inzero = true;
            break;
        default:
            VIXL_UNIMPLEMENTED();
    }

    auto type = GPRType(is64);
    auto src = Widen(ReadRegister(instr->GetRn(), type));

    // Rotate the source bitfield into place.
    Value rotated;
    if (R == 0) {
        rotated = src;
    } else if (is64) {
        rotated = this->template RorImm<Type::U64>(src, Imm{u32(R)}).WithView(Type::U64);
    } else {
        // 32 bit rotate of a zero extended value.
        auto lo = this->template LsrImm<Type::U64>(src, Imm{u32(R)}).WithView(Type::U64);
        auto hi = this->template LslImm<Type::U64>(src, Imm{u32(reg_size - R)}).WithView(Type::U64);
        rotated = this->template Or<Type::U64>(lo, SingleOperand(hi)).WithView(Type::U64);
    }

    Value result =
            this->template And<Type::U64>(rotated, SingleOperand(Imm{mask})).WithView(Type::U64);

    if (extend) {
        // Sign bits: replicate bit S of the source across all bits above it,
        // then keep only the bits above the extracted field.
        u64 topbits = (diff == 63) ? 0 : (~UINT64_C(0) << (diff + 1));
        Value sign_extended;
        if (S == 63) {
            sign_extended = src;
        } else {
            auto shl = this->template LslImm<Type::U64>(src, Imm{u32(63 - S)}).WithView(Type::U64);
            sign_extended =
                    this->template AsrImm<Type::U64>(shl, Imm{u32(63 - S)}).WithView(Type::U64);
        }
        auto signbits = this->template And<Type::U64>(sign_extended, SingleOperand(Imm{topbits}))
                                .WithView(Type::U64);
        result = this->template Or<Type::U64>(result, SingleOperand(signbits)).WithView(Type::U64);
    }

    if (!inzero) {
        auto dst = Widen(ReadRegister(instr->GetRd(), type));
        auto dst_masked =
                this->template And<Type::U64>(dst, SingleOperand(Imm{~mask})).WithView(Type::U64);
        result =
                this->template Or<Type::U64>(result, SingleOperand(dst_masked)).WithView(Type::U64);
    }

    if (is64) {
        WriteXRegister(instr->GetRd(), result);
    } else {
        WriteWRegister(instr->GetRd(), result);
    }
}

void VisitExtract(const vixl::aarch64::Instruction* instr) override {
    bool is64 = instr->Mask(ExtractMask) == EXTR_x;
    auto type = GPRType(is64);
    u32 lsb = instr->GetImmS();

    auto rm = ReadRegister(instr->GetRm(), type);
    auto rn = ReadRegister(instr->GetRn(), type);

    Value result;
    if (lsb == 0) {
        result = rm;
    } else if (instr->GetRn() == instr->GetRm()) {
        // ROR alias.
        result = is64 ? this->template RorImm<Type::U64>(rm, Imm{lsb}).WithView(type)
                      : this->template RorImm<Type::U32>(rm, Imm{lsb}).WithView(type);
    } else {
        auto lo = is64 ? this->template LsrImm<Type::U64>(rm, Imm{lsb}).WithView(type)
                       : this->template LsrImm<Type::U32>(rm, Imm{lsb}).WithView(type);
        auto hi = is64 ? this->template LslImm<Type::U64>(rn, Imm{64 - lsb}).WithView(type)
                       : this->template LslImm<Type::U32>(rn, Imm{32 - lsb}).WithView(type);
        result = is64 ? this->template Or<Type::U64>(lo, SingleOperand(hi)).WithView(type)
                      : this->template Or<Type::U32>(lo, SingleOperand(hi)).WithView(type);
    }

    if (is64) {
        WriteXRegister(instr->GetRd(), result);
    } else {
        WriteWRegister(instr->GetRd(), result);
    }
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
    bool is64 = instr->GetSixtyFourBits();
    auto type = GPRType(is64);
    auto stype = is64 ? Type::S64 : Type::S32;
    u32 width = is64 ? 64 : 32;

    auto rn = ReadRegister(instr->GetRn(), type);
    auto rm = ReadRegister(instr->GetRm(), type);

    Value result{};
    switch (instr->Mask(DataProcessing2SourceMask)) {
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
        case LSLV_x: {
            auto amount = this->template And<Type::U32>(rm, SingleOperand(Imm{width - 1}))
                                  .WithView(Type::U32);
            result = is64 ? this->template LslValue<Type::U64>(rn, amount).WithView(type)
                          : this->template LslValue<Type::U32>(rn, amount).WithView(type);
            break;
        }
        case LSRV_w:
        case LSRV_x: {
            auto amount = this->template And<Type::U32>(rm, SingleOperand(Imm{width - 1}))
                                  .WithView(Type::U32);
            result = is64 ? this->template LsrValue<Type::U64>(rn, amount).WithView(type)
                          : this->template LsrValue<Type::U32>(rn, amount).WithView(type);
            break;
        }
        case ASRV_w:
        case ASRV_x: {
            auto amount = this->template And<Type::U32>(rm, SingleOperand(Imm{width - 1}))
                                  .WithView(Type::U32);
            result = is64 ? this->template AsrValue<Type::U64>(rn.SetCastType(stype), amount)
                                     .WithView(type)
                          : this->template AsrValue<Type::U32>(rn.SetCastType(stype), amount)
                                     .WithView(type);
            break;
        }
        case RORV_w:
        case RORV_x: {
            auto amount = this->template And<Type::U32>(rm, SingleOperand(Imm{width - 1}))
                                  .WithView(Type::U32);
            result = is64 ? this->template RorValue<Type::U64>(rn, amount).WithView(type)
                          : this->template RorValue<Type::U32>(rn, amount).WithView(type);
            break;
        }
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }

    if (is64) {
        WriteXRegister(instr->GetRd(), result);
    } else {
        WriteWRegister(instr->GetRd(), result);
    }
}

void VisitDataProcessing3Source(const vixl::aarch64::Instruction* instr) override {
    bool is64 = instr->GetSixtyFourBits();
    auto type = GPRType(is64);

    Value result{};
    switch (instr->Mask(DataProcessing3SourceMask)) {
        case MADD_w:
        case MADD_x: {
            // MUL alias when Ra == XZR.
            auto rn = ReadRegister(instr->GetRn(), type);
            auto rm = ReadRegister(instr->GetRm(), type);
            auto ra = ReadRegister(instr->GetRa(), type);
            auto mul = is64 ? this->template Mul<Type::U64>(rn, SingleOperand(rm)).WithView(type)
                            : this->template Mul<Type::U32>(rn, SingleOperand(rm)).WithView(type);
            result = is64 ? this->template Add<Type::U64>(ra, SingleOperand(mul)).WithView(type)
                          : this->template Add<Type::U32>(ra, SingleOperand(mul)).WithView(type);
            if (is64) {
                WriteXRegister(instr->GetRd(), result);
            } else {
                WriteWRegister(instr->GetRd(), result);
            }
            return;
        }
        case MSUB_w:
        case MSUB_x: {
            auto rn = ReadRegister(instr->GetRn(), type);
            auto rm = ReadRegister(instr->GetRm(), type);
            auto ra = ReadRegister(instr->GetRa(), type);
            auto mul = is64 ? this->template Mul<Type::U64>(rn, SingleOperand(rm)).WithView(type)
                            : this->template Mul<Type::U32>(rn, SingleOperand(rm)).WithView(type);
            result = is64 ? this->template Sub<Type::U64>(ra, SingleOperand(mul)).WithView(type)
                          : this->template Sub<Type::U32>(ra, SingleOperand(mul)).WithView(type);
            if (is64) {
                WriteXRegister(instr->GetRd(), result);
            } else {
                WriteWRegister(instr->GetRd(), result);
            }
            return;
        }
        case UMADDL_x: {
            // Widening multiply: sources are W registers.
            auto rn = ReadWRegister(instr->GetRn());
            auto rm = ReadWRegister(instr->GetRm());
            auto ra = ReadXRegister(instr->GetRa());
            auto l = this->ZeroExtend64(rn).WithView(Type::U64);
            auto r = this->ZeroExtend64(rm).WithView(Type::U64);
            auto mul = this->template Mul<Type::U64>(l, SingleOperand(r)).WithView(Type::U64);
            WriteXRegister(
                    instr->GetRd(),
                    this->template Add<Type::U64>(ra, SingleOperand(mul)).WithView(Type::U64));
            return;
        }
        case UMSUBL_x: {
            auto rn = ReadWRegister(instr->GetRn());
            auto rm = ReadWRegister(instr->GetRm());
            auto ra = ReadXRegister(instr->GetRa());
            auto l = this->ZeroExtend64(rn).WithView(Type::U64);
            auto r = this->ZeroExtend64(rm).WithView(Type::U64);
            auto mul = this->template Mul<Type::U64>(l, SingleOperand(r)).WithView(Type::U64);
            WriteXRegister(
                    instr->GetRd(),
                    this->template Sub<Type::U64>(ra, SingleOperand(mul)).WithView(Type::U64));
            return;
        }
        case SMADDL_x: {
            auto rn = ReadWRegister(instr->GetRn());
            auto rm = ReadWRegister(instr->GetRm());
            auto ra = ReadXRegister(instr->GetRa());
            auto l = SignExtendValue(rn, 32);
            auto r = SignExtendValue(rm, 32);
            auto mul = this->template Mul<Type::U64>(l, SingleOperand(r));
            WriteXRegister(
                    instr->GetRd(),
                    this->template Add<Type::U64>(ra, SingleOperand(mul)).WithView(Type::U64));
            return;
        }
        case SMSUBL_x: {
            auto rn = ReadWRegister(instr->GetRn());
            auto rm = ReadWRegister(instr->GetRm());
            auto ra = ReadXRegister(instr->GetRa());
            auto l = SignExtendValue(rn, 32);
            auto r = SignExtendValue(rm, 32);
            auto mul = this->template Mul<Type::U64>(l, SingleOperand(r));
            WriteXRegister(
                    instr->GetRd(),
                    this->template Sub<Type::U64>(ra, SingleOperand(mul)).WithView(Type::U64));
            return;
        }
        case SMULH_x:
        case UMULH_x: {
            auto left = ReadXRegister(instr->GetRn());
            auto right = ReadXRegister(instr->GetRm());
            auto low32 = [&](Value value) {
                return this->template And<Type::U64>(value, SingleOperand(Imm{0xFFFFFFFF}))
                        .WithView(Type::U64);
            };
            auto left_high = this->template LsrImm<Type::U64>(left, Imm{32}).WithView(Type::U64);
            auto right_high = this->template LsrImm<Type::U64>(right, Imm{32}).WithView(Type::U64);
            auto product00 = this->template Mul<Type::U64>(low32(left), SingleOperand(low32(right)))
                                     .WithView(Type::U64);
            auto product01 = this->template Mul<Type::U64>(low32(left), SingleOperand(right_high))
                                     .WithView(Type::U64);
            auto product10 = this->template Mul<Type::U64>(left_high, SingleOperand(low32(right)))
                                     .WithView(Type::U64);
            auto product11 = this->template Mul<Type::U64>(left_high, SingleOperand(right_high))
                                     .WithView(Type::U64);
            auto middle = this->template Add<Type::U64>(
                                      this->template LsrImm<Type::U64>(product00, Imm{32}),
                                      SingleOperand(low32(product01)))
                                  .WithView(Type::U64);
            middle = this->template Add<Type::U64>(middle, SingleOperand(low32(product10)))
                             .WithView(Type::U64);
            auto high =
                    this->template Add<Type::U64>(
                                product11,
                                SingleOperand(this->template LsrImm<Type::U64>(product01, Imm{32})))
                            .WithView(Type::U64);
            high = this->template Add<Type::U64>(
                               high,
                               SingleOperand(this->template LsrImm<Type::U64>(product10, Imm{32})))
                           .WithView(Type::U64);
            high = this->template Add<Type::U64>(
                               high,
                               SingleOperand(this->template LsrImm<Type::U64>(middle, Imm{32})))
                           .WithView(Type::U64);
            if (instr->Mask(DataProcessing3SourceMask) == SMULH_x) {
                auto left_sign =
                        this->template AsrImm<Type::U64>(left, Imm{63}).WithView(Type::U64);
                auto right_sign =
                        this->template AsrImm<Type::U64>(right, Imm{63}).WithView(Type::U64);
                high = this->template Sub<Type::U64>(high,
                                                     SingleOperand(this->template And<Type::U64>(
                                                             left_sign, SingleOperand(right))))
                               .WithView(Type::U64);
                high = this->template Sub<Type::U64>(high,
                                                     SingleOperand(this->template And<Type::U64>(
                                                             right_sign, SingleOperand(left))))
                               .WithView(Type::U64);
            }
            WriteXRegister(instr->GetRd(), high);
            return;
        }
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }
}

void VisitConditionalSelect(const vixl::aarch64::Instruction* instr) override {
    bool is64 = instr->GetSixtyFourBits();
    auto type = GPRType(is64);
    auto cond = static_cast<runtime::ir::Condition>(instr->GetCondition());

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
            false_val = is64 ? this->template Add<Type::U64>(rm, SingleOperand(Imm{u32(1)}))
                                        .WithView(type)
                             : this->template Add<Type::U32>(rm, SingleOperand(Imm{u32(1)}))
                                        .WithView(type);
            break;
        case CSINV_w:
        case CSINV_x:
            false_val = is64 ? this->template Xor<Type::U64>(rm, SingleOperand(Imm{u64(-1)}))
                                        .WithView(type)
                             : this->template Xor<Type::U32>(rm, SingleOperand(Imm{u32(-1)}))
                                        .WithView(type);
            break;
        case CSNEG_w:
        case CSNEG_x:
            false_val = is64 ? this->template Sub<Type::U64>(ImmValue(0, type), SingleOperand(rm))
                                        .WithView(type)
                             : this->template Sub<Type::U32>(ImmValue(0, type), SingleOperand(rm))
                                        .WithView(type);
            break;
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }

    auto result = this->CondSelect(cond, rn, false_val).WithView(type);
    if (is64) {
        WriteXRegister(instr->GetRd(), result);
    } else {
        WriteWRegister(instr->GetRd(), result);
    }
}

void ConditionalCompareHelper(const vixl::aarch64::Instruction* instr, const Input& op2) {
    bool is64 = instr->GetSixtyFourBits();
    auto type = GPRType(is64);
    auto rn = ReadRegister(instr->GetRn(), type);

    auto compare =
            this->Goto(CondPassed(static_cast<vixl::aarch64::Condition>(instr->GetCondition())));

    const u32 nzcv = instr->GetNzcv();
    const bool n = (nzcv & 8) != 0;
    const bool z = (nzcv & 4) != 0;
    const u8 c = (nzcv >> 1) & 1;
    const u8 v = nzcv & 1;
    auto fallback = is64 ? this->template Sub<Type::U64>(ImmValue(1, type), SingleOperand(Imm{0}))
                                    .WithView(type)
                         : this->template Sub<Type::U32>(ImmValue(1, type), SingleOperand(Imm{0}))
                                    .WithView(type);
    this->SaveFlags(fallback, (Flag::Negate | Flag::Zero));
    this->SetCarry(ImmValue(c, Type::U8));
    if (n) {
        auto negative = is64 ? this->template Sub<Type::U64>(ImmValue(0, type),
                                                             SingleOperand(Imm{u64(1) << 63}))
                                        .WithView(type)
                             : this->template Sub<Type::U32>(ImmValue(0, type),
                                                             SingleOperand(Imm{u32(1) << 31}))
                                        .WithView(type);
        this->SaveFlags(negative, Flag::Negate);
        this->SetCarry(ImmValue(c, Type::U8));
    }
    if (z) {
        auto zero = is64 ? this->template Sub<Type::U64>(ImmValue(0, type), SingleOperand(Imm{0}))
                                    .WithView(type)
                         : this->template Sub<Type::U32>(ImmValue(0, type), SingleOperand(Imm{0}))
                                    .WithView(type);
        this->SaveFlags(zero, Flag::Zero);
        this->SetCarry(ImmValue(c, Type::U8));
    }
    this->SetOverflow(ImmValue(v, Type::U8));
    auto done = this->Goto(Value{ImmValue(1, Type::U8)});

    this->BindLabel(compare);
    const bool is_ccmp = instr->Mask(ConditionalCompareMask) == CCMP;
    auto result =
            is64 ? (is_ccmp ? this->template Sub<Type::U64>(rn, SingleOperand(op2)).WithView(type)
                            : this->template Add<Type::U64>(rn, SingleOperand(op2)).WithView(type))
                 : (is_ccmp ? this->template Sub<Type::U32>(rn, SingleOperand(op2)).WithView(type)
                            : this->template Add<Type::U32>(rn, SingleOperand(op2)).WithView(type));
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
    bool is64 = instr->GetSixtyFourBits();
    auto type = GPRType(is64);
    auto rn = ReadRegister(instr->GetRn(), type);
    auto rd = u8(instr->GetRd());
    u32 opcode = u32(instr->GetInstructionBits() >> 10) & 0x3F;

    // Helper: byte-swap within each 16-bit half-pair (REV16 step).
    auto swap_bytes_in_pairs = [&](Value x) -> Value {
        return this
                ->template Or<Type::U64>(
                        this->template LsrImm<Type::U64>(
                                    this->template And<Type::U64>(
                                                x, SingleOperand(Imm{u64(0xFF00FF00FF00FF00)}))
                                            .WithView(Type::U64),
                                    Imm{8})
                                .WithView(Type::U64),
                        SingleOperand(
                                this->template LslImm<Type::U64>(
                                            this->template And<Type::U64>(
                                                        x,
                                                        SingleOperand(Imm{u64(0x00FF00FF00FF00FF)}))
                                                    .WithView(Type::U64),
                                            Imm{8})
                                        .WithView(Type::U64)))
                .WithView(Type::U64);
    };
    auto swap16 = [&](Value x) -> Value {
        return this
                ->template Or<Type::U64>(
                        this->template LsrImm<Type::U64>(
                                    this->template And<Type::U64>(
                                                x, SingleOperand(Imm{u64(0xFFFF0000FFFF0000)}))
                                            .WithView(Type::U64),
                                    Imm{16})
                                .WithView(Type::U64),
                        SingleOperand(
                                this->template LslImm<Type::U64>(
                                            this->template And<Type::U64>(
                                                        x,
                                                        SingleOperand(Imm{u64(0x0000FFFF0000FFFF)}))
                                                    .WithView(Type::U64),
                                            Imm{16})
                                        .WithView(Type::U64)))
                .WithView(Type::U64);
    };
    auto swap32 = [&](Value x) -> Value {
        return this
                ->template Or<Type::U64>(
                        this->template LsrImm<Type::U64>(x, Imm{32}).WithView(Type::U64),
                        SingleOperand(
                                this->template LslImm<Type::U64>(x, Imm{32}).WithView(Type::U64)))
                .WithView(Type::U64);
    };

    switch (opcode) {
        case 0x00: {  // RBIT
            auto x = Widen(rn);
            // Swap odd/even bits
            x = this->template Or<Type::U64>(
                            this->template LsrImm<Type::U64>(
                                        this->template And<Type::U64>(
                                                    x, SingleOperand(Imm{u64(0xAAAAAAAAAAAAAAAA)}))
                                                .WithView(Type::U64),
                                        Imm{1})
                                    .WithView(Type::U64),
                            SingleOperand(this->template LslImm<Type::U64>(
                                                      this->template And<Type::U64>(
                                                                  x,
                                                                  SingleOperand(Imm{
                                                                          u64(0x5555555555555555)}))
                                                              .WithView(Type::U64),
                                                      Imm{1})
                                                  .WithView(Type::U64)))
                        .WithView(Type::U64);
            // Swap pairs
            x = this->template Or<Type::U64>(
                            this->template LsrImm<Type::U64>(
                                        this->template And<Type::U64>(
                                                    x, SingleOperand(Imm{u64(0xCCCCCCCCCCCCCCCC)}))
                                                .WithView(Type::U64),
                                        Imm{2})
                                    .WithView(Type::U64),
                            SingleOperand(this->template LslImm<Type::U64>(
                                                      this->template And<Type::U64>(
                                                                  x,
                                                                  SingleOperand(Imm{
                                                                          u64(0x3333333333333333)}))
                                                              .WithView(Type::U64),
                                                      Imm{2})
                                                  .WithView(Type::U64)))
                        .WithView(Type::U64);
            // Swap nibbles
            x = this->template Or<Type::U64>(
                            this->template LsrImm<Type::U64>(
                                        this->template And<Type::U64>(
                                                    x, SingleOperand(Imm{u64(0xF0F0F0F0F0F0F0F0)}))
                                                .WithView(Type::U64),
                                        Imm{4})
                                    .WithView(Type::U64),
                            SingleOperand(this->template LslImm<Type::U64>(
                                                      this->template And<Type::U64>(
                                                                  x,
                                                                  SingleOperand(Imm{
                                                                          u64(0x0F0F0F0F0F0F0F0F)}))
                                                              .WithView(Type::U64),
                                                      Imm{4})
                                                  .WithView(Type::U64)))
                        .WithView(Type::U64);
            x = swap_bytes_in_pairs(x);
            x = swap16(x);
            if (is64) x = swap32(x);
            if (is64)
                WriteXRegister(rd, x);
            else
                WriteWRegister(rd, x);
            break;
        }
        case 0x01: {  // REV16
            auto x = swap_bytes_in_pairs(Widen(rn));
            if (is64)
                WriteXRegister(rd, x);
            else
                WriteWRegister(rd, x);
            break;
        }
        case 0x02: {  // REV (32-bit) / REV32 (64-bit)
            auto x = swap_bytes_in_pairs(Widen(rn));
            x = swap16(x);
            if (is64)
                WriteXRegister(rd, x);
            else
                WriteWRegister(rd, x);
            break;
        }
        case 0x03: {  // REV (64-bit only)
            auto x = swap_bytes_in_pairs(Widen(rn));
            x = swap16(x);
            x = swap32(x);
            WriteXRegister(rd, x);
            break;
        }
        case 0x04: {  // CLZ
            auto x = Widen(rn);
            x = this->template Or<Type::U64>(
                            x, SingleOperand(this->template LsrImm<Type::U64>(x, Imm{1})))
                        .WithView(Type::U64);
            x = this->template Or<Type::U64>(
                            x, SingleOperand(this->template LsrImm<Type::U64>(x, Imm{2})))
                        .WithView(Type::U64);
            x = this->template Or<Type::U64>(
                            x, SingleOperand(this->template LsrImm<Type::U64>(x, Imm{4})))
                        .WithView(Type::U64);
            x = this->template Or<Type::U64>(
                            x, SingleOperand(this->template LsrImm<Type::U64>(x, Imm{8})))
                        .WithView(Type::U64);
            x = this->template Or<Type::U64>(
                            x, SingleOperand(this->template LsrImm<Type::U64>(x, Imm{16})))
                        .WithView(Type::U64);
            if (is64) {
                x = this->template Or<Type::U64>(
                                x, SingleOperand(this->template LsrImm<Type::U64>(x, Imm{32})))
                            .WithView(Type::U64);
            }
            auto pairs = this->template And<Type::U64>(this->template LsrImm<Type::U64>(x, Imm{1}),
                                                       SingleOperand(Imm{u64(0x5555555555555555)}))
                                 .WithView(Type::U64);
            x = this->template Sub<Type::U64>(x, SingleOperand(pairs)).WithView(Type::U64);
            auto quarters =
                    this->template And<Type::U64>(this->template LsrImm<Type::U64>(x, Imm{2}),
                                                  SingleOperand(Imm{u64(0x3333333333333333)}))
                            .WithView(Type::U64);
            x = this->template Add<Type::U64>(
                            this->template And<Type::U64>(
                                    x, SingleOperand(Imm{u64(0x3333333333333333)})),
                            SingleOperand(quarters))
                        .WithView(Type::U64);
            x = this->template And<Type::U64>(
                            this->template Add<Type::U64>(
                                    x, SingleOperand(this->template LsrImm<Type::U64>(x, Imm{4}))),
                            SingleOperand(Imm{u64(0x0F0F0F0F0F0F0F0F)}))
                        .WithView(Type::U64);
            x = this->template Add<Type::U64>(
                            x, SingleOperand(this->template LsrImm<Type::U64>(x, Imm{8})))
                        .WithView(Type::U64);
            x = this->template Add<Type::U64>(
                            x, SingleOperand(this->template LsrImm<Type::U64>(x, Imm{16})))
                        .WithView(Type::U64);
            if (is64) {
                x = this->template Add<Type::U64>(
                                x, SingleOperand(this->template LsrImm<Type::U64>(x, Imm{32})))
                            .WithView(Type::U64);
            }
            auto pop = this->template And<Type::U64>(x, SingleOperand(Imm{is64 ? 0x7F : 0x3F}))
                               .WithView(Type::U64);
            auto count = this->template Sub<Type::U64>(ImmValue(is64 ? 64 : 32, Type::U64),
                                                       SingleOperand(pop))
                                 .WithView(Type::U64);
            if (is64)
                WriteXRegister(rd, count);
            else
                WriteWRegister(rd, count);
            break;
        }
        case 0x05: {  // CLS
            const u32 width = is64 ? 64 : 32;
            auto sign = this->AsrImm(rn, Imm{width - 1}).WithView(type);
            auto normalized = this->Xor(rn, SingleOperand(sign)).WithView(type);
            Value leading;
            if (is64)
                leading = this->CountLeadingZeros64(normalized);
            else
                leading = this->CountLeadingZeros32(normalized);
            auto count = this->Sub(leading, SingleOperand(Imm{u64{1}})).WithView(type);
            if (is64)
                WriteXRegister(rd, count);
            else
                WriteWRegister(rd, count);
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