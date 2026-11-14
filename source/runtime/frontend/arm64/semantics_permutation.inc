void VisitNEONCopy(const vixl::aarch64::Instruction* instr) override {
    const u32 imm5 = instr->GetImmNEON5();
    if ((imm5 & 15) == 0) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const u32 size = vixl::CountTrailingZeros(imm5);
    const u32 bits = 8u << size;
    const u32 index = imm5 >> (size + 1);
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    const bool is_q = instr->GetNEONQ() != 0;
    const u8 rn = instr->GetRn();
    const u8 rd = instr->GetRd();
    auto extract = [&](u32 lane) {
        auto value = ReadVHalf(rn, lane * bits >= 64);
        const u32 shift = (lane * bits) % 64;
        if (shift) {
            value = this->template LsrImm<Type::U64>(value, Imm{shift}).WithView(Type::U64);
        }
        return this->template And<Type::U64>(value, SingleOperand(Imm{mask})).WithView(Type::U64);
    };

    const bool ins_element = instr->Mask(NEONCopyInsElementMask) == NEON_INS_ELEMENT;
    const bool ins_general = instr->Mask(NEONCopyInsGeneralMask) == NEON_INS_GENERAL;
    if (ins_element || ins_general) {
        const u32 source_index = (instr->GetInstructionBits() >> 11 & 15) >> size;
        auto source = ins_element ? extract(source_index) : ReadXRegister(rn);
        source =
                this->template And<Type::U64>(source, SingleOperand(Imm{mask})).WithView(Type::U64);
        const u32 shift = (index * bits) % 64;
        if (shift) {
            source = this->template LslImm<Type::U64>(source, Imm{shift}).WithView(Type::U64);
        }
        const bool high = index * bits >= 64;
        auto old = ReadVHalf(rd, high);
        auto kept = this->template And<Type::U64>(old, SingleOperand(Imm{~(mask << shift)}))
                            .WithView(Type::U64);
        auto value = this->template Or<Type::U64>(kept, SingleOperand(source)).WithView(Type::U64);
        this->StoreUniform(Uniform{VRegOff(rd) + (high ? 8u : 0u), Type::U64}, value);
        return;
    }

    const auto op = instr->Mask(NEONCopyDupElementMask);
    if (op == NEON_DUP_ELEMENT || op == NEON_DUP_GENERAL) {
        if (!is_q && bits == 64) {
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return;
        }
        if (op == NEON_DUP_ELEMENT && is_q && (bits == 32 || bits == 64)) {
            const u32 shuf = bits == 32 ? index * 0x55u : (index ? 0xEEu : 0x44u);
            auto vec = this->VecShuffle32(ReadVRegister(rn, Type::V128), Imm{shuf})
                               .WithView(Type::V128);
            WriteQRegister(rd, vec);
            return;
        }
        Value half = op == NEON_DUP_ELEMENT
                             ? extract(index)
                             : (bits == 64 ? ReadXRegister(rn)
                                           : this->template And<Type::U64>(
                                                         ReadXRegister(rn),
                                                         SingleOperand(Imm{mask}))
                                                     .WithView(Type::U64));
        half = ReplicateTo64(half, bits);
        if (is_q)
            WriteQRegister(rd, this->VecDup64(half).WithView(Type::V128));
        else
            WriteDRegister(rd, half);
        return;
    }
    if (op == NEON_UMOV || op == NEON_SMOV) {
        if ((op == NEON_UMOV && (is_q ? bits != 64 : bits == 64)) ||
            (op == NEON_SMOV && bits >= (is_q ? 64u : 32u))) {
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return;
        }
        auto value = extract(index);
        if (op == NEON_SMOV) value = SignExtendValue(value, bits);
        if (is_q)
            WriteXRegister(rd, value);
        else
            WriteWRegister(rd, value);
    } else {
        Interrupt(InterruptReason::FALLBACK, current_pc);
    }
}

