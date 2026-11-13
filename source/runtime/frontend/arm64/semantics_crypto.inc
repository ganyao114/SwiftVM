void VisitCryptoAES(const vixl::aarch64::Instruction* instr) override {
    // VIXL's AES visitor does not validate the opcode or size field. All four
    // allocated encodings operate on exactly sixteen bytes.
    const u32 op = instr->GetInstructionBits() & 0xFFFFFC00u;
    constexpr u32 aese = 0x4E284800, aesd = 0x4E285800;
    constexpr u32 aesmc = 0x4E286800, aesimc = 0x4E287800;
    if (op != aese && op != aesd && op != aesmc && op != aesimc) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    auto source = ReadVRegister(instr->GetRn(), Type::V128);
    auto zero = this->VecDup64(ImmValue(0, Type::U64)).WithView(Type::V128);
    Value result;
    if (op == aese || op == aesd) {
        // ARM adds the key before substitution; shared AES IR follows x86 and
        // adds it afterward. Snapshot both operands before an aliased write.
        auto old = ReadVRegister(instr->GetRd(), Type::V128);
        auto keyed = this->VecXor(old, source).WithView(Type::V128);
        result = (op == aese ? this->VecAesEncLast(keyed, zero) : this->VecAesDecLast(keyed, zero))
                         .WithView(Type::V128);
    } else if (op == aesmc) {
        // Inverse ShiftRows/SubBytes followed by the forward transforms cancel,
        // leaving MixColumns. This keeps AES-capable hosts on native vector IR.
        auto inverse = this->VecAesDecLast(source, zero).WithView(Type::V128);
        result = this->VecAesEnc(inverse, zero).WithView(Type::V128);
    } else {
        auto forward = this->VecAesEncLast(source, zero).WithView(Type::V128);
        result = this->VecAesDec(forward, zero).WithView(Type::V128);
    }
    WriteVRegister(instr->GetRd(), result);
}

void VisitCrypto2RegSHA(const vixl::aarch64::Instruction* instr) override {
    const u32 op = instr->GetInstructionBits() & 0xFFFFFC00u;
    if (op != 0x5E280800 && op != 0x5E281800 && op != 0x5E282800) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    if (op == 0x5E282800) {
        auto old = ReadVRegister(instr->GetRd(), Type::V128);
        auto source = ReadVRegister(instr->GetRn(), Type::V128);
        WriteVRegister(instr->GetRd(), this->VecSha256Msg1(old, source).WithView(Type::V128));
        return;
    }
    auto words = [&](u32 reg) {
        std::array<Value, 4> result;
        auto low = ReadVHalf(reg, false), high = ReadVHalf(reg, true);
        for (u32 i = 0; i < 4; ++i) {
            auto shifted = this->template LsrImm<Type::U64>(i < 2 ? low : high, Imm{(i % 2) * 32})
                                   .WithView(Type::U64);
            result[i] = this->template And<Type::U32>(shifted, SingleOperand(Imm{u32{0xFFFFFFFF}}))
                                .WithView(Type::U32);
        }
        return result;
    };
    const auto source = words(instr->GetRn());
    auto rotate = [&](Value word, u32 right) {
        return this->template RorImm<Type::U32>(word, Imm{right}).WithView(Type::U32);
    };
    if (op == 0x5E280800) {
        WriteVRegister(instr->GetRd(), rotate(source[0], 2));
        return;
    }
    const auto old = words(instr->GetRd());
    std::array<Value, 4> result;
    for (u32 i = 0; i < 4; ++i) {
        auto other = i < 3 ? source[i + 1] : result[0];
        result[i] = rotate(
                this->template Xor<Type::U32>(old[i], SingleOperand(other)).WithView(Type::U32),
                31);
    }
    auto pair = [&](u32 i) {
        auto upper = this->template LslImm<Type::U64>(Widen(result[i + 1]), Imm{32u})
                             .WithView(Type::U64);
        return this->template Or<Type::U64>(Widen(result[i]), SingleOperand(upper))
                .WithView(Type::U64);
    };
    WriteVHalves(instr->GetRd(), pair(0), pair(2));
}

