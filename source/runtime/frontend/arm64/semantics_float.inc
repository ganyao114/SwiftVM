
// AArch64 executes with the architectural environment. x64 uses an exact ARM
// semantic operation with a guarded fast path and the same mathematical leaves.
bool X64FP() const { return target.mode == TargetMode::Known && target.target == 0x8664; }

void EmitX64FP(u32 rd, u32 rn, u32 rm, u32 mode) {
    auto lhs = ReadVRegister(rn, Type::V128), rhs = ReadVRegister(rm, Type::V128);
    auto result = Emit<OpCode::ArmFPBinary, Type::V128>(
            lhs, rhs, Imm{layout.fpcr_offset}, Imm{layout.fpsr_offset}, Imm{mode});
    WriteVRegister(rd, result);
}

bool TryNativeScalarFP(const vixl::aarch64::Instruction* instr) {
    if (target.mode != TargetMode::Known || (target.target != 0xa64 && !X64FP())) return false;
    const u32 word = instr->GetInstructionBits();
    const u32 operation = word & FPDataProcessing2SourceMask;
    u32 kind;
    if (operation == FADD || operation == FADD_d)
        kind = 0;
    else if (operation == FSUB || operation == FSUB_d)
        kind = 1;
    else if (operation == FMUL || operation == FMUL_d)
        kind = 2;
    else if (operation == FDIV || operation == FDIV_d)
        kind = 3;
    else
        return false;
    const u32 bits = (operation == FADD_d || operation == FSUB_d || operation == FMUL_d ||
                      operation == FDIV_d)
                             ? 64
                             : 32;
    if (X64FP()) {
        EmitX64FP(instr->GetRd(),
                  instr->GetRn(),
                  instr->GetRm(),
                  runtime::arm64::ArmFPBinaryMode(kind, bits, 1));
        return true;
    }
    auto lhs = ReadVRegister(instr->GetRn(), Type::V128);
    auto rhs = ReadVRegister(instr->GetRm(), Type::V128);
    auto result = Emit<OpCode::ArmScalarFP, Type::V128>(lhs, rhs, Imm{u8(kind)}, Imm{u8(bits)});
    WriteVRegister(instr->GetRd(), result);
    return true;
}

bool EmitX64Numeric(const vixl::aarch64::Instruction* instr, Numeric family) {
    if (!X64FP()) return false;
    using runtime::arm64::FPValueKind;
    const u32 word = instr->GetInstructionBits();
    const auto encoded = runtime::arm64::NumericCall(family, word);
    const auto info = runtime::arm64::DescribeFPValues(encoded);
    if (info.kind == FPValueKind::Invalid || info.kind == FPValueKind::Divide) return false;
    const Imm offsets{runtime::arm64::FPStateOffsets(layout.fpcr_offset, layout.fpsr_offset)};
    const Imm mode{encoded};
    const u32 rn = instr->GetRn(), rm = (word >> 16) & 31, rd = instr->GetRd();
    const auto lhs =
            info.kind == FPValueKind::FromInt ? ReadXRegister(rn) : ReadVRegister(rn, Type::V128);
    if (info.kind == FPValueKind::Compare) {
        const auto rhs = ReadVRegister(rm, Type::V128);
        Emit<OpCode::ArmFPCompare>(lhs, rhs, offsets, mode);
    } else if (info.kind == FPValueKind::Fused) {
        const auto rhs = ReadVRegister(rm, Type::V128);
        const auto addend =
                ReadVRegister(family == Numeric::Scalar ? instr->GetRa() : rd, Type::V128);
        const auto result = Emit<OpCode::ArmFPTernary, Type::V128>(lhs, rhs, addend, offsets, mode);
        WriteVRegister(rd, result);
    } else if (info.kind == FPValueKind::FromInt) {
        WriteVRegister(rd, Emit<OpCode::ArmFPConvert, Type::V128>(lhs, offsets, mode));
    } else if (info.kind == FPValueKind::ToInt) {
        WriteXRegister(rd, Emit<OpCode::ArmFPConvert, Type::U64>(lhs, offsets, mode));
    } else
        WriteVRegister(rd, Emit<OpCode::ArmFPUnary, Type::V128>(lhs, offsets, mode));
    return true;
}