void VisitNEONExtract(const vixl::aarch64::Instruction* instr) override {
    if (instr->Mask(NEONExtractMask) != NEON_EXT) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }

    auto rn_low = ReadVHalf(instr->GetRn(), false);
    const u32 index = instr->GetImmNEONExt();
    if (!instr->GetNEONQ()) {
        if (index >= 8) {
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return;
        }
        auto low = rn_low;
        if (index != 0) {
            const u32 shift = index * 8;
            auto rm_low = ReadVHalf(instr->GetRm(), false);
            low = this->template Or<Type::U64>(this->template LsrImm<Type::U64>(rn_low, Imm{shift}),
                                               SingleOperand(this->template LslImm<Type::U64>(
                                                       rm_low, Imm{64 - shift})))
                          .WithView(Type::U64);
        }
        WriteVRegister(instr->GetRd(), low);
        return;
    }
    auto rn_high = ReadVHalf(instr->GetRn(), true);
    auto rm_low = ReadVHalf(instr->GetRm(), false);
    auto rm_high = ReadVHalf(instr->GetRm(), true);
    if (index == 0) {
        WriteVHalves(instr->GetRd(), rn_low, rn_high);
        return;
    }
    if (index == 8) {
        WriteVHalves(instr->GetRd(), rn_high, rm_low);
        return;
    }

    auto combine = [&](Value low, Value high, u32 bytes) {
        const u32 shift = bytes * 8;
        return this
                ->template Or<Type::U64>(
                        this->template LsrImm<Type::U64>(low, Imm{shift}),
                        SingleOperand(this->template LslImm<Type::U64>(high, Imm{64 - shift})))
                .WithView(Type::U64);
    };
    if (index < 8) {
        WriteVHalves(
                instr->GetRd(), combine(rn_low, rn_high, index), combine(rn_high, rm_low, index));
    } else {
        const u32 tail = index - 8;
        WriteVHalves(
                instr->GetRd(), combine(rn_high, rm_low, tail), combine(rm_low, rm_high, tail));
    }
}

void VisitNEONScalarCopy(const vixl::aarch64::Instruction* instr) override {
    const u32 imm5 = instr->GetImmNEON5();
    if ((imm5 & 15) == 0) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const u32 bits = 8u << vixl::CountTrailingZeros(imm5);
    const u32 index = imm5 >> (vixl::CountTrailingZeros(imm5) + 1);
    auto value = ReadVHalf(instr->GetRn(), index * bits >= 64);
    const u32 shift = (index * bits) % 64;
    if (shift) {
        value = this->template LsrImm<Type::U64>(value, Imm{shift}).WithView(Type::U64);
    }
    if (bits < 64) {
        value = this->template And<Type::U64>(value, SingleOperand(Imm{(u64{1} << bits) - 1}))
                        .WithView(Type::U64);
    }
    WriteVRegister(instr->GetRd(), value);
}

void VisitNEONPerm(const vixl::aarch64::Instruction* instr) override {
    const u32 bits = 8u << instr->GetNEONSize();
    const bool q = instr->GetNEONQ();
    const u32 lanes = (q ? 128 : 64) / bits;
    const auto op = instr->Mask(NEONPermMask);
    const bool second = op == NEON_UZP2 || op == NEON_ZIP2 || op == NEON_TRN2;
    if (lanes < 2) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    if (q) {
        auto left = ReadVRegister(instr->GetRn(), Type::V128);
        auto right = ReadVRegister(instr->GetRm(), Type::V128);
        Value result;
        if (op == NEON_UZP1 || op == NEON_UZP2)
            result = this->VecUnzip(left, right, Imm{bits}, Imm{second});
        else if (op == NEON_ZIP1 || op == NEON_ZIP2)
            result = this->VecZip(left, right, Imm{bits}, Imm{second});
        else if (op == NEON_TRN1 || op == NEON_TRN2) {
            auto a = this->VecUnzip(left, left, Imm{bits}, Imm{second}).WithView(Type::V128);
            auto b = this->VecUnzip(right, right, Imm{bits}, Imm{second}).WithView(Type::V128);
            result = this->VecZip(a, b, Imm{bits}, Imm{false});
        } else {
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return;
        }
        WriteVRegister(instr->GetRd(), result.WithView(Type::V128));
        return;
    }
    auto left = ReadVHalf(instr->GetRn(), false), right = ReadVHalf(instr->GetRm(), false);
    auto packed = ImmValue(0, Type::U64);
    const u64 mask = (u64{1} << bits) - 1;
    for (u32 index = 0; index < lanes; ++index) {
        Value source;
        u32 lane{};
        if (op == NEON_UZP1 || op == NEON_UZP2) {
            source = index < lanes / 2 ? left : right;
            lane = (index % (lanes / 2)) * 2 + second;
        } else if (op == NEON_ZIP1 || op == NEON_ZIP2) {
            source = index % 2 ? right : left;
            lane = index / 2 + second * (lanes / 2);
        } else if (op == NEON_TRN1 || op == NEON_TRN2) {
            source = index % 2 ? right : left;
            lane = (index / 2) * 2 + second;
        } else {
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return;
        }
        auto value = this->LsrImm(source, Imm{lane * bits}).WithView(Type::U64);
        value = this->And(value, SingleOperand(Imm{mask})).WithView(Type::U64);
        value = this->LslImm(value, Imm{index * bits}).WithView(Type::U64);
        packed = this->Or(packed, SingleOperand(value)).WithView(Type::U64);
    }
    WriteVRegister(instr->GetRd(), packed);
}

