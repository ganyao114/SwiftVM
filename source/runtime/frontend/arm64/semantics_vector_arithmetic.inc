bool TryNEONInteger(const vixl::aarch64::Instruction* instr, bool miscellaneous) {
    const u32 bits = 8u << instr->GetNEONSize();
    const bool q = instr->GetNEONQ();
    const auto op = instr->Mask(miscellaneous ? u32(NEON2RegMiscMask) : u32(NEON3SameMask));
    auto vector = [&](u32 reg) { return ReadVRegister(reg, Type::V128); };
    auto constant = [&](u64 half) {
        return this->VecLoadConst(Imm{half}, Imm{half}).WithView(Type::V128);
    };
    auto commit = [&](Value result) {
        result.WithView(Type::V128);
        if (!q) result = Reinterpret(result, Type::V64);
        WriteVRegister(instr->GetRd(), result);
    };
    const auto ones = [&] { return constant(~u64{0}); };
    if (!miscellaneous) {
        switch (instr->Mask(NEON3SameLogicalMask)) {
            case NEON_AND:
                commit(this->VecAnd(vector(instr->GetRn()), vector(instr->GetRm())));
                return true;
            case NEON_ORR:
                commit(this->VecOr(vector(instr->GetRn()), vector(instr->GetRm())));
                return true;
            case NEON_EOR:
                commit(this->VecXor(vector(instr->GetRn()), vector(instr->GetRm())));
                return true;
            case NEON_BIC:
                commit(this->VecAndNot(vector(instr->GetRn()), vector(instr->GetRm())));
                return true;
            case NEON_ORN: {
                auto inverted = this->VecXor(vector(instr->GetRm()), ones()).WithView(Type::V128);
                commit(this->VecOr(vector(instr->GetRn()), inverted));
                return true;
            }
            case NEON_BSL:
            case NEON_BIT:
            case NEON_BIF: {
                const auto logical = instr->Mask(NEON3SameLogicalMask);
                auto old = vector(instr->GetRd()), left = vector(instr->GetRn()),
                     right = vector(instr->GetRm());
                auto mask = logical == NEON_BSL ? old : right;
                if (logical == NEON_BIF) mask = this->VecXor(mask, ones()).WithView(Type::V128);
                auto selected = this->VecAnd(left, mask).WithView(Type::V128);
                auto retained = this->VecAndNot(logical == NEON_BSL ? right : old, mask)
                                        .WithView(Type::V128);
                commit(this->VecOr(selected, retained));
                return true;
            }
            default:
                break;
        }
    } else if (op == NEON_RBIT_NOT && instr->GetNEONSize() == 0) {
        commit(this->VecXor(vector(instr->GetRn()), ones()));
        return true;
    }
    if (bits == 64) {
        // VIXL's generic class visitor also admits reserved width/Q fields.
        // Check only recognized integer operations; other opcode classes keep
        // their own size interpretation and unsupported-operation fallback.
        bool invalid = false;
        if (miscellaneous) {
            switch (op) {
                case NEON_CMEQ_zero:
                case NEON_CMGT_zero:
                case NEON_CMGE_zero:
                case NEON_CMLT_zero:
                case NEON_CMLE_zero:
                case NEON_ABS:
                case NEON_NEG:
                    invalid = !q;
                    break;
                default:
                    break;
            }
        } else {
            switch (op) {
                case NEON_MUL:
                case NEON_MLA:
                case NEON_MLS:
                case NEON_SMAX:
                case NEON_SMIN:
                case NEON_UMAX:
                case NEON_UMIN:
                case NEON_SMAXP:
                case NEON_SMINP:
                case NEON_UMAXP:
                case NEON_UMINP:
                    invalid = true;
                    break;
                case NEON_ADD:
                case NEON_SUB:
                case NEON_ADDP:
                case NEON_CMEQ:
                case NEON_CMGT:
                case NEON_CMGE:
                case NEON_CMHI:
                case NEON_CMHS:
                case NEON_CMTST:
                    invalid = !q;
                    break;
                default:
                    break;
            }
        }
        if (invalid) {
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return true;
        }
        if (!q) return false;
    }
    auto zero = [&] { return this->VecSharedZero().WithView(Type::V128); };
    const bool pairwise =
            !miscellaneous && (op == NEON_ADDP || op == NEON_SMAXP || op == NEON_SMINP ||
                               op == NEON_UMAXP || op == NEON_UMINP);
    if (pairwise && (bits != 64 || op == NEON_ADDP)) {
        if (q) {
            auto left = vector(instr->GetRn()), right = vector(instr->GetRm());
            auto even = this->VecUnzip(left, right, Imm{bits}, Imm{false}).WithView(Type::V128);
            auto odd = this->VecUnzip(left, right, Imm{bits}, Imm{true}).WithView(Type::V128);
            if (op == NEON_ADDP)
                commit(this->VecAdd(even, odd, Imm{bits}));
            else if (op == NEON_SMAXP || op == NEON_UMAXP)
                commit(this->VecMax(even, odd, Imm{bits}, Imm{op == NEON_SMAXP}));
            else
                commit(this->VecMin(even, odd, Imm{bits}, Imm{op == NEON_SMINP}));
        } else {
            const u32 pairs = 32 / bits;
            const u64 mask = (u64{1} << bits) - 1;
            auto left = ReadVHalf(instr->GetRn(), false), right = ReadVHalf(instr->GetRm(), false);
            auto packed = ImmValue(0, Type::U64);
            for (u32 index = 0; index < pairs * 2; ++index) {
                auto source = index < pairs ? left : right;
                auto a = this->LsrImm(source, Imm{(index % pairs) * bits * 2}).WithView(Type::U64);
                auto b = this->LsrImm(source, Imm{((index % pairs) * 2 + 1) * bits})
                                 .WithView(Type::U64);
                a = this->And(a, SingleOperand(Imm{mask})).WithView(Type::U64);
                b = this->And(b, SingleOperand(Imm{mask})).WithView(Type::U64);
                const bool sign = op == NEON_SMAXP || op == NEON_SMINP;
                if (sign) {
                    a = SignExtendValue(a, bits);
                    b = SignExtendValue(b, bits);
                }
                Value lane;
                if (op == NEON_ADDP)
                    lane = this->Add(a, SingleOperand(b)).WithView(Type::U64);
                else {
                    auto difference = this->Sub(a, SingleOperand(b)).WithView(Type::U64);
                    auto less = this->TestBit(difference, Imm{63}).WithView(Type::U8);
                    const bool maximum = op == NEON_SMAXP || op == NEON_UMAXP;
                    lane = this->Select(less, maximum ? b : a, maximum ? a : b).WithView(Type::U64);
                }
                lane = this->And(lane, SingleOperand(Imm{mask})).WithView(Type::U64);
                lane = this->LslImm(lane, Imm{index * bits}).WithView(Type::U64);
                packed = this->Or(packed, SingleOperand(lane)).WithView(Type::U64);
            }
            WriteVRegister(instr->GetRd(), packed);
        }
        return true;
    }
    auto lhs = vector(instr->GetRn());
    auto rhs = miscellaneous ? zero() : vector(instr->GetRm());
    auto compare = [&](Value a, Value b, bool sign, bool equal) {
        if (!sign) {
            u64 bias{};
            for (u32 shift = 0; shift < 64; shift += bits) bias |= u64{1} << (shift + bits - 1);
            auto flip = constant(bias);
            a = this->VecXor(a, flip).WithView(Type::V128);
            b = this->VecXor(b, flip).WithView(Type::V128);
        }
        auto gt = this->VecCmpGt(a, b, Imm{bits}).WithView(Type::V128);
        if (!equal) return gt;
        auto eq = this->VecCmpEq(a, b, Imm{bits}).WithView(Type::V128);
        return this->VecOr(gt, eq).WithView(Type::V128);
    };
    if (miscellaneous) {
        switch (op) {
            case NEON_CMEQ_zero:
                commit(this->VecCmpEq(lhs, rhs, Imm{bits}));
                return true;
            case NEON_CMGT_zero:
                commit(compare(lhs, rhs, true, false));
                return true;
            case NEON_CMGE_zero:
                commit(compare(lhs, rhs, true, true));
                return true;
            case NEON_CMLT_zero:
                commit(compare(rhs, lhs, true, false));
                return true;
            case NEON_CMLE_zero:
                commit(compare(rhs, lhs, true, true));
                return true;
            case NEON_NEG:
                commit(this->VecSub(rhs, lhs, Imm{bits}));
                return true;
            case NEON_ABS: {
                auto negative = this->VecSub(rhs, lhs, Imm{bits}).WithView(Type::V128);
                auto mask = compare(rhs, lhs, true, false);
                auto selected = this->VecAnd(negative, mask).WithView(Type::V128);
                auto retained = this->VecAndNot(lhs, mask).WithView(Type::V128);
                commit(this->VecOr(selected, retained));
                return true;
            }
            default:
                return false;
        }
    }
    switch (op) {
        case NEON_ADD:
            commit(this->VecAdd(lhs, rhs, Imm{bits}));
            return true;
        case NEON_SUB:
            commit(this->VecSub(lhs, rhs, Imm{bits}));
            return true;
        case NEON_MUL:
        case NEON_MLA:
        case NEON_MLS: {
            if (bits == 64) return false;
            auto product = this->VecMul(lhs, rhs, Imm{bits}).WithView(Type::V128);
            if (op == NEON_MUL)
                commit(product);
            else {
                auto old = vector(instr->GetRd());
                commit(op == NEON_MLA ? this->VecAdd(old, product, Imm{bits})
                                      : this->VecSub(old, product, Imm{bits}));
            }
            return true;
        }
        case NEON_CMEQ:
            commit(this->VecCmpEq(lhs, rhs, Imm{bits}));
            return true;
        case NEON_CMGT:
        case NEON_CMGE:
        case NEON_CMHI:
        case NEON_CMHS:
            commit(compare(lhs,
                           rhs,
                           op == NEON_CMGT || op == NEON_CMGE,
                           op == NEON_CMGE || op == NEON_CMHS));
            return true;
        case NEON_CMTST: {
            auto combined = this->VecAnd(lhs, rhs).WithView(Type::V128);
            auto eq = this->VecCmpEq(combined, zero(), Imm{bits}).WithView(Type::V128);
            commit(this->VecXor(eq, ones()));
            return true;
        }
        case NEON_SMAX:
        case NEON_SMIN:
        case NEON_UMAX:
        case NEON_UMIN:
            if (bits == 64) return false;
            if (op == NEON_SMAX || op == NEON_UMAX)
                commit(this->VecMax(lhs, rhs, Imm{bits}, Imm{op == NEON_SMAX}));
            else
                commit(this->VecMin(lhs, rhs, Imm{bits}, Imm{op == NEON_SMIN}));
            return true;
        default:
            return false;
    }
}