bool TryNativeScalarFMA(const vixl::aarch64::Instruction* instr) {
    if (target.mode != TargetMode::Known || target.target != 0xa64) return false;
    const u32 word = instr->GetInstructionBits();
    const u32 operation = word & FPDataProcessing3SourceMask;
    u32 kind, bits;
    if (operation == FMADD_s)
        kind = 0, bits = 32;
    else if (operation == FMADD_d)
        kind = 0, bits = 64;
    else if (operation == FMSUB_s)
        kind = 1, bits = 32;
    else if (operation == FMSUB_d)
        kind = 1, bits = 64;
    else if (operation == FNMADD_s)
        kind = 2, bits = 32;
    else if (operation == FNMADD_d)
        kind = 2, bits = 64;
    else if (operation == FNMSUB_s)
        kind = 3, bits = 32;
    else if (operation == FNMSUB_d)
        kind = 3, bits = 64;
    else
        return false;
    auto n = ReadVRegister(instr->GetRn(), Type::V128);
    auto m = ReadVRegister(instr->GetRm(), Type::V128);
    auto a = ReadVRegister(instr->GetRa(), Type::V128);
    auto result =
            Emit<OpCode::ArmScalarFMA, Type::V128>(n, m, a, Imm{u8(kind | (bits == 64 ? 4u : 0u))});
    WriteVRegister(instr->GetRd(), result);
    return true;
}

void ScalarFPHelper(const vixl::aarch64::Instruction* instr, bool comparison = false) {
    if (!runtime::arm64::IsSupportedScalarFP(instr->GetInstructionBits()))
        throw Rejected{BuildError::Unsupported};
    if (EmitX64Numeric(instr, Numeric::Scalar)) return;
    auto value = Helper(Numeric::Scalar);
    if (comparison) WriteNZCV(value);
}

bool TryScalarPairwiseFP(const vixl::aarch64::Instruction* instr) {
    return TryNumeric(instr, Numeric::Pairwise);
}

bool TryNEONFP16Unary(const vixl::aarch64::Instruction* instr) {
    return TryNumeric(instr, Numeric::Unary16);
}

bool TryNEONVectorFPBinary(const vixl::aarch64::Instruction* instr) {
    if (X64FP()) {
        const u32 word = instr->GetInstructionBits(), op = word & NEON3SameFPMask;
        const u32 kind = op == NEON_FADD ? 0 : op == NEON_FSUB ? 1 : 2;
        if ((op == NEON_FADD || op == NEON_FSUB || op == NEON_FMUL) &&
            runtime::arm64::SupportsNumeric(Numeric::VectorBinary, word)) {
            const u32 bits = word & (1u << 22) ? 64 : 32;
            EmitX64FP(
                    instr->GetRd(),
                    instr->GetRn(),
                    instr->GetRm(),
                    runtime::arm64::ArmFPBinaryMode(kind, bits, (word & NEON_Q ? 128 : 64) / bits));
            return true;
        }
    }
    return TryNumeric(instr, Numeric::VectorBinary);
}

bool TryNEONVectorFPUnary(const vixl::aarch64::Instruction* instr) {
    return TryNumeric(instr, Numeric::VectorUnary);
}

bool TryNEONFPFixedSIMD(const vixl::aarch64::Instruction* instr) {
    return TryNumeric(instr, Numeric::Fixed);
}

bool TryFRINTRange(const vixl::aarch64::Instruction* instr) {
    return TryNumeric(instr, Numeric::RangeRound);
}

bool TryNEONFPPrecision(const vixl::aarch64::Instruction* instr) {
    return TryNumeric(instr, Numeric::Precision);
}