void VisitCrypto3RegSHA(const vixl::aarch64::Instruction* instr) override {
    const u32 op = instr->GetInstructionBits() & 0xFFE0FC00u;
    if (op < 0x5E000000 || op > 0x5E006000 || ((op - 0x5E000000) & 0xFFF)) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    auto old = ReadVRegister(instr->GetRd(), Type::V128);
    auto source = ReadVRegister(instr->GetRn(), Type::V128);
    auto message = ReadVRegister(instr->GetRm(), Type::V128);
    if (op == 0x5E006000) {
        auto addend = this->VecExtractBytes(source, message, Imm{4u}).WithView(Type::V128);
        auto partial = this->VecAdd(old, addend, Imm{32u}).WithView(Type::V128);
        WriteVRegister(instr->GetRd(), this->VecSha256Msg2(partial, message).WithView(Type::V128));
        return;
    }
    if (op == 0x5E004000 || op == 0x5E005000) {
        auto abcd = op == 0x5E004000 ? old : source;
        auto efgh = op == 0x5E004000 ? source : old;
        auto reversed_a = this->VecShuffle32(abcd, Imm{0x1Bu}).WithView(Type::V128);
        auto reversed_e = this->VecShuffle32(efgh, Imm{0x1Bu}).WithView(Type::V128);
        auto packed_source =
                this->VecShuffle32TwoSrc(reversed_e, reversed_a, Imm{0xEEu}).WithView(Type::V128);
        auto packed_destination =
                this->VecShuffle32TwoSrc(reversed_e, reversed_a, Imm{0x44u}).WithView(Type::V128);
        auto first = this->VecSha256Rnds2(packed_destination, packed_source, message)
                             .WithView(Type::V128);
        // After two rounds the other packed half equals the original source.
        auto second_keys = this->VecShuffle32(message, Imm{0x0Eu}).WithView(Type::V128);
        auto second = this->VecSha256Rnds2(packed_source, first, second_keys).WithView(Type::V128);
        auto result = this->VecShuffle32TwoSrc(second, first, Imm{op == 0x5E004000 ? 0xBBu : 0x11u})
                              .WithView(Type::V128);
        WriteVRegister(instr->GetRd(), result);
        return;
    }
    auto words = [&](Value vector) {
        std::array<Value, 4> result;
        auto low = this->VecExtract64(vector, Imm{0u}).WithView(Type::U64);
        auto high = this->VecExtract64(vector, Imm{1u}).WithView(Type::U64);
        for (u32 i = 0; i < 4; ++i) {
            auto shifted = this->template LsrImm<Type::U64>(i < 2 ? low : high, Imm{(i % 2) * 32})
                                   .WithView(Type::U64);
            result[i] = this->template And<Type::U32>(shifted, SingleOperand(Imm{u32{0xFFFFFFFF}}))
                                .WithView(Type::U32);
        }
        return result;
    };
    auto d = words(old);
    const auto n = words(source), m = words(message);
    auto xor32 = [&](Value a, Value b) {
        return this->template Xor<Type::U32>(a, SingleOperand(b)).WithView(Type::U32);
    };
    auto and32 = [&](Value a, Value b) {
        return this->template And<Type::U32>(a, SingleOperand(b)).WithView(Type::U32);
    };
    auto add32 = [&](Value a, Value b) {
        return this->template Add<Type::U32>(a, SingleOperand(b)).WithView(Type::U32);
    };
    if (op == 0x5E003000) {
        const auto initial = d;
        for (u32 i = 0; i < 4; ++i)
            d[i] = xor32(xor32(initial[i], i < 2 ? initial[i + 2] : n[i - 2]), m[i]);
    } else {
        auto e = n[0];
        for (u32 round = 0; round < 4; ++round) {
            Value function;
            if (op == 0x5E000000) {
                function = xor32(d[3], and32(d[1], xor32(d[2], d[3])));
            } else if (op == 0x5E001000) {
                function = xor32(xor32(d[1], d[2]), d[3]);
            } else {
                auto first = and32(d[1], d[2]);
                auto other = and32(d[3],
                                   this->template Or<Type::U32>(d[1], SingleOperand(d[2]))
                                           .WithView(Type::U32));
                function = this->template Or<Type::U32>(first, SingleOperand(other))
                                   .WithView(Type::U32);
            }
            auto a = this->template RorImm<Type::U32>(d[0], Imm{27u}).WithView(Type::U32);
            auto result = add32(add32(add32(a, function), e), m[round]);
            e = d[3];
            d = {result,
                 d[0],
                 this->template RorImm<Type::U32>(d[1], Imm{2u}).WithView(Type::U32),
                 d[2]};
        }
    }
    auto pair = [&](u32 i) {
        auto upper =
                this->template LslImm<Type::U64>(Widen(d[i + 1]), Imm{32u}).WithView(Type::U64);
        return this->template Or<Type::U64>(Widen(d[i]), SingleOperand(upper)).WithView(Type::U64);
    };
    WriteVHalves(instr->GetRd(), pair(0), pair(2));
}
