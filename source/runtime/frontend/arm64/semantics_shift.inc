void VisitNEONShiftImmediate(const vixl::aarch64::Instruction* instr) override {
    if (TryNEONFPFixedSIMD(instr)) return;
    if (TryNEONVectorSaturatingShift(instr)) return;
    if (TryNEONShiftImmediate(instr)) return;
    const int highest_bit = HighestBit(instr->GetImmNEONImmh());
    const int right_shift = (16 << highest_bit) - instr->GetImmNEONImmhImmb();
    if (instr->Mask(NEONShiftImmediateMask) != NEON_SHRN) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    if (highest_bit < 0 || highest_bit > 2) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }

    auto low = ReadVHalf(instr->GetRn(), false);
    auto high = ReadVHalf(instr->GetRn(), true);
    const u32 lane_bits = 8u << highest_bit;
    const u32 source_bits = lane_bits * 2;
    const u32 source_lanes_per_half = 64 / source_bits;
    auto packed = ImmValue(0, Type::U64);
    for (u32 i = 0; i < 64 / lane_bits; ++i) {
        auto source = i < source_lanes_per_half ? low : high;
        auto lane = this->template LsrImm<Type::U64>(
                                source,
                                Imm{u32((i % source_lanes_per_half) * source_bits + right_shift)})
                            .WithView(Type::U64);
        lane = this->template And<Type::U64>(lane, SingleOperand(Imm{(u64{1} << lane_bits) - 1}))
                       .WithView(Type::U64);
        if (i != 0) {
            lane = this->template LslImm<Type::U64>(lane, Imm{i * lane_bits}).WithView(Type::U64);
        }
        packed = this->template Or<Type::U64>(packed, SingleOperand(lane)).WithView(Type::U64);
    }
    if (instr->GetNEONQ()) {
        WriteVHalves(instr->GetRd(), ReadVHalf(instr->GetRd(), false), packed);
    } else {
        WriteVHalves(instr->GetRd(), packed, ImmValue(0, Type::U64));
    }
}