void VisitNEONAcrossLanes(const vixl::aarch64::Instruction* instr) override {
    const bool fp16 = instr->Mask(NEONAcrossLanesFP16FMask) == NEONAcrossLanesFP16Fixed;
    if (TryNEONFPReduction(instr, fp16, true)) return;
    const auto op = instr->Mask(NEONAcrossLanesMask);
    const u32 bits = 8u << instr->GetNEONSize();
    const bool q = instr->GetNEONQ();
    if (bits >= 64 || (bits == 32 && !q)) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const bool add = op == NEON_ADDV || op == NEON_SADDLV || op == NEON_UADDLV;
    const bool sign = op == NEON_SADDLV || op == NEON_SMAXV || op == NEON_SMINV;
    const bool maximum = op == NEON_SMAXV || op == NEON_UMAXV;
    if (!add && !maximum && op != NEON_SMINV && op != NEON_UMINV) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    auto low = ReadVHalf(instr->GetRn(), false), high = ReadVHalf(instr->GetRn(), true);
    auto result = ImmValue(0, Type::U64);
    const u64 mask = (u64{1} << bits) - 1;
    for (u32 index = 0; index < (q ? 128u : 64u) / bits; ++index) {
        auto lane = this->LsrImm(index * bits < 64 ? low : high, Imm{index * bits % 64})
                            .WithView(Type::U64);
        lane = this->And(lane, SingleOperand(Imm{mask})).WithView(Type::U64);
        if (sign) lane = SignExtendValue(lane, bits);
        if (!index)
            result = lane;
        else if (add)
            result = this->Add(result, SingleOperand(lane)).WithView(Type::U64);
        else {
            // All legal source widths here are <=32, so the difference fits s64.
            auto diff = this->Sub(result, SingleOperand(lane)).WithView(Type::U64);
            auto negative = this->LsrImm(diff, Imm{63}).WithView(Type::U64);
            auto less = this->TestNotZero(negative).WithView(Type::U8);
            result = this->Select(less, maximum ? lane : result, maximum ? result : lane)
                             .WithView(Type::U64);
        }
    }
    const u32 out_bits = (op == NEON_SADDLV || op == NEON_UADDLV) ? bits * 2 : bits;
    auto narrow = Reinterpret(result, VectorBytesType(out_bits / 8));
    WriteVRegister(instr->GetRd(), narrow);
}

