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
    if (instr->Mask(NEON2RegMiscMask) == NEON_RBIT_NOT && instr->GetNEONSize() == 0) {
        auto invert = [&](bool high) {
            return this
                    ->template Xor<Type::U64>(ReadVHalf(instr->GetRn(), high),
                                              SingleOperand(Imm{~u64{0}}))
                    .WithView(Type::U64);
        };
        auto low = invert(false);
        auto high = instr->GetNEONQ() ? invert(true) : ImmValue(0, Type::U64);
        WriteVHalves(instr->GetRd(), low, high);
        return;
    }
    if (instr->Mask(NEON2RegMiscMask) != NEON_CMEQ_zero || instr->GetNEONSize() != 0) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    auto rd = u8(instr->GetRd());
    auto rn = u8(instr->GetRn());
    auto low = ZeroByteMask(ReadVHalf(rn, false));
    auto high = instr->GetNEONQ() ? ZeroByteMask(ReadVHalf(rn, true)) : ImmValue(0, Type::U64);
    WriteVHalves(rd, low, high);
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
    const auto logical_op = instr->Mask(NEON3SameLogicalMask);
    switch (logical_op) {
        case NEON_AND:
        case NEON_ORR:
        case NEON_ORN:
        case NEON_EOR:
        case NEON_BIC:
        case NEON_BIF:
        case NEON_BIT:
        case NEON_BSL: {
            auto apply = [&](bool high) {
                auto left = ReadVHalf(instr->GetRn(), high);
                auto right = ReadVHalf(instr->GetRm(), high);
                auto invert = [&](Value value) {
                    return this->template Xor<Type::U64>(value, SingleOperand(Imm{~u64{0}}))
                            .WithView(Type::U64);
                };
                if (logical_op == NEON_AND || logical_op == NEON_BIC) {
                    return this
                            ->template And<Type::U64>(
                                    left,
                                    SingleOperand(logical_op == NEON_BIC ? invert(right) : right))
                            .WithView(Type::U64);
                }
                if (logical_op == NEON_ORR || logical_op == NEON_ORN) {
                    return this
                            ->template Or<Type::U64>(
                                    left,
                                    SingleOperand(logical_op == NEON_ORN ? invert(right) : right))
                            .WithView(Type::U64);
                }
                if (logical_op == NEON_EOR) {
                    return this->template Xor<Type::U64>(left, SingleOperand(right))
                            .WithView(Type::U64);
                }
                auto old = ReadVHalf(instr->GetRd(), high);
                auto mask = logical_op == NEON_BSL ? old : right;
                if (logical_op == NEON_BIF) mask = invert(mask);
                auto alternate = logical_op == NEON_BSL ? right : old;
                auto selected = this->template And<Type::U64>(left, SingleOperand(mask))
                                        .WithView(Type::U64);
                auto kept = this->template And<Type::U64>(alternate, SingleOperand(invert(mask)))
                                    .WithView(Type::U64);
                return this->template Or<Type::U64>(selected, SingleOperand(kept))
                        .WithView(Type::U64);
            };
            // Capture both source halves before publishing an aliased destination.
            auto low = apply(false);
            auto high = instr->GetNEONQ() ? apply(true) : ImmValue(0, Type::U64);
            WriteVHalves(instr->GetRd(), low, high);
            return;
        }
        default:
            break;
    }
    const auto arithmetic_op = instr->Mask(NEON3SameMask);
    if (arithmetic_op == NEON_ADD || arithmetic_op == NEON_SUB || arithmetic_op == NEON_MUL) {
        const u32 bits = 8u << instr->GetNEONSize();
        if (bits == 64 && (!instr->GetNEONQ() || arithmetic_op == NEON_MUL)) {
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return;
        }
        const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
        auto apply = [&](bool high) {
            auto left_half = ReadVHalf(instr->GetRn(), high);
            auto right_half = ReadVHalf(instr->GetRm(), high);
            auto packed = ImmValue(0, Type::U64);
            for (u32 shift = 0; shift < 64; shift += bits) {
                auto left =
                        this->template LsrImm<Type::U64>(left_half, Imm{shift}).WithView(Type::U64);
                auto right = this->template LsrImm<Type::U64>(right_half, Imm{shift})
                                     .WithView(Type::U64);
                left = this->template And<Type::U64>(left, SingleOperand(Imm{mask}))
                               .WithView(Type::U64);
                right = this->template And<Type::U64>(right, SingleOperand(Imm{mask}))
                                .WithView(Type::U64);
                auto lane = arithmetic_op == NEON_ADD
                                    ? this->template Add<Type::U64>(left, SingleOperand(right))
                            : arithmetic_op == NEON_SUB
                                    ? this->template Sub<Type::U64>(left, SingleOperand(right))
                                    : this->template Mul<Type::U64>(left, SingleOperand(right));
                lane.WithView(Type::U64);
                lane = this->template And<Type::U64>(lane, SingleOperand(Imm{mask}))
                               .WithView(Type::U64);
                if (shift)
                    lane = this->template LslImm<Type::U64>(lane, Imm{shift}).WithView(Type::U64);
                packed = this->template Or<Type::U64>(packed, SingleOperand(lane))
                                 .WithView(Type::U64);
            }
            return packed;
        };
        auto low = apply(false);
        auto high = instr->GetNEONQ() ? apply(true) : ImmValue(0, Type::U64);
        WriteVHalves(instr->GetRd(), low, high);
        return;
    }
    if (!instr->GetNEONQ()) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }

    auto rd = u8(instr->GetRd());
    auto rn = u8(instr->GetRn());
    auto rm = u8(instr->GetRm());
    if (instr->GetNEONSize() != 0) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }

    if (instr->Mask(NEON3SameMask) == NEON_CMEQ) {
        auto low = this->template Xor<Type::U64>(ReadVHalf(rn, false),
                                                 SingleOperand(ReadVHalf(rm, false)))
                           .WithView(Type::U64);
        auto high = this->template Xor<Type::U64>(ReadVHalf(rn, true),
                                                  SingleOperand(ReadVHalf(rm, true)))
                            .WithView(Type::U64);
        WriteVHalves(rd, ZeroByteMask(low), ZeroByteMask(high));
        return;
    }

    if (instr->Mask(NEON3SameMask) == NEON_CMHS) {
        auto compare_half = [&](bool high) {
            auto left_half = ReadVHalf(rn, high);
            auto right_half = ReadVHalf(rm, high);
            auto packed = ImmValue(0, Type::U64);
            for (u32 i = 0; i < 8; ++i) {
                auto left = this->template And<Type::U64>(
                                        this->template LsrImm<Type::U64>(left_half, Imm{i * 8}),
                                        SingleOperand(Imm{0xFF}))
                                    .WithView(Type::U64);
                auto right = this->template And<Type::U64>(
                                         this->template LsrImm<Type::U64>(right_half, Imm{i * 8}),
                                         SingleOperand(Imm{0xFF}))
                                     .WithView(Type::U64);
                auto diff = this->template Sub<Type::U64>(left, SingleOperand(right))
                                    .WithView(Type::U64);
                // Byte operands cannot overflow signed 64-bit subtraction.
                // Derive the comparison without publishing guest NZCV.
                auto negative =
                        this->template LsrImm<Type::U64>(diff, Imm{63u}).WithView(Type::U64);
                auto ge = this->template Xor<Type::U64>(negative, SingleOperand(Imm{u64{1}}))
                                  .WithView(Type::U64);
                auto lane = this->template Mul<Type::U64>(ge, SingleOperand(Imm{0xFF}))
                                    .WithView(Type::U64);
                if (i != 0) {
                    lane = this->template LslImm<Type::U64>(lane, Imm{i * 8}).WithView(Type::U64);
                }
                packed = this->template Or<Type::U64>(packed, SingleOperand(lane))
                                 .WithView(Type::U64);
            }
            return packed;
        };
        WriteVHalves(rd, compare_half(false), compare_half(true));
        return;
    }

    const auto op = instr->Mask(NEON3SameMask);
    if (op == NEON_ADDP || op == NEON_UMINP || op == NEON_UMAXP) {
        auto pack_pairs = [&](u8 source_code) {
            auto source_low = ReadVHalf(source_code, false);
            auto source_high = ReadVHalf(source_code, true);
            auto packed = ImmValue(0, Type::U64);
            for (u32 i = 0; i < 8; ++i) {
                auto source = i < 4 ? source_low : source_high;
                const u32 byte = (i % 4) * 2;
                auto left =
                        this->template LsrImm<Type::U64>(source, Imm{byte * 8}).WithView(Type::U64);
                auto right = this->template LsrImm<Type::U64>(source, Imm{(byte + 1) * 8})
                                     .WithView(Type::U64);
                left = this->template And<Type::U64>(left, SingleOperand(Imm{0xFF}))
                               .WithView(Type::U64);
                right = this->template And<Type::U64>(right, SingleOperand(Imm{0xFF}))
                                .WithView(Type::U64);
                Value lane;
                if (op == NEON_ADDP) {
                    lane = this->template Add<Type::U64>(left, SingleOperand(right))
                                   .WithView(Type::U64);
                } else {
                    auto diff = this->template Sub<Type::U64>(left, SingleOperand(right))
                                        .WithView(Type::U64);
                    auto negative =
                            this->template LsrImm<Type::U64>(diff, Imm{63u}).WithView(Type::U64);
                    auto left_ge = this->TestZero(negative);
                    left_ge.WithView(Type::U8);
                    lane = op == NEON_UMAXP
                                   ? this->Select(left_ge, left, right).WithView(Type::U64)
                                   : this->Select(left_ge, right, left).WithView(Type::U64);
                }
                lane = this->template And<Type::U64>(lane, SingleOperand(Imm{0xFF}))
                               .WithView(Type::U64);
                if (i != 0) {
                    lane = this->template LslImm<Type::U64>(lane, Imm{i * 8}).WithView(Type::U64);
                }
                packed = this->template Or<Type::U64>(packed, SingleOperand(lane))
                                 .WithView(Type::U64);
            }
            return packed;
        };
        WriteVHalves(rd, pack_pairs(rn), pack_pairs(rm));
        return;
    }

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
    auto result = op == NEON_FABS_H ? this->VecAndNot(input, signs) : this->VecXor(input, signs);
    result.WithView(Type::V128);
    if (!instr->GetNEONQ()) result = Reinterpret(result, Type::V64);
    WriteVRegister(instr->GetRd(), result);
}
