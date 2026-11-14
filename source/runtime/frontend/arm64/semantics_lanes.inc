Value EmitNEONScalar3SameShift(u32 op, Value a, Value b, Value zero) {
    auto count = SignExtendValue(b, 8);
    auto right = this->TestBit(count, Imm{63u}).WithView(Type::U8);
    auto negative = this->Sub(zero, SingleOperand(count)).WithView(Type::U64);
    auto amount = this->Select(right, negative, count).WithView(Type::U64);
    auto difference = this->Sub(amount, SingleOperand(Imm{u64{64}})).WithView(Type::U64);
    auto in_range = this->TestBit(difference, Imm{63u}).WithView(Type::U8);
    auto left_result = this->LslValue(a, amount).WithView(Type::U64);
    left_result = this->Select(in_range, left_result, zero).WithView(Type::U64);
    Value right_result;
    if (op == NEON_SSHL_scalar) {
        auto clamped = this->Select(in_range, amount, ImmValue(63, Type::U64)).WithView(Type::U64);
        right_result = this->AsrValue(a, clamped).WithView(Type::U64);
    } else {
        right_result = this->LsrValue(a, amount).WithView(Type::U64);
        right_result = this->Select(in_range, right_result, zero).WithView(Type::U64);
    }
    return this->Select(right, right_result, left_result).WithView(Type::U64);
}

void VisitNEONScalar3Same(const vixl::aarch64::Instruction* instr) override {
    if (TryNEONVariableShift(instr, true)) return;
    if (TryNEONFPCompare(instr, false, false)) return;
    if (TryNEONExtendedFP(instr, false)) return;
    if (TryNEONSaturatingMultiplyHigh(instr, true, false, false)) return;
    const auto op = instr->Mask(NEONScalar3SameMask);
    const bool saturation = op == NEON_SQADD_scalar || op == NEON_UQADD_scalar ||
                            op == NEON_SQSUB_scalar || op == NEON_UQSUB_scalar;
    const bool compare = op == NEON_CMEQ_scalar || op == NEON_CMGT_scalar ||
                         op == NEON_CMGE_scalar || op == NEON_CMHI_scalar ||
                         op == NEON_CMHS_scalar || op == NEON_CMTST_scalar;
    const bool shift = op == NEON_SSHL_scalar || op == NEON_USHL_scalar;
    const bool arithmetic = op == NEON_ADD_scalar || op == NEON_SUB_scalar;
    if (!saturation && !compare && !shift && !arithmetic) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    const u32 bits = 8u << instr->GetNEONSize();
    if (!saturation && bits != 64) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    auto a = ReadVHalf(instr->GetRn(), false), b = ReadVHalf(instr->GetRm(), false);
    a = this->And(a, SingleOperand(Imm{mask})).WithView(Type::U64);
    b = this->And(b, SingleOperand(Imm{mask})).WithView(Type::U64);
    auto zero = ImmValue(0, Type::U64);
    auto invert = [&](Value value) {
        return this->Xor(value, SingleOperand(Imm{mask})).WithView(Type::U64);
    };
    auto unsigned_less = [&](Value left, Value right) {
        auto difference = this->Sub(left, SingleOperand(right)).WithView(Type::U64);
        auto first = this->And(invert(left), SingleOperand(right)).WithView(Type::U64);
        auto changed = this->Xor(left, SingleOperand(right)).WithView(Type::U64);
        auto second = this->And(invert(changed), SingleOperand(difference)).WithView(Type::U64);
        auto borrow = this->Or(first, SingleOperand(second)).WithView(Type::U64);
        return this->TestBit(borrow, Imm{bits - 1}).WithView(Type::U8);
    };
    Value result;
    if (arithmetic || saturation) {
        const bool add =
                op == NEON_ADD_scalar || op == NEON_SQADD_scalar || op == NEON_UQADD_scalar;
        result = (add ? this->Add(a, SingleOperand(b)) : this->Sub(a, SingleOperand(b)))
                         .WithView(Type::U64);
        result = this->And(result, SingleOperand(Imm{mask})).WithView(Type::U64);
        if (saturation) {
            const bool sign = op == NEON_SQADD_scalar || op == NEON_SQSUB_scalar;
            Value overflow, limit;
            if (sign) {
                auto changed = this->Xor(a, SingleOperand(result)).WithView(Type::U64);
                auto other =
                        this->Xor(add ? b : a, SingleOperand(add ? result : b)).WithView(Type::U64);
                auto crossed = this->And(changed, SingleOperand(other)).WithView(Type::U64);
                overflow = this->TestBit(crossed, Imm{bits - 1}).WithView(Type::U8);
                auto negative = this->TestBit(a, Imm{bits - 1}).WithView(Type::U8);
                limit = this->Select(negative,
                                     ImmValue(u64{1} << (bits - 1), Type::U64),
                                     ImmValue((u64{1} << (bits - 1)) - 1, Type::U64))
                                .WithView(Type::U64);
            } else {
                overflow = add ? unsigned_less(result, a) : unsigned_less(a, b);
                limit = ImmValue(add ? mask : 0, Type::U64);
            }
            result = this->Select(overflow, limit, result).WithView(Type::U64);
            AccumulateQC(overflow);
        }
    } else if (compare) {
        Value condition;
        if (op == NEON_CMEQ_scalar) {
            auto unequal = this->Xor(a, SingleOperand(b)).WithView(Type::U64);
            condition = this->TestZero(unequal).WithView(Type::U8);
        } else if (op == NEON_CMTST_scalar) {
            auto common = this->And(a, SingleOperand(b)).WithView(Type::U64);
            condition = this->TestNotZero(common).WithView(Type::U8);
        } else {
            const bool sign = op == NEON_CMGT_scalar || op == NEON_CMGE_scalar;
            const bool equal = op == NEON_CMGE_scalar || op == NEON_CMHS_scalar;
            auto left = a, right = b;
            if (sign) {
                left = this->Xor(a, SingleOperand(Imm{u64{1} << 63})).WithView(Type::U64);
                right = this->Xor(b, SingleOperand(Imm{u64{1} << 63})).WithView(Type::U64);
            }
            condition = unsigned_less(equal ? left : right, equal ? right : left);
            if (equal)
                condition = this->Xor(condition, SingleOperand(Imm{u8{1}})).WithView(Type::U8);
        }
        result = this->Select(condition, ImmValue(mask, Type::U64), zero).WithView(Type::U64);
    } else {
        result = EmitNEONScalar3SameShift(op, a, b, zero);
    }
    WriteVRegister(instr->GetRd(), result);
}