void VisitNEON3Different(const vixl::aarch64::Instruction* instr) override {
    if (TryNEONHighNarrow(instr)) return;
    if (TryNEONAbsoluteLong(instr)) return;
    if (TryNEONPolynomial(instr, true)) return;
    if (TryNEONVectorDoublingLong(instr, false)) return;
    const u32 bits = 8u << instr->GetNEONSize();
    const auto op = instr->Mask(NEON3DifferentMask) & ~NEON_Q;
    const bool sign = !(instr->GetInstructionBits() & NEON3SameUBit);
    const bool upper = instr->GetNEONQ();
    const bool multiply = op == NEON_SMULL || op == NEON_UMULL || op == NEON_SMLAL ||
                          op == NEON_UMLAL || op == NEON_SMLSL || op == NEON_UMLSL;
    const bool add = op == NEON_SADDL || op == NEON_UADDL || op == NEON_SADDW || op == NEON_UADDW;
    const bool sub = op == NEON_SSUBL || op == NEON_USUBL || op == NEON_SSUBW || op == NEON_USUBW;
    const bool wide_left =
            op == NEON_SADDW || op == NEON_UADDW || op == NEON_SSUBW || op == NEON_USUBW;
    const bool accumulate =
            op == NEON_SMLAL || op == NEON_UMLAL || op == NEON_SMLSL || op == NEON_UMLSL;
    if (!multiply && !add && !sub) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    if (bits >= 64) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const u32 out_bits = bits * 2;
    auto left = ReadVHalf(instr->GetRn(), upper), right = ReadVHalf(instr->GetRm(), upper);
    auto left_low = wide_left ? ReadVHalf(instr->GetRn(), false) : left;
    auto left_high = wide_left ? ReadVHalf(instr->GetRn(), true) : left;
    auto old_low = accumulate ? ReadVHalf(instr->GetRd(), false) : ImmValue(0, Type::U64);
    auto old_high = accumulate ? ReadVHalf(instr->GetRd(), true) : old_low;
    const u64 mask = (u64{1} << bits) - 1;
    const u64 out_mask = out_bits == 64 ? ~u64{0} : (u64{1} << out_bits) - 1;
    auto low = ImmValue(0, Type::U64), high = low;
    for (u32 index = 0; index < 64 / bits; ++index) {
        auto a = this->LsrImm(wide_left ? (index * out_bits < 64 ? left_low : left_high) : left,
                              Imm{index * (wide_left ? out_bits : bits) % 64})
                         .WithView(Type::U64);
        auto b = this->LsrImm(right, Imm{index * bits}).WithView(Type::U64);
        a = this->And(a, SingleOperand(Imm{wide_left ? out_mask : mask})).WithView(Type::U64);
        b = this->And(b, SingleOperand(Imm{mask})).WithView(Type::U64);
        if (sign) {
            if (!wide_left) a = SignExtendValue(a, bits);
            b = SignExtendValue(b, bits);
        }
        auto value = multiply ? this->Mul(a, SingleOperand(b))
                     : add    ? this->Add(a, SingleOperand(b))
                              : this->Sub(a, SingleOperand(b));
        value.WithView(Type::U64);
        if (accumulate) {
            auto old = this->LsrImm(index * out_bits < 64 ? old_low : old_high,
                                    Imm{index * out_bits % 64})
                               .WithView(Type::U64);
            value = (op == NEON_SMLSL || op == NEON_UMLSL) ? this->Sub(old, SingleOperand(value))
                                                           : this->Add(old, SingleOperand(value));
            value.WithView(Type::U64);
        }
        value = this->And(value, SingleOperand(Imm{out_mask})).WithView(Type::U64);
        value = this->LslImm(value, Imm{index * out_bits % 64}).WithView(Type::U64);
        auto& packed = index * out_bits < 64 ? low : high;
        packed = this->Or(packed, SingleOperand(value)).WithView(Type::U64);
    }
    WriteVHalves(instr->GetRd(), low, high);
}