bool TryNEONIndexedFP(const vixl::aarch64::Instruction* instr) {
    if (X64FP()) {
        const u32 word = instr->GetInstructionBits();
        const bool scalar = word & NEONScalar;
        const u32 mask =
                scalar ? u32(NEONScalarByIndexedElementMask) : u32(NEONByIndexedElementMask);
        const u32 flags = scalar ? u32(NEON_Q | NEONScalar) : 0;
        const u32 size = (word >> 22) & 3;
        if ((word & mask) == (u32(NEON_FMUL_H_byelement) | flags) && size >= 2 &&
            runtime::arm64::SupportsNumeric(Numeric::Indexed, word)) {
            const u32 bits = size == 2 ? 32 : 64;
            const u32 index =
                    bits == 32 ? (((word >> 11) & 1) << 1) | ((word >> 21) & 1) : (word >> 11) & 1;
            const u32 lanes = scalar ? 1 : (word & NEON_Q ? 128 : 64) / bits;
            EmitX64FP(instr->GetRd(),
                      instr->GetRn(),
                      (word >> 16) & 31,
                      runtime::arm64::ArmFPBinaryMode(2, bits, lanes, true, index));
            return true;
        }
    }
    return TryNumeric(instr, Numeric::Indexed);
}

bool TryNEONFPCompare(const vixl::aarch64::Instruction* i, bool h, bool z) {
    return TryNumeric(i,
                      h ? (z ? Numeric::Compare11 : Numeric::Compare10)
                        : (z ? Numeric::Compare01 : Numeric::Compare00));
}

bool TryNEONFPInteger(const vixl::aarch64::Instruction* i, bool h) {
    return TryNumeric(i, h ? Numeric::Integer16 : Numeric::Integer);
}

bool TryNEONFPEstimate(const vixl::aarch64::Instruction* i, bool h) {
    return TryNumeric(i, h ? Numeric::Estimate16 : Numeric::Estimate);
}

bool TryNEONFPReduction(const vixl::aarch64::Instruction* i, bool h, bool a) {
    return TryNumeric(i,
                      h ? (a ? Numeric::Reduction11 : Numeric::Reduction10)
                        : (a ? Numeric::Reduction01 : Numeric::Reduction00));
}

bool TryNEONVectorFMA(const vixl::aarch64::Instruction* i, bool) {
    return TryNumeric(i, Numeric::Fused);
}

bool TryNEONExtendedFP(const vixl::aarch64::Instruction* i, bool) {
    return TryNumeric(i, Numeric::Extended);
}

void VisitNEON3SameFP16(const vixl::aarch64::Instruction* i) override {
    if (TryNEONFPCompare(i, true, false) || TryNEONFPReduction(i, true, false) ||
        TryNEONExtendedFP(i, true) || TryNEONVectorFMA(i, true) || TryNumeric(i, Numeric::Binary16))
        return;
    throw Rejected{BuildError::Unsupported};
}

void VisitNEONScalar3SameFP16(const vixl::aarch64::Instruction* i) override {
    VisitNEON3SameFP16(i);
}

void VisitNEONScalar2RegMiscFP16(const vixl::aarch64::Instruction* i) override {
    if (!TryNumeric(i, Numeric::Scalar16)) throw Rejected{BuildError::Unsupported};
}

void VisitFPIntegerConvert(const vixl::aarch64::Instruction* instr) override {
    switch (instr->Mask(FPIntegerConvertMask)) {
        case FMOV_wh:
        case FMOV_xh: {
            auto value = this->template And<Type::U64>(ReadVHalf(instr->GetRn(), false),
                                                       SingleOperand(Imm{u64{0xFFFF}}))
                                 .WithView(Type::U64);
            WriteXRegister(instr->GetRd(), value);
            return;
        }
        case FMOV_ws:
            WriteWRegister(instr->GetRd(), ReadVHalf(instr->GetRn(), false));
            return;
        case FMOV_hw:
        case FMOV_hx: {
            auto value = this->template And<Type::U64>(ReadXRegister(instr->GetRn()),
                                                       SingleOperand(Imm{u64{0xFFFF}}))
                                 .WithView(Type::U64);
            WriteVRegister(instr->GetRd(), value);
            return;
        }
        case FMOV_sw:
            WriteVRegister(instr->GetRd(), ReadWRegister(instr->GetRn()));
            return;
        case FMOV_xd:
            WriteXRegister(instr->GetRd(), ReadVHalf(instr->GetRn(), false));
            return;
        case FMOV_x_d1:
            WriteXRegister(instr->GetRd(), ReadVHalf(instr->GetRn(), true));
            return;
        case FMOV_d1_x: {
            auto low = ReadVHalf(instr->GetRd(), false);
            auto high = ReadXRegister(instr->GetRn());
            WriteVHalves(instr->GetRd(), low, high);
            return;
        }
        case FMOV_dx:
            WriteVRegister(instr->GetRd(), ReadXRegister(instr->GetRn()));
            return;
        default:
            ScalarFPHelper(instr);
            return;
    }
}