void VisitNEONTable(const vixl::aarch64::Instruction* instr) override {
    const u32 count = ((instr->GetInstructionBits() >> 13) & 3) + 1;
    auto indexes = ReadVRegister(instr->GetRm(), Type::V128);
    auto nibble_mask =
            this->VecLoadConst(Imm{u64{0xf0f0f0f0f0f0f0f0ULL}}, Imm{u64{0xf0f0f0f0f0f0f0f0ULL}})
                    .WithView(Type::V128);
    auto upper = this->VecAnd(indexes, nibble_mask).WithView(Type::V128);
    auto result = this->VecSharedZero().WithView(Type::V128);
    auto valid = result;
    for (u32 table = 0; table < count; ++table) {
        const u64 repeated = 0x0101010101010101ULL * (table * 16);
        auto id = this->VecLoadConst(Imm{repeated}, Imm{repeated}).WithView(Type::V128);
        auto mask = this->VecCmpEq(upper, id, Imm{8}).WithView(Type::V128);
        auto source = ReadVRegister((instr->GetRn() + table) & 31, Type::V128);
        auto looked = this->VecTableLookup8(source, indexes).WithView(Type::V128);
        auto selected = this->VecAnd(looked, mask).WithView(Type::V128);
        result = this->VecOr(result, selected).WithView(Type::V128);
        valid = this->VecOr(valid, mask).WithView(Type::V128);
    }
    if (instr->GetInstructionBits() & NEONTableExt) {
        auto retained = this->VecAndNot(ReadVRegister(instr->GetRd(), Type::V128), valid)
                                .WithView(Type::V128);
        result = this->VecOr(result, retained).WithView(Type::V128);
    }
    if (!instr->GetNEONQ()) result = Reinterpret(result, Type::V64);
    WriteVRegister(instr->GetRd(), result);
}