bool TryNEONShiftImmediate(const vixl::aarch64::Instruction* instr) {
    const auto op = instr->Mask(NEONShiftImmediateMask);
    const bool widen = op == NEON_SSHLL || op == NEON_USHLL;
    const bool narrow = op == NEON_RSHRN;
    const bool left = op == NEON_SHL || op == NEON_SLI || widen;
    const bool insert = op == NEON_SLI || op == NEON_SRI;
    const bool sign = op == NEON_SSHR || op == NEON_SSRA || op == NEON_SRSHR || op == NEON_SRSRA;
    const bool rounded =
            op == NEON_SRSHR || op == NEON_URSHR || op == NEON_SRSRA || op == NEON_URSRA || narrow;
    const bool accumulate =
            op == NEON_SSRA || op == NEON_USRA || op == NEON_SRSRA || op == NEON_URSRA;
    if (!left && !insert && !sign && !rounded && !accumulate && op != NEON_USHR) return false;
    const int highest = HighestBit(instr->GetImmNEONImmh());
    if (highest < 0 || highest > 3 || ((widen || narrow) && highest > 2)) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    const u32 bits = 8u << highest;
    const u32 encoded = instr->GetImmNEONImmhImmb();
    const u32 shift = left ? encoded - bits : 2 * bits - encoded;
    const bool q = instr->GetNEONQ();
    if (widen || narrow) {
        const u32 source_bits = narrow ? bits * 2 : bits;
        const u32 result_bits = narrow ? bits : bits * 2;
        auto input_low = ReadVHalf(instr->GetRn(), widen && q);
        auto input_high = narrow ? ReadVHalf(instr->GetRn(), true) : input_low;
        const u64 result_mask = result_bits == 64 ? ~u64{0} : (u64{1} << result_bits) - 1;
        const u64 source_mask = source_bits == 64 ? ~u64{0} : (u64{1} << source_bits) - 1;
        auto low = ImmValue(0, Type::U64), high = low;
        for (u32 lane = 0; lane < (narrow ? 64u : 128u) / result_bits; ++lane) {
            const u32 source_position = lane * source_bits;
            auto value = this->LsrImm(source_position < 64 ? input_low : input_high,
                                      Imm{source_position % 64})
                                 .WithView(Type::U64);
            value = this->And(value, SingleOperand(Imm{source_mask})).WithView(Type::U64);
            if (narrow) {
                auto round = this->LsrImm(value, Imm{shift - 1}).WithView(Type::U64);
                round = this->And(round, SingleOperand(Imm{u64{1}})).WithView(Type::U64);
                value = this->LsrImm(value, Imm{shift}).WithView(Type::U64);
                value = this->Add(value, SingleOperand(round)).WithView(Type::U64);
            } else {
                if (op == NEON_SSHLL) value = SignExtendValue(value, bits);
                value = this->LslImm(value, Imm{shift}).WithView(Type::U64);
            }
            value = this->And(value, SingleOperand(Imm{result_mask})).WithView(Type::U64);
            value = this->LslImm(value, Imm{lane * result_bits % 64}).WithView(Type::U64);
            auto& half = lane * result_bits < 64 ? low : high;
            half = this->Or(half, SingleOperand(value)).WithView(Type::U64);
        }
        if (narrow && q)
            WriteVHalves(instr->GetRd(), ReadVHalf(instr->GetRd(), false), low);
        else
            WriteVHalves(instr->GetRd(), low, high);
        return true;
    }
    if (bits == 64 && !q) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    auto input = ReadVRegister(instr->GetRn(), Type::V128);
    auto result = left   ? this->VecShiftLeftImm(input, Imm{shift}, Imm{bits})
                  : sign ? this->VecShiftRightArithmeticImm(input, Imm{shift}, Imm{bits})
                         : this->VecShiftRightImm(input, Imm{shift}, Imm{bits});
    result.WithView(Type::V128);
    auto constant = [&](u64 lane) {
        u64 half{};
        for (u32 position = 0; position < 64; position += bits) half |= lane << position;
        return this->VecLoadConst(Imm{half}, Imm{half}).WithView(Type::V128);
    };
    if (rounded) {
        // Quotient plus the last discarded bit avoids overflowing the source lane.
        auto round = this->VecShiftRightImm(input, Imm{shift - 1}, Imm{bits}).WithView(Type::V128);
        round = this->VecAnd(round, constant(1)).WithView(Type::V128);
        result = this->VecAdd(result, round, Imm{bits}).WithView(Type::V128);
    }
    if (accumulate || insert) {
        auto old = ReadVRegister(instr->GetRd(), Type::V128);
        if (accumulate)
            result = this->VecAdd(old, result, Imm{bits}).WithView(Type::V128);
        else {
            const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
            const u64 keep = left ? (shift == 0 ? 0 : (u64{1} << shift) - 1)
                                  : (shift == bits ? mask : mask ^ (mask >> shift));
            old = this->VecAnd(old, constant(keep)).WithView(Type::V128);
            result = this->VecOr(old, result).WithView(Type::V128);
        }
    }
    if (!q) result = Reinterpret(result, Type::V64);
    WriteVRegister(instr->GetRd(), result);
    return true;
}

