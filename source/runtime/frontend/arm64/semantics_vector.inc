void VisitNEONModifiedImmediate(const vixl::aarch64::Instruction* instr) override {
    const u32 cmode = instr->GetNEONCmode();
    const u32 op_bit = instr->GetNEONModImmOp();
    const u64 imm8 = instr->GetImmNEONabcdefgh();
    const u32 cmode_3_1 = (cmode >> 1) & 7;

    u64 lane{};
    u32 lane_bits{};
    switch (cmode_3_1) {
        case 0:
        case 1:
        case 2:
        case 3:
            lane_bits = 32;
            lane = imm8 << (8 * cmode_3_1);
            break;
        case 4:
        case 5:
            lane_bits = 16;
            lane = imm8 << (8 * ((cmode >> 1) & 1));
            break;
        case 6:
            lane_bits = 32;
            lane = (cmode & 1) ? ((imm8 << 16) | 0xFFFF) : ((imm8 << 8) | 0xFF);
            break;
        case 7:
            if (cmode == 14 && op_bit == 0) {
                lane_bits = 8;
                lane = imm8;
            } else if (cmode == 14) {
                lane_bits = 64;
                for (u32 i = 0; i < 8; ++i) {
                    if (imm8 & (u64{1} << i)) lane |= u64{0xFF} << (i * 8);
                }
            } else if (op_bit == 0) {
                lane_bits = 32;
                lane = ((imm8 & 0x80) << 24) | (((imm8 ^ 0x40) & 0x40) << 24) |
                       ((imm8 & 0x40) ? 0x3E000000 : 0) | ((imm8 & 0x3F) << 19);
            } else if (instr->GetNEONQ()) {
                lane_bits = 64;
                lane = ((imm8 & 0x80) << 56) | (((imm8 ^ 0x40) & 0x40) << 56) |
                       ((imm8 & 0x40) ? u64{0x3FC0000000000000} : 0) | ((imm8 & 0x3F) << 48);
            } else {
                Interrupt(InterruptReason::ILL_CODE, current_pc);
                return;
            }
            break;
        default:
            throw Rejected{BuildError::InvalidEncoding};
    }

    u64 fill;
    switch (lane_bits) {
        case 8:
            fill = lane * u64(0x0101010101010101);
            break;
        case 16:
            fill = lane * u64(0x0001000100010001);
            break;
        case 32:
            fill = lane | (lane << 32);
            break;
        case 64:
            fill = lane;
            break;
        default:
            throw Rejected{BuildError::InvalidEncoding};
    }

    const bool is_logic = cmode < 12 && (cmode & 1) != 0;
    const bool invert_fill = cmode < 14 && op_bit;
    if (!is_logic) {
        const u64 low_bits = invert_fill ? ~fill : fill;
        const u64 high_bits = instr->GetNEONQ() ? low_bits : 0;
        auto vec = (low_bits == 0 && high_bits == 0)
                           ? this->VecSharedZero().WithView(Type::V128)
                           : this->VecLoadConst(Imm{low_bits}, Imm{high_bits}).WithView(Type::V128);
        WriteQRegister(instr->GetRd(), vec);
        return;
    }
    const u64 mask_bits = op_bit ? ~fill : fill;
    if (instr->GetNEONQ()) {
        auto old = ReadVRegister(instr->GetRd(), Type::V128);
        auto cvec = this->VecLoadConst(Imm{mask_bits}, Imm{mask_bits}).WithView(Type::V128);
        auto res = (op_bit ? this->VecAnd(old, cvec) : this->VecOr(old, cvec)).WithView(Type::V128);
        WriteQRegister(instr->GetRd(), res);
        return;
    }
    auto old = ReadVHalf(instr->GetRd(), false);
    auto low = op_bit ? this->template And<Type::U64>(old, SingleOperand(Imm{mask_bits}))
                                .WithView(Type::U64)
                      : this->template Or<Type::U64>(old, SingleOperand(Imm{mask_bits}))
                                .WithView(Type::U64);
    WriteDRegister(instr->GetRd(), low);
}

void VisitNEON2RegMisc(const vixl::aarch64::Instruction* instr) override {
    if (TryNEONMixedSaturating(instr, false)) return;
    if (TryNEONPairwiseWiden(instr)) return;
    if (TryFRINTRange(instr)) return;
    if (TryNEONUnsignedEstimate(instr)) return;
    if (TryNEONFPEstimate(instr, false)) return;
    if (TryNEONFPPrecision(instr)) return;
    if (TryNEONFPInteger(instr, false)) return;
    if (TryNEONFPCompare(instr, false, true)) return;
    if (TryNEONVectorFPUnary(instr)) return;
    if (TryNEONBitMisc(instr)) return;
    if (TryNEONInteger(instr, true)) return;
    Interrupt(InterruptReason::FALLBACK, current_pc);
}

void VisitNEON3Same(const vixl::aarch64::Instruction* instr) override {
    if (TryNEONHalving(instr)) return;
    if (TryNEONAbsoluteNarrow(instr)) return;
    if (TryNEONPolynomial(instr, false)) return;
    if (TryNEONFPReduction(instr, false, false)) return;
    if (TryNEONFPCompare(instr, false, false)) return;
    if (TryNEONExtendedFP(instr, false)) return;
    if (TryNEONVectorFMA(instr, false)) return;
    if (TryNEONVectorFPBinary(instr)) return;
    if (TryNEONSaturatingMultiplyHigh(instr, false, false, false)) return;
    if (TryNEONSaturatingArithmetic(instr)) return;
    if (TryNEONVariableShift(instr)) return;
    if (TryNEONInteger(instr, false)) return;
    Interrupt(InterruptReason::FALLBACK, current_pc);
}

void VisitNEON2RegMiscFP16(const vixl::aarch64::Instruction* instr) override {
    if (TryNEONFPEstimate(instr, true)) return;
    if (TryNEONFPInteger(instr, true)) return;
    if (TryNEONFPCompare(instr, true, true)) return;
    if (TryNEONFP16Unary(instr)) return;
    const auto op = instr->Mask(NEON2RegMiscFP16Mask);
    if (op != NEON_FABS_H && op != NEON_FNEG_H) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    auto input = ReadVRegister(instr->GetRn(), Type::V128);
    auto constant = [&](u64 half) {
        return this->VecLoadConst(Imm{half}, Imm{half}).WithView(Type::V128);
    };
    auto signs = constant(0x8000800080008000);
    auto magnitude = this->VecAndNot(input, signs).WithView(Type::V128);
    // FEAT_AFP alternate handling preserves the original sign of every NaN,
    // including signaling NaNs. Neither operation processes FP exceptions,
    // flushes subnormals, quiets NaNs nor consults the rounding mode.
    auto nan =
            this->VecCmpGt(magnitude, constant(0x7C007C007C007C00), Imm{16}).WithView(Type::V128);
    auto fpcr = this->template LoadUniform<Type::U32>(
            Uniform{u32(offsetof(GuestState, fpcr)), Type::U32});
    auto alternate = this->TestBit(fpcr, Imm{1}).WithView(Type::U8);
    auto preserved = this->Select(alternate, nan, this->VecSharedZero().WithView(Type::V128))
                             .WithView(Type::V128);
    signs = this->VecAndNot(signs, preserved).WithView(Type::V128);
    auto result = (op == NEON_FABS_H ? this->VecAndNot(input, signs) : this->VecXor(input, signs))
                          .WithView(Type::V128);
    if (!instr->GetNEONQ()) result = Reinterpret(result, Type::V64);
    WriteVRegister(instr->GetRd(), result);
}