bool TryNEONSaturatingArithmetic(const vixl::aarch64::Instruction* instr) {
    const auto op = instr->Mask(NEON3SameMask);
    if (op != NEON_SQADD && op != NEON_UQADD && op != NEON_SQSUB && op != NEON_UQSUB) return false;
    const bool add = op == NEON_SQADD || op == NEON_UQADD;
    const bool sign = op == NEON_SQADD || op == NEON_SQSUB;
    const u32 bits = 8u << instr->GetNEONSize();
    const bool q = instr->GetNEONQ();
    if (bits == 64 && !q) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    Value saturated;
    if (bits <= 16) {
        auto left = ReadVRegister(instr->GetRn(), Type::V128);
        auto right = ReadVRegister(instr->GetRm(), Type::V128);
        if (!q) {
            auto mask = this->VecLoadConst(Imm{~u64{0}}, Imm{u64{0}}).WithView(Type::V128);
            left = this->VecAnd(left, mask).WithView(Type::V128);
            right = this->VecAnd(right, mask).WithView(Type::V128);
        }
        auto result = add ? this->VecSatAdd(left, right, Imm{bits}, Imm{sign})
                          : this->VecSatSub(left, right, Imm{bits}, Imm{sign});
        result.WithView(Type::V128);
        auto wrapped =
                add ? this->VecAdd(left, right, Imm{bits}) : this->VecSub(left, right, Imm{bits});
        wrapped.WithView(Type::V128);
        auto different = this->VecXor(result, wrapped).WithView(Type::V128);
        auto low = this->VecExtract64(different, Imm{false}).WithView(Type::U64);
        auto high = this->VecExtract64(different, Imm{true}).WithView(Type::U64);
        auto any = this->Or(low, SingleOperand(high)).WithView(Type::U64);
        saturated = this->TestNotZero(any).WithView(Type::U8);
        if (!q) result = Reinterpret(result, Type::V64);
        WriteVRegister(instr->GetRd(), result);
    } else {
        auto left_low = ReadVHalf(instr->GetRn(), false),
             left_high = ReadVHalf(instr->GetRn(), true);
        auto right_low = ReadVHalf(instr->GetRm(), false),
             right_high = ReadVHalf(instr->GetRm(), true);
        auto low = ImmValue(0, Type::U64), high = low;
        saturated = ImmValue(0, Type::U8);
        const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
        for (u32 lane = 0; lane < (q ? 128u : 64u) / bits; ++lane) {
            const u32 position = lane * bits;
            auto a = this->LsrImm(position < 64 ? left_low : left_high, Imm{position % 64})
                             .WithView(Type::U64);
            auto b = this->LsrImm(position < 64 ? right_low : right_high, Imm{position % 64})
                             .WithView(Type::U64);
            a = this->And(a, SingleOperand(Imm{mask})).WithView(Type::U64);
            b = this->And(b, SingleOperand(Imm{mask})).WithView(Type::U64);
            auto result = add ? this->Add(a, SingleOperand(b)) : this->Sub(a, SingleOperand(b));
            result.WithView(Type::U64);
            result = this->And(result, SingleOperand(Imm{mask})).WithView(Type::U64);
            auto invert = [&](Value value) {
                return this->Xor(value, SingleOperand(Imm{mask})).WithView(Type::U64);
            };
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
                // The top carry/borrow bit remains valid even for 64-bit lanes.
                auto first = this->And(add ? a : invert(a), SingleOperand(b)).WithView(Type::U64);
                auto related = add ? this->Or(a, SingleOperand(b)) : this->Xor(a, SingleOperand(b));
                related.WithView(Type::U64);
                auto second = this->And(add ? related : invert(related),
                                        SingleOperand(add ? invert(result) : result))
                                      .WithView(Type::U64);
                auto carry = this->Or(first, SingleOperand(second)).WithView(Type::U64);
                overflow = this->TestBit(carry, Imm{bits - 1}).WithView(Type::U8);
                limit = ImmValue(add ? mask : 0, Type::U64);
            }
            saturated = this->Or(saturated, SingleOperand(overflow)).WithView(Type::U8);
            result = this->Select(overflow, limit, result).WithView(Type::U64);
            result = this->LslImm(result, Imm{position % 64}).WithView(Type::U64);
            auto& half = position < 64 ? low : high;
            half = this->Or(half, SingleOperand(result)).WithView(Type::U64);
        }
        WriteVHalves(instr->GetRd(), low, high);
    }
    Uniform status{u32(offsetof(GuestState, fpsr)), Type::U32};
    auto old = this->template LoadUniform<Type::U32>(status);
    auto qc = this->Select(saturated, ImmValue(u64{1} << 27, Type::U32), ImmValue(0, Type::U32))
                      .WithView(Type::U32);
    this->StoreUniform(status, this->template Or<Type::U32>(old, SingleOperand(qc)));
    return true;
}