void VisitNEONScalar2RegMisc(const vixl::aarch64::Instruction* instr) override {
    if (TryNEONMixedSaturating(instr, true)) return;
    if (TryNEONFPEstimate(instr, false)) return;
    if (TryNEONFPInteger(instr, false)) return;
    if (TryNEONFPCompare(instr, false, true)) return;
    const auto op = instr->Mask(NEONScalar2RegMiscMask);
    const bool narrow =
            op == NEON_SQXTN_scalar || op == NEON_UQXTN_scalar || op == NEON_SQXTUN_scalar;
    const bool absolute = op == NEON_ABS_scalar || op == NEON_SQABS_scalar;
    const bool negate = op == NEON_NEG_scalar || op == NEON_SQNEG_scalar;
    const bool saturate_unary = op == NEON_SQABS_scalar || op == NEON_SQNEG_scalar;
    const bool compare = op == NEON_CMGT_zero_scalar || op == NEON_CMGE_zero_scalar ||
                         op == NEON_CMEQ_zero_scalar || op == NEON_CMLT_zero_scalar ||
                         op == NEON_CMLE_zero_scalar;
    if (!narrow && !absolute && !negate && !compare) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    const u32 bits = 8u << instr->GetNEONSize();
    if ((narrow && bits == 64) || (!narrow && !saturate_unary && bits != 64)) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const u32 source_bits = narrow ? bits * 2 : bits;
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    const u64 source_mask = source_bits == 64 ? ~u64{0} : (u64{1} << source_bits) - 1;
    auto source = this->template And<Type::U64>(ReadVHalf(instr->GetRn(), false),
                                                SingleOperand(Imm{source_mask}))
                          .WithView(Type::U64);
    auto zero = ImmValue(0, Type::U64);
    Value result, saturated;
    if (compare) {
        auto negative = this->TestBit(source, Imm{63u}).WithView(Type::U8);
        auto is_zero = this->TestZero(source).WithView(Type::U8);
        Value condition;
        if (op == NEON_CMEQ_zero_scalar)
            condition = is_zero;
        else if (op == NEON_CMLT_zero_scalar)
            condition = negative;
        else if (op == NEON_CMGE_zero_scalar)
            condition = this->template Xor<Type::U8>(negative, SingleOperand(Imm{u8{1}}))
                                .WithView(Type::U8);
        else {
            condition = this->template Or<Type::U8>(negative, SingleOperand(is_zero))
                                .WithView(Type::U8);
            if (op == NEON_CMGT_zero_scalar)
                condition = this->template Xor<Type::U8>(condition, SingleOperand(Imm{u8{1}}))
                                    .WithView(Type::U8);
        }
        result = this->Select(condition, ImmValue(~u64{0}, Type::U64), zero).WithView(Type::U64);
    } else if (narrow) {
        const bool signed_source = op != NEON_UQXTN_scalar;
        if (signed_source) source = SignExtendValue(source, source_bits);
        auto trimmed =
                this->template And<Type::U64>(source, SingleOperand(Imm{mask})).WithView(Type::U64);
        if (op == NEON_SQXTN_scalar) trimmed = SignExtendValue(trimmed, bits);
        auto changed =
                this->template Xor<Type::U64>(source, SingleOperand(trimmed)).WithView(Type::U64);
        saturated = this->TestNotZero(changed).WithView(Type::U8);
        auto limit = ImmValue(mask, Type::U64);
        if (signed_source) {
            auto negative = this->TestBit(source, Imm{63u}).WithView(Type::U8);
            limit = this->Select(negative,
                                 ImmValue(op == NEON_SQXTN_scalar ? u64{1} << (bits - 1) : 0,
                                          Type::U64),
                                 ImmValue(op == NEON_SQXTN_scalar ? (u64{1} << (bits - 1)) - 1
                                                                  : mask,
                                          Type::U64))
                            .WithView(Type::U64);
        }
        result = this->Select(saturated, limit, source).WithView(Type::U64);
    } else {
        auto negative = this->TestBit(source, Imm{bits - 1}).WithView(Type::U8);
        auto opposite =
                this->template Sub<Type::U64>(zero, SingleOperand(source)).WithView(Type::U64);
        result = absolute ? this->Select(negative, opposite, source).WithView(Type::U64) : opposite;
        if (saturate_unary) {
            auto changed =
                    this->template Xor<Type::U64>(source, SingleOperand(Imm{u64{1} << (bits - 1)}))
                            .WithView(Type::U64);
            saturated = this->TestZero(changed).WithView(Type::U8);
            result =
                    this->Select(saturated, ImmValue((u64{1} << (bits - 1)) - 1, Type::U64), result)
                            .WithView(Type::U64);
        }
    }
    if (narrow || saturate_unary) AccumulateQC(saturated);
    result = this->template And<Type::U64>(result, SingleOperand(Imm{mask})).WithView(Type::U64);
    WriteVRegister(instr->GetRd(), result);
}