bool TryNEONBitMisc(const vixl::aarch64::Instruction* instr) {
    const auto op = instr->Mask(NEON2RegMiscMask);
    const u32 size = instr->GetNEONSize(), bits = 8u << size;
    const bool q = instr->GetNEONQ();
    const bool narrow = op == NEON_XTN || op == NEON_SQXTN || op == NEON_UQXTN || op == NEON_SQXTUN;
    const bool count = op == NEON_CLS || op == NEON_CLZ;
    const u32 group_bits = op == NEON_REV64   ? 64
                           : op == NEON_REV32 ? 32
                           : op == NEON_REV16 ? 16
                                              : 0;
    const bool reverse_bits = op == NEON_RBIT_NOT && size == 1;
    if (op == NEON_RBIT_NOT && size > 1) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    if (!narrow && !count && !group_bits && op != NEON_CNT && !reverse_bits) return false;
    if ((narrow || count) ? size == 3
        : group_bits      ? bits >= group_bits
                          : op == NEON_CNT && size != 0) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    const auto input_low = ReadVHalf(instr->GetRn(), false);
    const auto input_high = ReadVHalf(instr->GetRn(), true);
    auto zero = ImmValue(0, Type::U64);
    auto low = zero, high = zero;
    if (op == NEON_CNT || reverse_bits) {
        auto apply = [&](Value value) {
            if (op == NEON_CNT) {
                auto shifted = this->LsrImm(value, Imm{1}).WithView(Type::U64);
                shifted = this->And(shifted, SingleOperand(Imm{u64{0x5555555555555555}}))
                                  .WithView(Type::U64);
                value = this->Sub(value, SingleOperand(shifted)).WithView(Type::U64);
                shifted = this->LsrImm(value, Imm{2}).WithView(Type::U64);
                shifted = this->And(shifted, SingleOperand(Imm{u64{0x3333333333333333}}))
                                  .WithView(Type::U64);
                value = this->And(value, SingleOperand(Imm{u64{0x3333333333333333}}))
                                .WithView(Type::U64);
                value = this->Add(value, SingleOperand(shifted)).WithView(Type::U64);
                shifted = this->LsrImm(value, Imm{4}).WithView(Type::U64);
                value = this->Add(value, SingleOperand(shifted)).WithView(Type::U64);
                return this->And(value, SingleOperand(Imm{u64{0x0F0F0F0F0F0F0F0F}}))
                        .WithView(Type::U64);
            }
            const u64 masks[] = {0x5555555555555555, 0x3333333333333333, 0x0F0F0F0F0F0F0F0F};
            for (u32 stage = 0; stage < 3; ++stage) {
                auto right = this->LsrImm(value, Imm{1u << stage}).WithView(Type::U64);
                right = this->And(right, SingleOperand(Imm{masks[stage]})).WithView(Type::U64);
                auto left = this->And(value, SingleOperand(Imm{masks[stage]})).WithView(Type::U64);
                left = this->LslImm(left, Imm{1u << stage}).WithView(Type::U64);
                value = this->Or(left, SingleOperand(right)).WithView(Type::U64);
            }
            return value;
        };
        low = apply(input_low);
        if (q) high = apply(input_high);
    } else {
        const u32 source_bits = narrow ? bits * 2 : bits;
        const u64 mask = (u64{1} << bits) - 1;
        const u64 source_mask = source_bits == 64 ? ~u64{0} : (u64{1} << source_bits) - 1;
        auto saturated = ImmValue(0, Type::U8);
        const u32 lanes = (narrow ? 64 : q ? 128 : 64) / bits;
        for (u32 lane = 0; lane < lanes; ++lane) {
            const u32 source_lane =
                    group_bits ? lane / (group_bits / bits) * (group_bits / bits) +
                                         (group_bits / bits - 1 - lane % (group_bits / bits))
                               : lane;
            const u32 position = source_lane * source_bits;
            auto value = this->LsrImm(position < 64 ? input_low : input_high, Imm{position % 64})
                                 .WithView(Type::U64);
            value = this->And(value, SingleOperand(Imm{source_mask})).WithView(Type::U64);
            if (count) {
                if (op == NEON_CLS) {
                    auto sign = this->TestBit(value, Imm{bits - 1}).WithView(Type::U8);
                    auto flip =
                            this->Select(sign, ImmValue(mask, Type::U64), zero).WithView(Type::U64);
                    value = this->Xor(value, SingleOperand(flip)).WithView(Type::U64);
                }
                value = this->CountLeadingZeros64(value).WithView(Type::U64);
                value = this->Sub(value,
                                  SingleOperand(Imm{64u - bits + (op == NEON_CLS ? 1u : 0u)}))
                                .WithView(Type::U64);
            } else if (narrow && op != NEON_XTN) {
                const bool signed_source = op != NEON_UQXTN;
                if (signed_source) value = SignExtendValue(value, source_bits);
                auto trimmed = this->And(value, SingleOperand(Imm{mask})).WithView(Type::U64);
                if (op == NEON_SQXTN) trimmed = SignExtendValue(trimmed, bits);
                auto mismatch = this->Xor(value, SingleOperand(trimmed)).WithView(Type::U64);
                auto overflow = this->TestNotZero(mismatch).WithView(Type::U8);
                saturated = this->Or(saturated, SingleOperand(overflow)).WithView(Type::U8);
                Value limit = ImmValue(mask, Type::U64);
                if (signed_source) {
                    auto sign = this->TestBit(value, Imm{63}).WithView(Type::U8);
                    limit = this->Select(sign,
                                         ImmValue(op == NEON_SQXTN ? u64{1} << (bits - 1) : 0,
                                                  Type::U64),
                                         ImmValue(op == NEON_SQXTN ? (u64{1} << (bits - 1)) - 1
                                                                   : mask,
                                                  Type::U64))
                                    .WithView(Type::U64);
                }
                value = this->Select(overflow, limit, value).WithView(Type::U64);
            }
            value = this->And(value, SingleOperand(Imm{mask})).WithView(Type::U64);
            value = this->LslImm(value, Imm{lane * bits % 64}).WithView(Type::U64);
            auto& half = lane * bits < 64 ? low : high;
            half = this->Or(half, SingleOperand(value)).WithView(Type::U64);
        }
        if (narrow && op != NEON_XTN) {
            Uniform status{u32(offsetof(GuestState, fpsr)), Type::U32};
            auto old = this->template LoadUniform<Type::U32>(status);
            auto qc = this->Select(saturated, ImmValue(1u << 27, Type::U32), ImmValue(0, Type::U32))
                              .WithView(Type::U32);
            this->StoreUniform(status, this->template Or<Type::U32>(old, SingleOperand(qc)));
        }
    }
    if (narrow && q)
        WriteVHalves(instr->GetRd(), ReadVHalf(instr->GetRd(), false), low);
    else
        WriteVHalves(instr->GetRd(), low, high);
    return true;
}
