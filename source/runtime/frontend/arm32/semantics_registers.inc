// ARM core and VFP/NEON register views share the same architectural banks.
u32 PC() const { return current_location.Address() + (current_location.Thumb() ? 4u : 8u); }

u32 AlignPC(unsigned alignment) const { return PC() & ~(alignment - 1); }

Value GetRegister(Reg reg) {
    return reg == Reg::PC
                   ? Imm32(PC())
                   : LoadUniform(Uniform{layout.gprs_offset + u32(RegNumber(reg)) * 8, Type::U32});
}

void SetRegister(Reg reg, Value value) {
    if (reg == Reg::PC) {
        ALUWritePC(value);
        return;
    }
    StoreUniform(Uniform{layout.gprs_offset + u32(RegNumber(reg)) * 8, Type::U64},
                 value.WithView(Type::U32));
}

u32 ExtendedRegisterOffset(ExtReg reg) const {
    return layout.fprs_offset + u32(RegNumber(reg)) * (IsSingleExtReg(reg)   ? 4
                                                       : IsDoubleExtReg(reg) ? 8
                                                                             : 16);
}

Value GetExtendedRegister(ExtReg reg) {
    return LoadUniform(
            Uniform{ExtendedRegisterOffset(reg), IsSingleExtReg(reg) ? Type::U32 : Type::U64});
}

void SetExtendedRegister(ExtReg reg, Value value) {
    const auto type = IsSingleExtReg(reg) ? Type::U32 : Type::U64;
    StoreUniform(Uniform{ExtendedRegisterOffset(reg), type}, value.WithView(type));
}

Value GetVector(ExtReg reg) {
    if (IsQuadExtReg(reg)) return LoadUniform(Uniform{ExtendedRegisterOffset(reg), Type::V128});
    // Native vector carriers are 128 bits, while Dn aliases one independent
    // half of Qn. Its consumer view retains the active 64-bit NEON width.
    return ZeroExtendToQuad(LoadUniform(Uniform{ExtendedRegisterOffset(reg), Type::U64}))
            .WithView(Type::V64);
}

void SetVector(ExtReg reg, Value value) {
    if (IsQuadExtReg(reg))
        StoreUniform(Uniform{ExtendedRegisterOffset(reg), Type::V128}, value.WithView(Type::V128));
    else
        StoreUniform(Uniform{ExtendedRegisterOffset(reg), Type::U64},
                     VecExtract64(value.WithView(Type::V128), Literal{0u}));
}

Value LeastSignificantByte(Value value) { return value.WithView(Type::U8); }

Value LeastSignificantHalf(Value value) { return value.WithView(Type::U16); }

Value LeastSignificantWord(Value value) { return value.WithView(Type::U32); }

ShiftResult MostSignificantWord(Value value) {
    return {LsrImm(value.WithView(Type::U64), Literal{32u}).WithView(Type::U32),
            TestBit(value.WithView(Type::U64), Literal{31u})};
}

Value Pack2x32To1x64(Value lo, Value hi) {
    return Or(ZeroExtendWordToLong(lo), LslImm(ZeroExtendWordToLong(hi), Literal{32u}));
}

Value ZeroExtendByteToWord(Value value) {
    return Emit<OpCode::ZeroExtend32>(value.WithView(Type::U8));
}

Value ZeroExtendHalfToWord(Value value) {
    return Emit<OpCode::ZeroExtend32>(value.WithView(Type::U16));
}

Value ZeroExtendToWord(Value value) { return Emit<OpCode::ZeroExtend32>(value); }

Value ZeroExtendWordToLong(Value value) {
    return Emit<OpCode::ZeroExtend32To64>(value.WithView(Type::U32));
}

Value ZeroExtendToLong(Value value) { return Emit<OpCode::ZeroExtend64>(value); }

Value SignExtendByteToWord(Value value) {
    return Emit<OpCode::SignExtend, Type::U32>(value.WithView(Type::U8));
}

Value SignExtendHalfToWord(Value value) {
    return Emit<OpCode::SignExtend, Type::U32>(value.WithView(Type::U16));
}

Value SignExtendWordToLong(Value value) {
    return Emit<OpCode::SignExtend, Type::U64>(value.WithView(Type::U32));
}

Value GetCpsr() {
    auto flags = Or(LslImm(CondPassed(Cond::MI).WithView(Type::U32), Literal{31u}),
                    LslImm(CondPassed(Cond::EQ).WithView(Type::U32), Literal{30u}));
    flags = Or(flags, LslImm(CondPassed(Cond::CS).WithView(Type::U32), Literal{29u}));
    flags = Or(flags, LslImm(CondPassed(Cond::VS).WithView(Type::U32), Literal{28u}));
    const auto it = current_location.IT().Value();
    const u32 context = (u32(current_location.Thumb()) << 5) |
                        (u32(current_location.EFlag()) << 9) | ((u32(it) & 3) << 25) |
                        ((u32(it) & 252) << 8);
    return Or(Or(flags, LoadUniform(Uniform{layout.psr_aux_offset, Type::U32})), Literal{context});
}

void SetCpsrNZCVQ(Value value) {
    SetCpsrNZCVRaw(value);
    const auto old = LoadUniform(Uniform{layout.psr_aux_offset, Type::U32});
    StoreUniform(Uniform{layout.psr_aux_offset, Type::U64},
                 Or(And(old, Literal{~0x08000000u}), And(value, Literal{0x08000000u})));
}

void OrQFlag(Value value) {
    const auto old = LoadUniform(Uniform{layout.psr_aux_offset, Type::U32});
    StoreUniform(Uniform{layout.psr_aux_offset, Type::U64},
                 Or(old, LslImm(value.WithView(Type::U32), Literal{27u})));
}

Value GetGEFlags() {
    const auto packed = LoadUniform(Uniform{layout.psr_aux_offset, Type::U32});
    return And(LsrImm(packed, Literal{16u}), Literal{15u});
}

void SetGEFlags(Value value) {
    SetGEFlagsCompressed(LslImm(And(value.WithView(Type::U32), Literal{15u}), Literal{16u}));
}

void SetGEFlagsCompressed(Value value) {
    const auto old = LoadUniform(Uniform{layout.psr_aux_offset, Type::U32});
    StoreUniform(Uniform{layout.psr_aux_offset, Type::U64},
                 Or(And(old, Literal{~0x000f0000u}),
                    And(value.WithView(Type::U32), Literal{0x000f0000u})));
}

Value LeastSignificant(size_t bits, Value value) { return value.WithView(ScalarType(bits)); }

Value SignExtendToWord(Value value) { return Emit<OpCode::SignExtend, Type::U32>(value); }

Value SignExtendToLong(Value value) { return Emit<OpCode::SignExtend, Type::U64>(value); }
