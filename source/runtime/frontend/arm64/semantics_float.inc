
void ScalarFPHelper(const vixl::aarch64::Instruction* instr, bool comparison = false) {
    if (!runtime::arm64::IsSupportedScalarFP(instr->GetInstructionBits()))
        throw Rejected{BuildError::Unsupported};
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
            WriteVHalves(instr->GetRd(), value, ImmValue(0, Type::U64));
            return;
        }
        case FMOV_sw:
            WriteVHalves(
                    instr->GetRd(), Widen(ReadWRegister(instr->GetRn())), ImmValue(0, Type::U64));
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
            WriteVHalves(instr->GetRd(), ReadXRegister(instr->GetRn()), ImmValue(0, Type::U64));
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
    WriteVHalves(instr->GetRd(), ImmValue(bits, Type::U64), ImmValue(0, Type::U64));
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
    WriteVHalves(instr->GetRd(), value, ImmValue(0, Type::U64));
}

void WriteNZCV(Value source) {
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
    ScalarFPHelper(instr);
}

void VisitFPFixedPointConvert(const vixl::aarch64::Instruction* instr) override {
    ScalarFPHelper(instr);
}

void VisitFPDataProcessing3Source(const vixl::aarch64::Instruction* instr) override {
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
    WriteVHalves(instr->GetRd(), value, ImmValue(0, Type::U64));
}