void VisitFPImmediate(const vixl::aarch64::Instruction* instr) override {
    u64 bits;
    switch (instr->Mask(FPImmediateMask)) {
        case FMOV_h_imm: {
            const u64 imm8 = (instr->GetInstructionBits() >> 13) & 0xFF;
            bits = ((imm8 & 0x80) << 8) | (((imm8 ^ 0x40) & 0x40) << 8) |
                   ((imm8 & 0x40) ? 0x3000 : 0) | ((imm8 & 0x3F) << 6);
            break;
        }
        case FMOV_s_imm: {
            const float value = instr->GetImmFP32();
            u32 raw;
            std::memcpy(&raw, &value, sizeof(raw));
            bits = raw;
            break;
        }
        case FMOV_d_imm: {
            const double value = instr->GetImmFP64();
            std::memcpy(&bits, &value, sizeof(bits));
            break;
        }
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }
    WriteVRegister(instr->GetRd(), ImmValue(bits, Type::U64));
}

void VisitFPDataProcessing1Source(const vixl::aarch64::Instruction* instr) override {
    if (TryFRINTRange(instr)) return;
    const auto op = instr->Mask(FPDataProcessing1SourceMask);
    u32 bits;
    switch (op) {
        case FMOV_h:
        case FABS_h:
        case FNEG_h:
            bits = 16;
            break;
        case FMOV_s:
        case FABS_s:
        case FNEG_s:
            bits = 32;
            break;
        case FMOV_d:
        case FABS_d:
        case FNEG_d:
            bits = 64;
            break;
        default:
            ScalarFPHelper(instr);
            return;
    }
    const u64 lane_mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    const u64 sign = u64{1} << (bits - 1);
    const bool is_abs = op == FABS_h || op == FABS_s || op == FABS_d;
    const bool is_neg = op == FNEG_h || op == FNEG_s || op == FNEG_d;
    if (X64FP()) {
        const auto value =
                Emit<OpCode::ArmFPBitUnary, Type::V128>(ReadVRegister(instr->GetRn(), Type::V128),
                                                        Imm{layout.fpcr_offset},
                                                        Imm{instr->GetInstructionBits()});
        WriteVRegister(instr->GetRd(), value);
        return;
    }
    auto value = this->template And<Type::U64>(
                             ReadVHalf(instr->GetRn(), false),
                             SingleOperand(Imm{is_abs ? lane_mask & ~sign : lane_mask}))
                         .WithView(Type::U64);
    if (is_neg) {
        value = this->template Xor<Type::U64>(value, SingleOperand(Imm{sign})).WithView(Type::U64);
    }
    if (is_abs || is_neg) {
        // FEAT_AFP alternate handling leaves NaN signs unchanged. These
        // bit operations preserve signaling payloads and do not raise FPSR
        // exceptions or apply DN/FZ/rounding, just like the vector forms.
        auto original = this->template And<Type::U64>(ReadVHalf(instr->GetRn(), false),
                                                      SingleOperand(Imm{lane_mask}))
                                .WithView(Type::U64);
        const u64 exponent = bits == 16 ? 0x7C00 : bits == 32 ? 0x7F800000 : 0x7FF0000000000000;
        const u64 fraction = lane_mask & ~(sign | exponent);
        auto encoded_exponent =
                this->template And<Type::U64>(original, SingleOperand(Imm{exponent}))
                        .WithView(Type::U64);
        auto exponent_diff =
                this->template Xor<Type::U64>(encoded_exponent, SingleOperand(Imm{exponent}))
                        .WithView(Type::U64);
        auto all_exponent = this->TestZero(exponent_diff).WithView(Type::U8);
        auto payload = this->template And<Type::U64>(original, SingleOperand(Imm{fraction}))
                               .WithView(Type::U64);
        auto nonzero_payload = this->TestNotZero(payload).WithView(Type::U8);
        auto nan = this->template And<Type::U8>(all_exponent, SingleOperand(nonzero_payload))
                           .WithView(Type::U8);
        const Uniform control{u32(offsetof(GuestState, fpcr)), Type::U32};
        auto fpcr = this->template LoadUniform<Type::U32>(control);
        auto alternate = this->TestBit(fpcr, Imm{1}).WithView(Type::U8);
        auto preserve =
                this->template And<Type::U8>(alternate, SingleOperand(nan)).WithView(Type::U8);
        value = this->Select(preserve, original, value).WithView(Type::U64);
    }
    WriteVRegister(instr->GetRd(), value);
}

