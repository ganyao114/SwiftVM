void VisitCryptoAES(const vixl::aarch64::Instruction* instr) override {
    // VIXL's AES visitor does not validate the opcode or size field. All four
    // allocated encodings operate on exactly sixteen bytes.
    constexpr u32 kCrypto2RegMask = 0xFFFFFC00u;
    constexpr u32 kAeseOp = 0x4E284800u, kAesdOp = 0x4E285800u;
    constexpr u32 kAesmcOp = 0x4E286800u, kAesimcOp = 0x4E287800u;
    const u32 op = instr->GetInstructionBits() & kCrypto2RegMask;
    if (op != kAeseOp && op != kAesdOp && op != kAesmcOp && op != kAesimcOp) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    auto source = ReadVRegister(instr->GetRn(), Type::V128);
    auto zero = this->VecSharedZero().WithView(Type::V128);
    Value result;
    if (op == kAeseOp || op == kAesdOp) {
        // ARM adds the key before substitution; shared AES IR follows x86 and
        // adds it afterward. Snapshot both operands before an aliased write.
        auto old = ReadVRegister(instr->GetRd(), Type::V128);
        auto keyed = this->VecXor(old, source).WithView(Type::V128);
        result = (op == kAeseOp ? this->VecAesEncLast(keyed, zero)
                                : this->VecAesDecLast(keyed, zero))
                         .WithView(Type::V128);
    } else if (op == kAesmcOp) {
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

void WritePackedWords32(u8 rd, const std::array<Value, 4>& words) {
    auto pair = [&](u32 i) {
        auto upper = this->template LslImm<Type::U64>(Widen(words[i + 1]), Imm{32u})
                             .WithView(Type::U64);
        return this->template Or<Type::U64>(Widen(words[i]), SingleOperand(upper))
                .WithView(Type::U64);
    };
    WriteVHalves(rd, pair(0), pair(2));
}

void VisitCrypto2RegSHA(const vixl::aarch64::Instruction* instr) override {
    constexpr u32 kCrypto2RegMask = 0xFFFFFC00u;
    constexpr u32 kSha1hOp = 0x5E280800u, kSha1su1Op = 0x5E281800u, kSha256su0Op = 0x5E282800u;
    const u32 op = instr->GetInstructionBits() & kCrypto2RegMask;
    if (op != kSha1hOp && op != kSha1su1Op && op != kSha256su0Op) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    if (op == kSha256su0Op) {
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
    if (op == kSha1hOp) {
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
    WritePackedWords32(instr->GetRd(), result);
}

Value EmitSha256Rounds(bool high_half, Value old, Value source, Value message) {
    auto abcd = high_half ? source : old;
    auto efgh = high_half ? old : source;
    auto reversed_a = this->VecShuffle32(abcd, Imm{0x1Bu}).WithView(Type::V128);
    auto reversed_e = this->VecShuffle32(efgh, Imm{0x1Bu}).WithView(Type::V128);
    auto packed_source =
            this->VecShuffle32TwoSrc(reversed_e, reversed_a, Imm{0xEEu}).WithView(Type::V128);
    auto packed_destination =
            this->VecShuffle32TwoSrc(reversed_e, reversed_a, Imm{0x44u}).WithView(Type::V128);
    auto first =
            this->VecSha256Rnds2(packed_destination, packed_source, message).WithView(Type::V128);
    // After two rounds the other packed half equals the original source.
    auto second_keys = this->VecShuffle32(message, Imm{0x0Eu}).WithView(Type::V128);
    auto second = this->VecSha256Rnds2(packed_source, first, second_keys).WithView(Type::V128);
    return this->VecShuffle32TwoSrc(second, first, Imm{high_half ? 0x11u : 0xBBu})
            .WithView(Type::V128);
}

void VisitCrypto3RegSHA(const vixl::aarch64::Instruction* instr) override {
    constexpr u32 kCrypto3RegMask = 0xFFE0FC00u;
    constexpr u32 kSha1cOp = 0x5E000000u, kSha1pOp = 0x5E001000u, kSha1mOp = 0x5E002000u;
    constexpr u32 kSha1su0Op = 0x5E003000u, kSha256hOp = 0x5E004000u, kSha256h2Op = 0x5E005000u;
    constexpr u32 kSha256su1Op = 0x5E006000u;
    const u32 op = instr->GetInstructionBits() & kCrypto3RegMask;
    if (op < kSha1cOp || op > kSha256su1Op || ((op - kSha1cOp) & 0xFFFu)) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    auto old = ReadVRegister(instr->GetRd(), Type::V128);
    auto source = ReadVRegister(instr->GetRn(), Type::V128);
    auto message = ReadVRegister(instr->GetRm(), Type::V128);
    if (op == kSha256su1Op) {
        auto addend = this->VecExtractBytes(source, message, Imm{4u}).WithView(Type::V128);
        auto partial = this->VecAdd(old, addend, Imm{32u}).WithView(Type::V128);
        WriteVRegister(instr->GetRd(), this->VecSha256Msg2(partial, message).WithView(Type::V128));
        return;
    }
    if (op == kSha256hOp || op == kSha256h2Op) {
        WriteVRegister(instr->GetRd(), EmitSha256Rounds(op == kSha256h2Op, old, source, message));
        return;
    }
    (void)kSha1mOp;
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
    if (op == kSha1su0Op) {
        const auto initial = d;
        for (u32 i = 0; i < 4; ++i)
            d[i] = xor32(xor32(initial[i], i < 2 ? initial[i + 2] : n[i - 2]), m[i]);
    } else {
        auto e = n[0];
        for (u32 round = 0; round < 4; ++round) {
            Value function;
            if (op == kSha1cOp) {
                function = xor32(d[3], and32(d[1], xor32(d[2], d[3])));
            } else if (op == kSha1pOp) {
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
    WritePackedWords32(instr->GetRd(), d);
}