void VisitNEONScalarPairwise(const vixl::aarch64::Instruction* instr) override {
    if (TryScalarPairwiseFP(instr)) return;
    // The generic VIXL visitor admits reserved ADDP size fields, including
    // size=01 whose bit 23 differs from the allocated ADDP opcode constant.
    const auto op = instr->GetInstructionBits() & ~(3u << 22);
    const bool integer_add = (op & 0xFFFFFC00u) == (u32(NEON_ADDP_scalar) & ~(3u << 22));
    if (!integer_add) {
        // FP pairwise operations require guest FPCR/FPSR and ARM NaN handling.
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    if (instr->GetNEONSize() != 3) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    auto low = ReadVHalf(instr->GetRn(), false), high = ReadVHalf(instr->GetRn(), true);
    auto result = this->template Add<Type::U64>(low, SingleOperand(high)).WithView(Type::U64);
    WriteVRegister(instr->GetRd(), result);
}

Value ScalarDoublingMultiply(
        Value lhs, Value rhs, Value accumulator, u32 bits, bool accumulate, bool subtract) {
    const u32 out_bits = bits * 2;
    const u64 source_mask = (u64{1} << bits) - 1;
    const u64 out_mask = out_bits == 64 ? ~u64{0} : (u64{1} << out_bits) - 1;
    const u64 out_sign = u64{1} << (out_bits - 1);
    lhs = this->template And<Type::U64>(lhs, SingleOperand(Imm{source_mask})).WithView(Type::U64);
    rhs = this->template And<Type::U64>(rhs, SingleOperand(Imm{source_mask})).WithView(Type::U64);
    auto lhs_min = this->TestZero(this->template Xor<Type::U64>(
                                              lhs, SingleOperand(Imm{u64{1} << (bits - 1)}))
                                          .WithView(Type::U64))
                           .WithView(Type::U8);
    auto rhs_min = this->TestZero(this->template Xor<Type::U64>(
                                              rhs, SingleOperand(Imm{u64{1} << (bits - 1)}))
                                          .WithView(Type::U64))
                           .WithView(Type::U8);
    // The only doubled product outside the signed double-width range is
    // MIN * MIN. Saturate it before adding/subtracting the accumulator: a
    // negative accumulator must not cancel this first saturation or its QC.
    auto saturated =
            this->template And<Type::U8>(lhs_min, SingleOperand(rhs_min)).WithView(Type::U8);
    lhs = SignExtendValue(lhs, bits);
    rhs = SignExtendValue(rhs, bits);
    auto product = this->template Mul<Type::U64>(lhs, SingleOperand(rhs)).WithView(Type::U64);
    product = this->template LslImm<Type::U64>(product, Imm{u32{1}}).WithView(Type::U64);
    product =
            this->Select(saturated, ImmValue(out_sign - 1, Type::U64), product).WithView(Type::U64);
    Value result = product;
    if (accumulate) {
        accumulator = SignExtendValue(accumulator, out_bits);
        product = SignExtendValue(product, out_bits);
        result = (subtract ? this->template Sub<Type::U64>(accumulator, SingleOperand(product))
                           : this->template Add<Type::U64>(accumulator, SingleOperand(product)))
                         .WithView(Type::U64);
        // For addition overflow requires equal input signs and a changed
        // result sign; subtraction requires different input signs instead.
        auto input_signs = this->template Xor<Type::U64>(accumulator, SingleOperand(product))
                                   .WithView(Type::U64);
        if (!subtract)
            input_signs = this->template Xor<Type::U64>(input_signs, SingleOperand(Imm{~u64{0}}))
                                  .WithView(Type::U64);
        auto result_sign = this->template Xor<Type::U64>(accumulator, SingleOperand(result))
                                   .WithView(Type::U64);
        auto overflow_bits = this->template And<Type::U64>(input_signs, SingleOperand(result_sign))
                                     .WithView(Type::U64);
        auto overflow = this->TestBit(overflow_bits, Imm{out_bits - 1}).WithView(Type::U8);
        auto negative = this->TestBit(accumulator, Imm{out_bits - 1}).WithView(Type::U8);
        auto limit = this->Select(negative,
                                  ImmValue(out_sign, Type::U64),
                                  ImmValue(out_sign - 1, Type::U64))
                             .WithView(Type::U64);
        result = this->Select(overflow, limit, result).WithView(Type::U64);
        saturated =
                this->template Or<Type::U8>(saturated, SingleOperand(overflow)).WithView(Type::U8);
    }
    AccumulateQC(saturated);
    return this->template And<Type::U64>(result, SingleOperand(Imm{out_mask})).WithView(Type::U64);
}

struct NEONIndexedElement {
    u32 rm;
    u32 index;
};

static NEONIndexedElement DecodeNEONIndexedElement(const vixl::aarch64::Instruction* instr,
                                                   u32 bits) {
    const u32 rm = bits == 16 ? instr->GetRmLow16() : instr->GetRm();
    const u32 index =
            bits == 16 ? (instr->GetNEONH() << 2) | (instr->GetNEONL() << 1) | instr->GetNEONM()
                       : (instr->GetNEONH() << 1) | instr->GetNEONL();
    return {rm, index};
}

void VisitNEONScalar3Diff(const vixl::aarch64::Instruction* instr) override {
    const auto op = instr->Mask(NEONScalar3DiffMask);
    if (op != NEON_SQDMULL_scalar && op != NEON_SQDMLAL_scalar && op != NEON_SQDMLSL_scalar) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    const u32 bits = 8u << instr->GetNEONSize();
    if (bits != 16 && bits != 32) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const bool accumulate = op != NEON_SQDMULL_scalar;
    // Snapshot every source before writing an aliased scalar destination.
    auto lhs = ReadVHalf(instr->GetRn(), false);
    auto rhs = ReadVHalf(instr->GetRm(), false);
    auto zero = ImmValue(0, Type::U64);
    auto old = accumulate ? ReadVHalf(instr->GetRd(), false) : zero;
    auto result =
            ScalarDoublingMultiply(lhs, rhs, old, bits, accumulate, op == NEON_SQDMLSL_scalar);
    WriteVRegister(instr->GetRd(), result);
}

void VisitNEONScalarByIndexedElement(const vixl::aarch64::Instruction* instr) override {
    if (TryNEONIndexedFP(instr)) return;
    if (TryNEONSaturatingMultiplyHigh(instr, true, true, false)) return;
    const auto op = instr->Mask(NEONScalarByIndexedElementMask);
    if (op != NEON_SQDMULL_byelement_scalar && op != NEON_SQDMLAL_byelement_scalar &&
        op != NEON_SQDMLSL_byelement_scalar) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    const u32 bits = 8u << instr->GetNEONSize();
    if (bits != 16 && bits != 32) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const auto [rm, index] = DecodeNEONIndexedElement(instr, bits);
    const bool accumulate = op != NEON_SQDMULL_byelement_scalar;
    auto lhs = ReadVHalf(instr->GetRn(), false);
    auto rhs = ReadVHalf(rm, index * bits >= 64);
    rhs = this->template LsrImm<Type::U64>(rhs, Imm{index * bits % 64}).WithView(Type::U64);
    auto zero = ImmValue(0, Type::U64);
    auto old = accumulate ? ReadVHalf(instr->GetRd(), false) : zero;
    auto result = ScalarDoublingMultiply(
            lhs, rhs, old, bits, accumulate, op == NEON_SQDMLSL_byelement_scalar);
    WriteVRegister(instr->GetRd(), result);
}

void VisitNEONByIndexedElement(const vixl::aarch64::Instruction* instr) override {
    if (TryNEONVectorDoublingLong(instr, true)) return;
    if (TryNEONIndexedFP(instr)) return;
    if (TryNEONDotProduct(instr, true)) return;
    if (TryNEONSaturatingMultiplyHigh(instr, false, true, false)) return;
    const auto op = instr->Mask(NEONByIndexedElementMask);
    const bool narrow =
            op == NEON_MUL_byelement || op == NEON_MLA_byelement || op == NEON_MLS_byelement;
    const bool widen = op == NEON_SMULL_byelement || op == NEON_UMULL_byelement ||
                       op == NEON_SMLAL_byelement || op == NEON_UMLAL_byelement ||
                       op == NEON_SMLSL_byelement || op == NEON_UMLSL_byelement;
    if (!narrow && !widen) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    const u32 bits = 8u << instr->GetNEONSize();
    if (bits != 16 && bits != 32) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const bool upper = instr->GetNEONQ();
    const auto [rm, index] = DecodeNEONIndexedElement(instr, bits);
    const u64 mask = (u64{1} << bits) - 1;
    auto element = ReadVHalf(rm, index * bits >= 64);
    element = this->LsrImm(element, Imm{index * bits % 64}).WithView(Type::U64);
    element = this->And(element, SingleOperand(Imm{mask})).WithView(Type::U64);
    if (narrow) {
        auto broadcast = element;
        for (u32 shift = bits; shift < 64; shift *= 2) {
            auto copy = this->LslImm(broadcast, Imm{shift}).WithView(Type::U64);
            broadcast = this->Or(broadcast, SingleOperand(copy)).WithView(Type::U64);
        }
        auto rhs = this->VecDup64(broadcast).WithView(Type::V128);
        auto lhs = ReadVRegister(instr->GetRn(), Type::V128);
        auto result = this->VecMul(lhs, rhs, Imm{bits}).WithView(Type::V128);
        if (op != NEON_MUL_byelement) {
            auto old = ReadVRegister(instr->GetRd(), Type::V128);
            result = (op == NEON_MLA_byelement ? this->VecAdd(old, result, Imm{bits})
                                               : this->VecSub(old, result, Imm{bits}))
                             .WithView(Type::V128);
        }
        if (!upper) result = Reinterpret(result, Type::V64);
        WriteVRegister(instr->GetRd(), result);
        return;
    }
    const bool sign =
            op == NEON_SMULL_byelement || op == NEON_SMLAL_byelement || op == NEON_SMLSL_byelement;
    const bool accumulate = op != NEON_SMULL_byelement && op != NEON_UMULL_byelement;
    const bool subtract = op == NEON_SMLSL_byelement || op == NEON_UMLSL_byelement;
    const u32 out_bits = bits * 2;
    const u64 out_mask = out_bits == 64 ? ~u64{0} : (u64{1} << out_bits) - 1;
    auto source = ReadVHalf(instr->GetRn(), upper);
    // Snapshot every input, including the accumulator, before writing an aliased Rd.
    auto old_low = accumulate ? ReadVHalf(instr->GetRd(), false) : ImmValue(0, Type::U64);
    auto old_high = accumulate ? ReadVHalf(instr->GetRd(), true) : old_low;
    if (sign) element = SignExtendValue(element, bits);
    auto low = ImmValue(0, Type::U64), high = low;
    for (u32 lane = 0; lane < 64 / bits; ++lane) {
        auto a = this->LsrImm(source, Imm{lane * bits}).WithView(Type::U64);
        a = this->And(a, SingleOperand(Imm{mask})).WithView(Type::U64);
        if (sign) a = SignExtendValue(a, bits);
        auto result = this->Mul(a, SingleOperand(element)).WithView(Type::U64);
        const u32 position = lane * out_bits;
        if (accumulate) {
            auto old = this->LsrImm(position < 64 ? old_low : old_high, Imm{position % 64})
                               .WithView(Type::U64);
            result = (subtract ? this->Sub(old, SingleOperand(result))
                               : this->Add(old, SingleOperand(result)))
                             .WithView(Type::U64);
        }
        result = this->And(result, SingleOperand(Imm{out_mask})).WithView(Type::U64);
        result = this->LslImm(result, Imm{position % 64}).WithView(Type::U64);
        auto& half = position < 64 ? low : high;
        half = this->Or(half, SingleOperand(result)).WithView(Type::U64);
    }
    WriteVHalves(instr->GetRd(), low, high);
}

bool TryNEONPolynomial(const vixl::aarch64::Instruction* instr, bool widening) {
    const u32 op =
            instr->Mask(widening ? u32(NEON3DifferentMask) : u32(NEON3SameMask)) & ~u32(NEON_Q);
    if (op != u32(widening ? u32(NEON_PMULL) : u32(NEON_PMUL))) return false;
    const u32 size = instr->GetNEONSize();
    const bool q = instr->GetNEONQ();
    if (widening ? size != 0 && size != 3 : size != 0) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    auto left = ReadVRegister(instr->GetRn(), Type::V128);
    auto right = ReadVRegister(instr->GetRm(), Type::V128);
    Value result;
    if (widening && size == 3) {
        // PMULL2 selects the high 64-bit lane of both operands.
        result = this->VecPclMul(left, right, Imm{q ? 0x11u : 0u}).WithView(Type::V128);
    } else {
        auto constant = [&](u64 half) {
            return this->VecLoadConst(Imm{half}, Imm{half}).WithView(Type::V128);
        };
        auto zero = this->VecSharedZero().WithView(Type::V128);
        if (widening) {
            left = this->VecZip(left, zero, Imm{8u}, Imm{q}).WithView(Type::V128);
            right = this->VecZip(right, zero, Imm{8u}, Imm{q}).WithView(Type::V128);
        }
        result = zero;
        const auto ones = constant(widening ? 0x0001000100010001ULL : 0x0101010101010101ULL);
        for (u32 bit = 0; bit < 8; ++bit) {
            auto coefficient =
                    this->VecShiftRightImm(right, Imm{bit}, Imm{16u}).WithView(Type::V128);
            coefficient = this->VecAnd(coefficient, ones).WithView(Type::V128);
            auto selected = this->VecCmpEq(coefficient, ones, Imm{widening ? 16u : 8u})
                                    .WithView(Type::V128);
            auto term = this->VecShiftLeftImm(left, Imm{bit}, Imm{16u}).WithView(Type::V128);
            if (!widening) {
                // H shifts implement byte shifts after masking the low bits
                // introduced from the adjacent byte, avoiding scalar B shifts.
                const u64 mask = u64((0xFFu << bit) & 0xFFu) * 0x0101010101010101ULL;
                term = this->VecAnd(term, constant(mask)).WithView(Type::V128);
            }
            term = this->VecAnd(term, selected).WithView(Type::V128);
            result = this->VecXor(result, term).WithView(Type::V128);
        }
        if (!widening && !q) result = Reinterpret(result, Type::V64);
    }
    WriteVRegister(instr->GetRd(), result);
    return true;
}

bool TryNEONMixedSaturating(const vixl::aarch64::Instruction* instr, bool scalar) {
    const u32 op =
            instr->GetInstructionBits() & NEON2RegMiscMask & ~(u32(NEON_Q) | u32(NEONScalar));
    const bool mixed = op == NEON_SUQADD || op == NEON_USQADD;
    const bool unary = !scalar && (op == NEON_SQABS || op == NEON_SQNEG);
    if (!mixed && !unary) return false;
    const u32 bits = 8u << instr->GetNEONSize();
    const bool q = instr->GetNEONQ();
    if (!scalar && bits == 64 && !q) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1, sign = u64{1} << (bits - 1);
    const u32 active = scalar ? 1 : (q ? 128 : 64) / bits;
    auto source_low = ReadVHalf(instr->GetRn(), false),
         source_high = ReadVHalf(instr->GetRn(), true);
    auto old_low = mixed ? ReadVHalf(instr->GetRd(), false) : ImmValue(0, Type::U64);
    auto old_high = mixed ? ReadVHalf(instr->GetRd(), true) : old_low;
    auto zero = ImmValue(0, Type::U64), low = zero, high = zero;
    auto any_saturation = ImmValue(0, Type::U8);
    auto invert = [&](Value value) {
        return this->Xor(value, SingleOperand(Imm{mask})).WithView(Type::U64);
    };
    auto unsigned_less = [&](Value a, Value b) {
        auto difference = this->Sub(a, SingleOperand(b)).WithView(Type::U64);
        auto first = this->And(invert(a), SingleOperand(b)).WithView(Type::U64);
        auto changed = this->Xor(a, SingleOperand(b)).WithView(Type::U64);
        auto second = this->And(invert(changed), SingleOperand(difference)).WithView(Type::U64);
        auto borrow = this->Or(first, SingleOperand(second)).WithView(Type::U64);
        return this->TestBit(borrow, Imm{bits - 1}).WithView(Type::U8);
    };
    for (u32 lane = 0; lane < active; ++lane) {
        const u32 position = lane * bits;
        auto source = this->LsrImm(position < 64 ? source_low : source_high, Imm{position % 64})
                              .WithView(Type::U64);
        source = this->And(source, SingleOperand(Imm{mask})).WithView(Type::U64);
        Value result, saturated;
        if (mixed) {
            auto old = this->LsrImm(position < 64 ? old_low : old_high, Imm{position % 64})
                               .WithView(Type::U64);
            old = this->And(old, SingleOperand(Imm{mask})).WithView(Type::U64);
            // Bias the accumulator into the source's signedness. The mixed
            // operation then becomes ordinary same-signedness saturating add.
            old = this->Xor(old, SingleOperand(Imm{sign})).WithView(Type::U64);
            result = this->Add(old, SingleOperand(source)).WithView(Type::U64);
            result = this->And(result, SingleOperand(Imm{mask})).WithView(Type::U64);
            Value limit;
            if (op == NEON_SUQADD) {
                saturated = unsigned_less(result, old);
                limit = ImmValue(mask, Type::U64);
            } else {
                auto first = this->Xor(old, SingleOperand(result)).WithView(Type::U64);
                auto second = this->Xor(source, SingleOperand(result)).WithView(Type::U64);
                auto changed = this->And(first, SingleOperand(second)).WithView(Type::U64);
                saturated = this->TestBit(changed, Imm{bits - 1}).WithView(Type::U8);
                auto negative = this->TestBit(old, Imm{bits - 1}).WithView(Type::U8);
                limit = this->Select(negative,
                                     ImmValue(sign, Type::U64),
                                     ImmValue(sign - 1, Type::U64))
                                .WithView(Type::U64);
            }
            result = this->Select(saturated, limit, result).WithView(Type::U64);
            result = this->Xor(result, SingleOperand(Imm{sign})).WithView(Type::U64);
        } else {
            auto difference = this->Xor(source, SingleOperand(Imm{sign})).WithView(Type::U64);
            saturated = this->TestZero(difference).WithView(Type::U8);
            result = this->Sub(zero, SingleOperand(source)).WithView(Type::U64);
            if (op == NEON_SQABS) {
                auto negative = this->TestBit(source, Imm{bits - 1}).WithView(Type::U8);
                result = this->Select(negative, result, source).WithView(Type::U64);
            }
            result = this->Select(saturated, ImmValue(sign - 1, Type::U64), result)
                             .WithView(Type::U64);
        }
        any_saturation = this->Or(any_saturation, SingleOperand(saturated)).WithView(Type::U8);
        result = this->And(result, SingleOperand(Imm{mask})).WithView(Type::U64);
        result = this->LslImm(result, Imm{position % 64}).WithView(Type::U64);
        auto& half = position < 64 ? low : high;
        half = this->Or(half, SingleOperand(result)).WithView(Type::U64);
    }
    AccumulateQC(any_saturation);
    WriteVHalves(instr->GetRd(), low, high);
    return true;
}

bool TryNEONPairwiseWiden(const vixl::aarch64::Instruction* instr) {
    const u32 op = instr->Mask(NEON2RegMiscMask);
    const bool shift = op == NEON_SHLL;
    const bool sign = op == NEON_SADDLP || op == NEON_SADALP;
    const bool accumulate = op == NEON_SADALP || op == NEON_UADALP;
    if (!shift && !sign && !accumulate && op != NEON_UADDLP) return false;
    const u32 bits = 8u << instr->GetNEONSize();
    const bool q = instr->GetNEONQ();
    if (bits == 64) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    const u32 output_bits = bits * 2;
    if (shift) {
        // SHLL's fixed shift equals the input width; Q selects the low/high
        // input half, and the output always occupies all 128 bits.
        auto input = ReadVRegister(instr->GetRn(), Type::V128);
        auto zero = this->VecSharedZero().WithView(Type::V128);
        auto widened = this->VecZip(input, zero, Imm{bits}, Imm{q}).WithView(Type::V128);
        auto result =
                this->VecShiftLeftImm(widened, Imm{bits}, Imm{output_bits}).WithView(Type::V128);
        WriteVRegister(instr->GetRd(), result);
        return true;
    }
    auto input = ReadVRegister(instr->GetRn(), Type::V128);
    auto zero = this->VecSharedZero().WithView(Type::V128);
    auto widen = [&](bool odd) {
        // De-interleaving places all even/odd source lanes in the low half.
        // ZIP with zero then widens those contiguous lanes to the output width.
        auto selected = this->VecUnzip(input, zero, Imm{bits}, Imm{odd}).WithView(Type::V128);
        auto result = this->VecZip(selected, zero, Imm{bits}, Imm{false}).WithView(Type::V128);
        if (sign) {
            result =
                    this->VecShiftLeftImm(result, Imm{bits}, Imm{output_bits}).WithView(Type::V128);
            result = this->VecShiftRightArithmeticImm(result, Imm{bits}, Imm{output_bits})
                             .WithView(Type::V128);
        }
        return result;
    };
    auto even = widen(false), odd = widen(true);
    auto result = this->VecAdd(even, odd, Imm{output_bits}).WithView(Type::V128);
    if (accumulate) {
        auto old = ReadVRegister(instr->GetRd(), Type::V128);
        result = this->VecAdd(old, result, Imm{output_bits}).WithView(Type::V128);
    }
    if (!q) result = Reinterpret(result, Type::V64);
    WriteVRegister(instr->GetRd(), result);
    return true;
}

bool TryNEONHighNarrow(const vixl::aarch64::Instruction* instr) {
    const u32 op = instr->Mask(NEON3DifferentMask) & ~u32(NEON_Q);
    const bool add = op == NEON_ADDHN || op == NEON_RADDHN;
    const bool subtract = op == NEON_SUBHN || op == NEON_RSUBHN;
    if (!add && !subtract) return false;
    const u32 size = instr->GetNEONSize();
    if (size == 3) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    const u32 output_bits = 8u << size, source_bits = 2 * output_bits;
    const bool upper = instr->GetNEONQ();
    // Snapshot Rd before any write: HN2 preserves its low half even when aliased.
    auto old_low = upper ? ReadVHalf(instr->GetRd(), false) : ImmValue(0, Type::U64);
    auto left = ReadVRegister(instr->GetRn(), Type::V128);
    auto right = ReadVRegister(instr->GetRm(), Type::V128);
    auto result = (add ? this->VecAdd(left, right, Imm{source_bits})
                       : this->VecSub(left, right, Imm{source_bits}))
                          .WithView(Type::V128);
    if (op == NEON_RADDHN || op == NEON_RSUBHN) {
        u64 rounding = 0;
        for (u32 lane = 0; lane < 64 / source_bits; ++lane)
            rounding |= (u64{1} << (output_bits - 1)) << (lane * source_bits);
        auto increment = this->VecLoadConst(Imm{rounding}, Imm{rounding}).WithView(Type::V128);
        result = this->VecAdd(result, increment, Imm{source_bits}).WithView(Type::V128);
    }
    // Odd half-sized elements are the high bits of each full-width sum.
    result = this->VecUnzip(result, result, Imm{output_bits}, Imm{true}).WithView(Type::V128);
    auto packed = Reinterpret(result, Type::U64);
    WriteVHalves(instr->GetRd(), upper ? old_low : packed, upper ? packed : ImmValue(0, Type::U64));
    return true;
}

bool TryNEONAbsoluteLong(const vixl::aarch64::Instruction* instr) {
    const u32 op = instr->Mask(NEON3DifferentMask) & ~u32(NEON_Q);
    const bool sign = op == NEON_SABDL || op == NEON_SABAL;
    const bool accumulate = op == NEON_SABAL || op == NEON_UABAL;
    if (!sign && !accumulate && op != NEON_UABDL) return false;
    const u32 bits = 8u << instr->GetNEONSize(), output_bits = bits * 2;
    if (bits == 64) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    const bool upper = instr->GetNEONQ();
    auto zero = this->VecSharedZero().WithView(Type::V128);
    auto widen = [&](u32 reg) {
        auto input = ReadVRegister(reg, Type::V128);
        auto result = this->VecZip(input, zero, Imm{bits}, Imm{upper}).WithView(Type::V128);
        if (sign) {
            result =
                    this->VecShiftLeftImm(result, Imm{bits}, Imm{output_bits}).WithView(Type::V128);
            result = this->VecShiftRightArithmeticImm(result, Imm{bits}, Imm{output_bits})
                             .WithView(Type::V128);
        }
        return result;
    };
    auto left = widen(instr->GetRn()), right = widen(instr->GetRm());
    // A double-width signed lane holds every signed/unsigned source
    // difference, so abs needs no extra carry or saturation information.
    auto difference = this->VecSub(left, right, Imm{output_bits}).WithView(Type::V128);
    auto negative =
            this->VecShiftRightArithmeticImm(difference, Imm{output_bits - 1}, Imm{output_bits})
                    .WithView(Type::V128);
    auto flipped = this->VecXor(difference, negative).WithView(Type::V128);
    auto result = this->VecSub(flipped, negative, Imm{output_bits}).WithView(Type::V128);
    if (accumulate) {
        auto old = ReadVRegister(instr->GetRd(), Type::V128);
        result = this->VecAdd(old, result, Imm{output_bits}).WithView(Type::V128);
    }
    WriteVRegister(instr->GetRd(), result);
    return true;
}

bool TryNEONAbsoluteNarrow(const vixl::aarch64::Instruction* instr) {
    const u32 op = instr->Mask(NEON3SameMask);
    const bool sign = op == NEON_SABD || op == NEON_SABA;
    const bool accumulate = op == NEON_SABA || op == NEON_UABA;
    if (!sign && !accumulate && op != NEON_UABD) return false;
    const u32 bits = 8u << instr->GetNEONSize();
    const bool q = instr->GetNEONQ();
    if (bits == 64) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    auto left = ReadVRegister(instr->GetRn(), Type::V128);
    auto right = ReadVRegister(instr->GetRm(), Type::V128);
    auto compare_left = left, compare_right = right;
    if (!sign) {
        u64 sign_bits = 0;
        for (u32 position = 0; position < 64; position += bits)
            sign_bits |= u64{1} << (position + bits - 1);
        auto bias = this->VecLoadConst(Imm{sign_bits}, Imm{sign_bits}).WithView(Type::V128);
        compare_left = this->VecXor(left, bias).WithView(Type::V128);
        compare_right = this->VecXor(right, bias).WithView(Type::V128);
    }
    auto greater = this->VecCmpGt(compare_left, compare_right, Imm{bits}).WithView(Type::V128);
    // Both differences wrap at lane width. Choosing the subtraction from
    // larger to smaller yields the full unsigned magnitude, including the
    // signed MIN/MAX difference that does not fit a signed narrow lane.
    auto forward = this->VecSub(left, right, Imm{bits}).WithView(Type::V128);
    auto reverse = this->VecSub(right, left, Imm{bits}).WithView(Type::V128);
    forward = this->VecAnd(forward, greater).WithView(Type::V128);
    reverse = this->VecAndNot(reverse, greater).WithView(Type::V128);
    auto result = this->VecOr(forward, reverse).WithView(Type::V128);
    if (accumulate) {
        auto old = ReadVRegister(instr->GetRd(), Type::V128);
        result = this->VecAdd(old, result, Imm{bits}).WithView(Type::V128);
    }
    if (!q) result = Reinterpret(result, Type::V64);
    WriteVRegister(instr->GetRd(), result);
    return true;
}

bool TryNEONHalving(const vixl::aarch64::Instruction* instr) {
    const u32 op = instr->Mask(NEON3SameMask);
    const bool add = op == NEON_SHADD || op == NEON_UHADD;
    const bool rounded = op == NEON_SRHADD || op == NEON_URHADD;
    const bool subtract = op == NEON_SHSUB || op == NEON_UHSUB;
    if (!add && !rounded && !subtract) return false;
    const u32 bits = 8u << instr->GetNEONSize();
    if (bits == 64) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    const bool sign = op == NEON_SHADD || op == NEON_SRHADD || op == NEON_SHSUB;
    const auto left = ReadVRegister(instr->GetRn(), Type::V128);
    const auto right = ReadVRegister(instr->GetRm(), Type::V128);
    const auto different = this->VecXor(left, right).WithView(Type::V128);
    auto half = [&](Value value) {
        return (sign ? this->VecShiftRightArithmeticImm(value, Imm{1u}, Imm{bits})
                     : this->VecShiftRightImm(value, Imm{1u}, Imm{bits}))
                .WithView(Type::V128);
    };
    Value result;
    if (subtract) {
        // floor((a-b)/2) = floor(a/2)-floor(b/2)-(!a[0] && b[0]).
        // The unsigned difference may be negative: retain its lane-sized bits.
        u64 ones = 0;
        for (u32 bit = 0; bit < 64; bit += bits) ones |= u64{1} << bit;
        auto low_bits = this->VecLoadConst(Imm{ones}, Imm{ones}).WithView(Type::V128);
        auto borrow = this->VecAnd(different, right).WithView(Type::V128);
        borrow = this->VecAnd(borrow, low_bits).WithView(Type::V128);
        result = this->VecSub(half(left), half(right), Imm{bits}).WithView(Type::V128);
        result = this->VecSub(result, borrow, Imm{bits}).WithView(Type::V128);
    } else if (rounded) {
        // ceil((a+b)/2) = (a|b)-floor((a^b)/2), including signed lanes.
        auto common = this->VecOr(left, right).WithView(Type::V128);
        result = this->VecSub(common, half(different), Imm{bits}).WithView(Type::V128);
    } else {
        // floor((a+b)/2) = (a&b)+floor((a^b)/2), without a lost carry.
        auto common = this->VecAnd(left, right).WithView(Type::V128);
        result = this->VecAdd(common, half(different), Imm{bits}).WithView(Type::V128);
    }
    if (!instr->GetNEONQ()) result = Reinterpret(result, Type::V64);
    WriteVRegister(instr->GetRd(), result);
    return true;
}

bool TryNEONVectorDoublingLong(const vixl::aarch64::Instruction* instr, bool indexed) {
    const u32 mask = indexed ? u32(NEONByIndexedElementMask) : u32(NEON3DifferentMask);
    const u32 op = instr->Mask(mask) & ~u32(NEON_Q);
    const u32 multiply = indexed ? u32(NEON_SQDMULL_byelement) : u32(NEON_SQDMULL);
    const u32 add = indexed ? u32(NEON_SQDMLAL_byelement) : u32(NEON_SQDMLAL);
    const u32 subtract = indexed ? u32(NEON_SQDMLSL_byelement) : u32(NEON_SQDMLSL);
    if (op != multiply && op != add && op != subtract) return false;
    const u32 bits = 8u << instr->GetNEONSize();
    if (bits != 16 && bits != 32) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    const bool upper = instr->GetNEONQ(), accumulate = op != multiply;
    // Q selects the source half; every form produces the complete widened
    // 128-bit destination. Snapshot it along with all sources before writes.
    auto lhs = ReadVHalf(instr->GetRn(), upper);
    Value rhs;
    if (indexed) {
        const auto [rm, index] = DecodeNEONIndexedElement(instr, bits);
        rhs = ReadVHalf(rm, index * bits >= 64);
        if (index * bits % 64)
            rhs = this->template LsrImm<Type::U64>(rhs, Imm{index * bits % 64}).WithView(Type::U64);
    } else {
        rhs = ReadVHalf(instr->GetRm(), upper);
    }
    auto accumulator_low = accumulate ? ReadVHalf(instr->GetRd(), false) : ImmValue(0, Type::U64);
    auto accumulator_high = accumulate ? ReadVHalf(instr->GetRd(), true) : accumulator_low;
    const u32 out_bits = bits * 2;
    auto low = ImmValue(0, Type::U64), high = low;
    for (u32 lane = 0; lane < 64 / bits; ++lane) {
        auto left =
                lane ? this->template LsrImm<Type::U64>(lhs, Imm{lane * bits}).WithView(Type::U64)
                     : lhs;
        auto right = !indexed && lane ? this->template LsrImm<Type::U64>(rhs, Imm{lane * bits})
                                                .WithView(Type::U64)
                                      : rhs;
        const u32 position = lane * out_bits;
        auto old = position < 64 ? accumulator_low : accumulator_high;
        if (position % 64)
            old = this->template LsrImm<Type::U64>(old, Imm{position % 64}).WithView(Type::U64);
        // This helper clamps 2*a*b before accumulator +/- product and ORs
        // either saturation into QC, including a later cancellation.
        auto result = ScalarDoublingMultiply(left, right, old, bits, accumulate, op == subtract);
        if (position % 64)
            result = this->template LslImm<Type::U64>(result, Imm{position % 64})
                             .WithView(Type::U64);
        auto& destination = position < 64 ? low : high;
        destination = this->template Or<Type::U64>(destination, SingleOperand(result))
                              .WithView(Type::U64);
    }
    WriteVHalves(instr->GetRd(), low, high);
    return true;
}

bool TryNEONDotProduct(const vixl::aarch64::Instruction* instr, bool indexed) {
    const auto op = instr->Mask(indexed ? u32(NEONByIndexedElementMask) : u32(NEON3SameExtraMask));
    const bool signed_bytes = op == (indexed ? u32(NEON_SDOT_byelement) : u32(NEON_SDOT));
    if (!signed_bytes && op != (indexed ? u32(NEON_UDOT_byelement) : u32(NEON_UDOT))) return false;
    if (instr->GetNEONSize() != 2) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    Helper(Numeric::Dot, indexed);
    return true;
}

void VisitNEON3SameExtra(const vixl::aarch64::Instruction* instr) override {
    if (TryNEONDotProduct(instr, false)) return;
    if (TryNEONSaturatingMultiplyHigh(instr, false, false, true)) return;
    Interrupt(InterruptReason::FALLBACK, current_pc);
}

bool TryNEONSaturatingMultiplyHigh(const vixl::aarch64::Instruction* instr,
                                   bool scalar,
                                   bool indexed,
                                   bool extra) {
    const u32 mask =
            indexed ? (scalar ? u32(NEONScalarByIndexedElementMask) : u32(NEONByIndexedElementMask))
            : extra ? (scalar ? u32(NEONScalar3SameExtraMask) : u32(NEON3SameExtraMask))
                    : (scalar ? u32(NEONScalar3SameMask) : u32(NEON3SameMask));
    const u32 op = instr->Mask(mask);
    const u32 scalar_bits = scalar ? (u32(NEON_Q) | u32(NEONScalar)) : 0u;
    const u32 plain = (indexed ? u32(NEON_SQDMULH_byelement) : u32(NEON_SQDMULH)) | scalar_bits;
    const u32 rounded = (indexed ? u32(NEON_SQRDMULH_byelement) : u32(NEON_SQRDMULH)) | scalar_bits;
    const u32 add = (indexed ? u32(NEON_SQRDMLAH_byelement) : u32(NEON_SQRDMLAH)) | scalar_bits;
    const u32 sub = (indexed ? u32(NEON_SQRDMLSH_byelement) : u32(NEON_SQRDMLSH)) | scalar_bits;
    const bool valid = ((!extra || indexed) && (op == plain || op == rounded)) ||
                       ((extra || indexed) && (op == add || op == sub));
    if (!valid) return false;
    const u32 bits = 8u << instr->GetNEONSize();
    if (bits != 16 && bits != 32) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }

    Helper(Numeric::SatMultiply, u32(scalar) | (u32(indexed) << 1) | (u32(extra) << 2));
    return true;
}

void VisitNEONScalar3SameExtra(const vixl::aarch64::Instruction* instr) override {
    if (!TryNEONSaturatingMultiplyHigh(instr, true, false, true))
        Interrupt(InterruptReason::FALLBACK, current_pc);
}

bool TryNEONUnsignedEstimate(const vixl::aarch64::Instruction* instr) {
    // Bit 23 distinguishes integer estimates from FCVTAS/FCVTAU; only bit
    // 22 is a reserved size selector for these unsigned estimates.
    const u32 opcode = instr->Mask(NEON2RegMiscFPMask);
    if (opcode != NEON_URECPE && opcode != NEON_URSQRTE) return false;
    if (instr->GetNEONSize() != 2) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    Helper(Numeric::UnsignedEstimate);
    return true;
}