bool TryNEONVariableShift(const vixl::aarch64::Instruction* instr, bool scalar = false) {
    // Scalar encodings share the vector opcode after removing their class bit.
    const auto op = (instr->GetInstructionBits() & ~u32(NEONScalar)) & NEON3SameMask;
    const bool sign = op == NEON_SSHL || op == NEON_SRSHL || op == NEON_SQSHL || op == NEON_SQRSHL;
    const bool rounded =
            op == NEON_SRSHL || op == NEON_URSHL || op == NEON_SQRSHL || op == NEON_UQRSHL;
    const bool saturating =
            op == NEON_SQSHL || op == NEON_UQSHL || op == NEON_SQRSHL || op == NEON_UQRSHL;
    if (!sign && !rounded && !saturating && op != NEON_USHL) return false;
    const u32 bits = 8u << instr->GetNEONSize();
    const bool q = instr->GetNEONQ();
    if ((scalar && !saturating && bits != 64) || (!scalar && bits == 64 && !q)) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    auto input_low = ReadVHalf(instr->GetRn(), false), input_high = ReadVHalf(instr->GetRn(), true);
    auto count_low = ReadVHalf(instr->GetRm(), false), count_high = ReadVHalf(instr->GetRm(), true);
    auto low = ImmValue(0, Type::U64), high = low;
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    auto zero = ImmValue(0, Type::U64);
    auto any_saturation = ImmValue(0, Type::U8);
    const u32 active = scalar ? 1 : (q ? 128u : 64u) / bits;
    for (u32 lane = 0; lane < active; ++lane) {
        const u32 position = lane * bits;
        auto input = this->LsrImm(position < 64 ? input_low : input_high, Imm{position % 64})
                             .WithView(Type::U64);
        input = this->And(input, SingleOperand(Imm{mask})).WithView(Type::U64);
        auto count = this->LsrImm(position < 64 ? count_low : count_high, Imm{position % 64})
                             .WithView(Type::U64);
        count = SignExtendValue(count, 8);
        auto right = this->TestBit(count, Imm{63u}).WithView(Type::U8);
        auto negative = this->Sub(zero, SingleOperand(count)).WithView(Type::U64);
        auto magnitude = this->Select(right, negative, count).WithView(Type::U64);
        auto difference = this->Sub(magnitude, SingleOperand(Imm{u64{bits}})).WithView(Type::U64);
        auto in_range = this->TestBit(difference, Imm{63u}).WithView(Type::U8);
        auto shifted_left = this->LslValue(input, magnitude).WithView(Type::U64);
        shifted_left = this->Select(in_range, shifted_left, zero).WithView(Type::U64);
        if (saturating) {
            auto narrowed = this->And(shifted_left, SingleOperand(Imm{mask})).WithView(Type::U64);
            auto original = sign ? SignExtendValue(input, bits) : input;
            auto extended = sign ? SignExtendValue(narrowed, bits) : narrowed;
            auto restored = sign ? this->AsrValue(extended, magnitude)
                                 : this->LsrValue(extended, magnitude);
            restored.WithView(Type::U64);
            auto mismatch = this->Xor(restored, SingleOperand(original)).WithView(Type::U64);
            auto changed = this->TestNotZero(mismatch).WithView(Type::U8);
            auto nonzero = this->TestNotZero(input).WithView(Type::U8);
            auto overflow = this->Select(in_range, changed, nonzero).WithView(Type::U8);
            overflow = this->Select(right, ImmValue(0, Type::U8), overflow).WithView(Type::U8);
            any_saturation = this->Or(any_saturation, SingleOperand(overflow)).WithView(Type::U8);
            auto limit = ImmValue(mask, Type::U64);
            if (sign) {
                auto negative_input = this->TestBit(input, Imm{bits - 1}).WithView(Type::U8);
                limit = this->Select(negative_input,
                                     ImmValue(u64{1} << (bits - 1), Type::U64),
                                     ImmValue((u64{1} << (bits - 1)) - 1, Type::U64))
                                .WithView(Type::U64);
            }
            shifted_left = this->Select(overflow, limit, narrowed).WithView(Type::U64);
        }
        Value shifted_right;
        if (sign) {
            auto signed_input = SignExtendValue(input, bits);
            auto amount = this->Select(in_range, magnitude, ImmValue(bits - 1, Type::U64))
                                  .WithView(Type::U64);
            shifted_right = this->AsrValue(signed_input, amount).WithView(Type::U64);
        } else {
            shifted_right = this->LsrValue(input, magnitude).WithView(Type::U64);
            shifted_right = this->Select(in_range, shifted_right, zero).WithView(Type::U64);
        }
        if (rounded) {
            auto last_bit = this->Sub(magnitude, SingleOperand(Imm{u64{1}})).WithView(Type::U64);
            auto round = this->LsrValue(input, last_bit).WithView(Type::U64);
            round = this->And(round, SingleOperand(Imm{u64{1}})).WithView(Type::U64);
            shifted_right = this->Add(shifted_right, SingleOperand(round)).WithView(Type::U64);
            auto below_next =
                    this->Sub(magnitude, SingleOperand(Imm{u64{bits + 1}})).WithView(Type::U64);
            auto round_range = this->TestBit(below_next, Imm{63u}).WithView(Type::U8);
            shifted_right = this->Select(round_range, shifted_right, zero).WithView(Type::U64);
        }
        auto value = this->Select(right, shifted_right, shifted_left).WithView(Type::U64);
        value = this->And(value, SingleOperand(Imm{mask})).WithView(Type::U64);
        value = this->LslImm(value, Imm{position % 64}).WithView(Type::U64);
        auto& half = position < 64 ? low : high;
        half = this->Or(half, SingleOperand(value)).WithView(Type::U64);
    }
    WriteVHalves(instr->GetRd(), low, high);
    if (saturating) {
        Uniform status{u32(offsetof(GuestState, fpsr)), Type::U32};
        auto old = this->template LoadUniform<Type::U32>(status);
        auto qc = this->Select(any_saturation,
                               ImmValue(u64{1} << 27, Type::U32),
                               ImmValue(0, Type::U32))
                          .WithView(Type::U32);
        this->StoreUniform(status, this->template Or<Type::U32>(old, SingleOperand(qc)));
    }
    return true;
}