void WriteNZCV(Value source) {
    if (X64FP()) {
        // Numerical leaves return the architectural N:Z:C:V nibble.
        auto packed = this->template LslImm<Type::U64>(source, Imm{28u}).WithView(Type::U64);
        Emit<OpCode::RestoreFlags>(packed, Flag::NZCV);
        return;
    }
    auto bit = [&](u32 position) {
        auto shifted = this->template LsrImm<Type::U64>(source, Imm{position}).WithView(Type::U64);
        return this->template And<Type::U64>(shifted, SingleOperand(Imm{u64{1}}))
                .WithView(Type::U64);
    };
    auto n = this->template LslImm<Type::U64>(bit(3), Imm{63u}).WithView(Type::U64);
    n = this->template Sub<Type::U64>(ImmValue(0, Type::U64), SingleOperand(n)).WithView(Type::U64);
    auto z = this->template Xor<Type::U64>(bit(2), SingleOperand(Imm{u64{1}})).WithView(Type::U64);
    z = this->template Sub<Type::U64>(z, SingleOperand(Imm{u64{0}})).WithView(Type::U64);
    this->SaveFlags(n, Flag::Negate);
    this->SaveFlags(z, Flag::Zero);
    this->SetCarry(bit(1));
    this->SetOverflow(bit(0));
}

void VisitFPDataProcessing2Source(const vixl::aarch64::Instruction* instr) override {
    if (TryNativeScalarFP(instr)) return;
    ScalarFPHelper(instr);
}

void VisitFPFixedPointConvert(const vixl::aarch64::Instruction* instr) override {
    ScalarFPHelper(instr);
}

void VisitFPDataProcessing3Source(const vixl::aarch64::Instruction* instr) override {
    if (TryNativeScalarFMA(instr)) return;
    ScalarFPHelper(instr);
}

void VisitFPCompare(const vixl::aarch64::Instruction* instr) override {
    ScalarFPHelper(instr, true);
}

void VisitFPConditionalCompare(const vixl::aarch64::Instruction* instr) override {
    auto compare =
            this->Goto(CondPassed(static_cast<vixl::aarch64::Condition>(instr->GetCondition())));
    WriteNZCV(ImmValue(instr->GetNzcv(), Type::U64));
    auto done = this->Goto(Value{ImmValue(1, Type::U8)});
    this->BindLabel(compare);
    ScalarFPHelper(instr, true);
    this->BindLabel(done);
}

void VisitFPConditionalSelect(const vixl::aarch64::Instruction* instr) override {
    const u32 type = instr->GetFPType();
    if (type != 0 && type != 1 && type != 3) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const u32 bits = type == 0 ? 32 : type == 1 ? 64 : 16;
    auto condition = CondPassed(static_cast<vixl::aarch64::Condition>(instr->GetCondition()));
    auto value = this->Select(condition,
                              ReadVHalf(instr->GetRn(), false),
                              ReadVHalf(instr->GetRm(), false))
                         .WithView(Type::U64);
    if (bits < 64) {
        value = this->template And<Type::U64>(value, SingleOperand(Imm{(u64{1} << bits) - 1}))
                        .WithView(Type::U64);
    }
    WriteVRegister(instr->GetRd(), value);
}