void VisitNEONScalarShiftImmediate(const vixl::aarch64::Instruction* instr) override {
    if (TryNEONFPFixedSIMD(instr)) return;
    const auto op = instr->Mask(NEONScalarShiftImmediateMask);
    const bool narrow = op == NEON_UQSHRN_scalar || op == NEON_UQRSHRN_scalar ||
                        op == NEON_SQSHRN_scalar || op == NEON_SQRSHRN_scalar ||
                        op == NEON_SQSHRUN_scalar || op == NEON_SQRSHRUN_scalar;
    const bool saturating_left =
            op == NEON_SQSHL_imm_scalar || op == NEON_UQSHL_imm_scalar || op == NEON_SQSHLU_scalar;
    const bool ordinary_left = op == NEON_SHL_scalar || op == NEON_SLI_scalar;
    const bool insert = op == NEON_SLI_scalar || op == NEON_SRI_scalar;
    const bool signed_right = op == NEON_SSHR_scalar || op == NEON_SRSHR_scalar ||
                              op == NEON_SSRA_scalar || op == NEON_SRSRA_scalar ||
                              op == NEON_SQSHRN_scalar || op == NEON_SQRSHRN_scalar ||
                              op == NEON_SQSHRUN_scalar || op == NEON_SQRSHRUN_scalar;
    const bool rounded = op == NEON_SRSHR_scalar || op == NEON_URSHR_scalar ||
                         op == NEON_SRSRA_scalar || op == NEON_URSRA_scalar ||
                         op == NEON_UQRSHRN_scalar || op == NEON_SQRSHRN_scalar ||
                         op == NEON_SQRSHRUN_scalar;
    const bool accumulate = op == NEON_SSRA_scalar || op == NEON_USRA_scalar ||
                            op == NEON_SRSRA_scalar || op == NEON_URSRA_scalar;
    if (!narrow && !saturating_left && !ordinary_left && !insert && !signed_right && !rounded &&
        !accumulate && op != NEON_USHR_scalar) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    const int highest = HighestBit(instr->GetImmNEONImmh());
    if (highest < 0 || highest > 3 || (narrow && highest > 2) ||
        (!narrow && !saturating_left && highest != 3)) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const u32 bits = 8u << highest, source_bits = narrow ? bits * 2 : bits;
    const bool left = saturating_left || ordinary_left;
    const u32 encoded = instr->GetImmNEONImmhImmb();
    const u32 shift = left ? encoded - bits : 2 * bits - encoded;
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    const u64 source_mask = source_bits == 64 ? ~u64{0} : (u64{1} << source_bits) - 1;
    auto input = this->template And<Type::U64>(ReadVHalf(instr->GetRn(), false),
                                               SingleOperand(Imm{source_mask}))
                         .WithView(Type::U64);
    auto old = (insert || accumulate) ? ReadVHalf(instr->GetRd(), false) : ImmValue(0, Type::U64);
    auto zero = ImmValue(0, Type::U64);
    Value result, saturated;
    if (left) {
        result = this->template LslImm<Type::U64>(input, Imm{shift}).WithView(Type::U64);
        result =
                this->template And<Type::U64>(result, SingleOperand(Imm{mask})).WithView(Type::U64);
        if (saturating_left) {
            const bool signed_result = op == NEON_SQSHL_imm_scalar;
            auto original = signed_result ? SignExtendValue(input, bits) : input;
            auto shifted = signed_result ? SignExtendValue(result, bits) : result;
            auto recovered = signed_result ? this->template AsrImm<Type::U64>(shifted, Imm{shift})
                                                     .WithView(Type::U64)
                                           : this->template LsrImm<Type::U64>(shifted, Imm{shift})
                                                     .WithView(Type::U64);
            auto changed = this->template Xor<Type::U64>(original, SingleOperand(recovered))
                                   .WithView(Type::U64);
            saturated = this->TestNotZero(changed).WithView(Type::U8);
            auto negative = this->TestBit(input, Imm{bits - 1}).WithView(Type::U8);
            Value limit = ImmValue(mask, Type::U64);
            if (signed_result) {
                limit = this->Select(negative,
                                     ImmValue(u64{1} << (bits - 1), Type::U64),
                                     ImmValue((u64{1} << (bits - 1)) - 1, Type::U64))
                                .WithView(Type::U64);
            } else if (op == NEON_SQSHLU_scalar) {
                // Negative signed inputs saturate to zero, including shift=0.
                saturated = this->template Or<Type::U8>(saturated, SingleOperand(negative))
                                    .WithView(Type::U8);
                limit = this->Select(negative, zero, limit).WithView(Type::U64);
            }
            result = this->Select(saturated, limit, result).WithView(Type::U64);
        } else if (insert) {
            const u64 keep = shift == 0 ? 0 : (u64{1} << shift) - 1;
            auto preserved = this->template And<Type::U64>(old, SingleOperand(Imm{keep}))
                                     .WithView(Type::U64);
            result = this->template Or<Type::U64>(preserved, SingleOperand(result))
                             .WithView(Type::U64);
        }
    } else {
        auto extended = signed_right ? SignExtendValue(input, source_bits) : input;
        if (signed_right) {
            // IR/x64 immediates mask a shift of 64; explicitly use the sign fill.
            result = this->template AsrImm<Type::U64>(extended, Imm{shift == 64 ? 63u : shift})
                             .WithView(Type::U64);
        } else if (shift == 64)
            result = zero;
        else
            result = this->template LsrImm<Type::U64>(extended, Imm{shift}).WithView(Type::U64);
        if (rounded) {
            auto round =
                    this->template LsrImm<Type::U64>(input, Imm{shift - 1}).WithView(Type::U64);
            round = this->template And<Type::U64>(round, SingleOperand(Imm{u64{1}}))
                            .WithView(Type::U64);
            result =
                    this->template Add<Type::U64>(result, SingleOperand(round)).WithView(Type::U64);
        }
        if (narrow) {
            const bool signed_result = op == NEON_SQSHRN_scalar || op == NEON_SQRSHRN_scalar;
            auto trimmed = this->template And<Type::U64>(result, SingleOperand(Imm{mask}))
                                   .WithView(Type::U64);
            if (signed_result) trimmed = SignExtendValue(trimmed, bits);
            auto changed = this->template Xor<Type::U64>(result, SingleOperand(trimmed))
                                   .WithView(Type::U64);
            saturated = this->TestNotZero(changed).WithView(Type::U8);
            auto limit = ImmValue(mask, Type::U64);
            if (signed_right) {
                auto negative = this->TestBit(result, Imm{63u}).WithView(Type::U8);
                limit = this->Select(negative,
                                     ImmValue(signed_result ? u64{1} << (bits - 1) : 0, Type::U64),
                                     ImmValue(signed_result ? (u64{1} << (bits - 1)) - 1 : mask,
                                              Type::U64))
                                .WithView(Type::U64);
            }
            result = this->Select(saturated, limit, result).WithView(Type::U64);
        } else if (accumulate) {
            result = this->template Add<Type::U64>(old, SingleOperand(result)).WithView(Type::U64);
        } else if (insert) {
            const u64 keep = shift == 64 ? ~u64{0} : mask ^ (mask >> shift);
            auto preserved = this->template And<Type::U64>(old, SingleOperand(Imm{keep}))
                                     .WithView(Type::U64);
            result = this->template Or<Type::U64>(preserved, SingleOperand(result))
                             .WithView(Type::U64);
        }
    }
    if (narrow || saturating_left) {
        Uniform status{u32(offsetof(GuestState, fpsr)), Type::U32};
        auto previous = this->template LoadUniform<Type::U32>(status);
        auto qc = this->Select(saturated, ImmValue(1u << 27, Type::U32), ImmValue(0, Type::U32))
                          .WithView(Type::U32);
        this->StoreUniform(
                status,
                this->template Or<Type::U32>(previous, SingleOperand(qc)).WithView(Type::U32));
    }
    result = this->template And<Type::U64>(result, SingleOperand(Imm{mask})).WithView(Type::U64);
    WriteVHalves(instr->GetRd(), result, zero);
}

bool TryNEONVectorSaturatingShift(const vixl::aarch64::Instruction* instr) {
    const auto op = instr->Mask(NEONShiftImmediateMask);
    bool narrow;
    switch (op) {
        case NEON_SQSHL_imm:
        case NEON_UQSHL_imm:
        case NEON_SQSHLU:
            narrow = false;
            break;
        case NEON_SQSHRN:
        case NEON_UQSHRN:
        case NEON_SQSHRUN:
        case NEON_SQRSHRN:
        case NEON_UQRSHRN:
        case NEON_SQRSHRUN:
            narrow = true;
            break;
        default:
            return false;
    }
    const auto highest = HighestBit(instr->GetImmNEONImmh());
    if (highest < 0 || highest > (narrow ? 2 : 3) ||
        (!narrow && highest == 3 && !instr->GetNEONQ()))
        throw Rejected{BuildError::InvalidEncoding};
    Helper(Numeric::SatShift);
    return true;
}
