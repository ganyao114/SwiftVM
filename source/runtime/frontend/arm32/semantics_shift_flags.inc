Value GetCFlag() { return CondPassed(Cond::CS); }

Value NZFrom(Value value) {
    switch (value.origin) {
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::Adc:
        case OpCode::Sbb:
        case OpCode::And:
        case OpCode::Or:
        case OpCode::Xor:
        case OpCode::AndNot:
            return value;
        default:
            return Or(value, Literal{u64{0}});
    }
}

Value NZCVFrom(Value value) { return value; }

void SaveFlags(Value value, Flag flags) {
    auto producer = sink.ProducerOf(value.raw);
    auto metadata = sink.Metadata(producer);
    metadata.flag_outputs |= FlagBits(flags);
    metadata.profile = value.origin == OpCode::And || value.origin == OpCode::Or ||
                                       value.origin == OpCode::Xor || value.origin == OpCode::AndNot
                               ? FlagProfile::LogicalZeroCVA
                               : FlagProfile::ArithmeticCanonicalCarry;
    if (const auto error = sink.SetMetadata(producer, metadata); error != Error::None)
        throw Rejected{BuildError::IRRejected, error};
    if (value.origin == OpCode::Sub && (FlagBits(flags) & FlagBits(Flag::Carry)))
        Emit<OpCode::InvertCarry>();
}

void SetCpsrNZ(Value value) { SaveFlags(value, Flag::Negate | Flag::Zero); }

void SetCpsrNZC(Value value, Value carry) {
    SetCpsrNZ(value);
    SetCarry(carry);
}

void SetCpsrNZCV(Value value) { SaveFlags(value, Flag::NZCV); }

void SetCpsrNZCVRaw(Value value) {
    // N and Z are independent architectural bits, including the N=Z=1 case.
    SaveFlags(And(value.WithView(Type::U32), Literal{0x80000000u}), Flag::Negate);
    SaveFlags(Or(Select(TestBit(value, Literal{30u}), Imm32(0), Imm32(1)), Literal{0u}),
              Flag::Zero);
    SetCarry(TestBit(value, Literal{29u}));
    SetOverflow(TestBit(value, Literal{28u}));
}

Value AddWithCarry(const Value& a, const Value& b, const Value& carry) {
    if (carry.known && !carry.literal) return Add(a, b);
    if (!carry.known) return Emit<OpCode::Adc>(a, b);
    // a + b + 1 == a - ~b. Sub's no-borrow carry and signed
    // overflow are exactly the ADC carry-out and overflow, including b=-1.
    return Sub(a, Not(b));
}

Value SubWithCarry(const Value& a, const Value& b, const Value& carry) {
    if (carry.known && carry.literal) return Sub(a, b);
    if (!carry.known) return Emit<OpCode::Adc>(a, Not(b));
    return Add(a, Not(b));
}

Value AddWithOverflow(Value a, Value b, Value carry, Value& overflow) {
    const auto result = AddWithCarry(a, b, carry);
    overflow = TestBit(And(Xor(a, result), Xor(b, result)), Literal{TypeBits(a.view) - 1});
    return result;
}

Value CountLeadingZeros(Value value) { return Emit<OpCode::CountLeadingZeros32>(value); }

Value ByteReverseWord(Value value) { return Emit<OpCode::ByteSwap>(value, Literal{32u}); }

Value ByteReverseHalf(Value value) {
    return Or(And(LsrImm(value, Literal{8u}), Literal{0x00ff00ffu}),
              LslImm(And(value, Literal{0x00ff00ffu}), Literal{8u}));
}

ShiftResult LogicalShiftLeft(const Value& value, const Value& amount, const Value& carry) {
    const unsigned width = TypeBits(value.view);
    if (amount.known) {
        const unsigned count = amount.literal & 255;
        if (!count) return {value, carry};
        if (count < width)
            return {LslImm(value, Literal{count}), TestBit(value, Literal{width - count})};
        return {ImmValue(0, value.view),
                count == width ? TestBit(value, Literal{0u}) : Imm1(false)};
    }
    auto count = ZeroExtendByteToWord(amount);
    auto zero = TestZero(count), small = TestBit(Sub(count, Literal{width}), Literal{31u});
    auto shifted = LslValue(value, count);
    auto carry_value = TestNotZero(And(LsrValue(value, Sub(Imm32(width), count)), Literal{u64{1}}));
    carry_value =
            Select(TestBit(Sub(count, Literal{width + 1}), Literal{31u}), carry_value, Imm1(false));
    return {Select(small, shifted, ImmValue(0, value.view)), Select(zero, carry, carry_value)};
}

ShiftResult LogicalShiftRight(const Value& value, const Value& amount, const Value& carry) {
    const unsigned width = TypeBits(value.view);
    if (amount.known) {
        const unsigned count = amount.literal & 255;
        if (!count) return {value, carry};
        if (count < width)
            return {LsrImm(value, Literal{count}), TestBit(value, Literal{count - 1})};
        return {ImmValue(0, value.view),
                count == width ? TestBit(value, Literal{width - 1}) : Imm1(false)};
    }
    const auto count = ZeroExtendByteToWord(amount);
    const auto zero = TestZero(count), small = TestBit(Sub(count, Literal{width}), Literal{31u});
    auto carry_value = TestNotZero(And(LsrValue(value, Sub(count, Literal{1u})), Literal{u64{1}}));
    carry_value =
            Select(TestBit(Sub(count, Literal{width + 1}), Literal{31u}), carry_value, Imm1(false));
    return {Select(small, LsrValue(value, count), ImmValue(0, value.view)),
            Select(zero, carry, carry_value)};
}

ShiftResult ArithmeticShiftRight(const Value& value, const Value& amount, const Value& carry) {
    const unsigned width = TypeBits(value.view);
    const auto sign = TestBit(value, Literal{width - 1});
    if (amount.known) {
        const unsigned count = amount.literal & 255;
        if (!count) return {value, carry};
        return {AsrImm(value, Literal{std::min(count, width - 1)}),
                count < width ? TestBit(value, Literal{count - 1}) : sign};
    }
    const auto count = ZeroExtendByteToWord(amount);
    const auto zero = TestZero(count), small = TestBit(Sub(count, Literal{width}), Literal{31u});
    auto bit = TestNotZero(And(LsrValue(value, Sub(count, Literal{1u})), Literal{u64{1}}));
    return {Select(small, AsrValue(value, count), AsrImm(value, Literal{width - 1})),
            Select(zero, carry, Select(small, bit, sign))};
}

ShiftResult RotateRight(const Value& value, const Value& amount, const Value& carry) {
    const unsigned width = TypeBits(value.view);
    if (amount.known) {
        const unsigned count = amount.literal & 255;
        if (!count) return {value, carry};
        const auto result = count % width ? RorImm(value, Literal{count % width}) : value;
        return {result, TestBit(result, Literal{width - 1})};
    }
    const auto count = ZeroExtendByteToWord(amount);
    const auto result = RorValue(value, count);
    return {result, Select(TestZero(count), carry, TestBit(result, Literal{width - 1}))};
}

ShiftResult RotateRightExtended(const Value& value, const Value& carry) {
    return {Or(LsrImm(value, Literal{1u}), LslImm(carry.WithView(Type::U32), Literal{31u})),
            TestBit(value, Literal{0u})};
}

Value LogicalShiftLeft(Value value, Value amount) {
    return LogicalShiftLeft(value, amount, Imm1(false)).result;
}

Value LogicalShiftRight(Value value, Value amount) {
    return LogicalShiftRight(value, amount, Imm1(false)).result;
}

Value ArithmeticShiftRight(Value value, Value amount) {
    return ArithmeticShiftRight(value, amount, Imm1(false)).result;
}

Value RotateRight(Value value, Value amount) {
    return RotateRight(value, amount, Imm1(false)).result;
}

ShiftResult EmitImmShift(Value value, ShiftType type, Imm<3> imm3, Imm<2> imm2, Value carry) {
    return EmitImmShift(value, type, concatenate(imm3, imm2), carry);
}

ShiftResult EmitImmShift(Value value, ShiftType type, Imm<5> amount, Value carry) {
    auto count = amount.ZeroExtend<u8>();
    switch (type) {
        case ShiftType::LSL:
            return LogicalShiftLeft(value, Imm8(count), carry);
        case ShiftType::LSR:
            return LogicalShiftRight(value, Imm8(count ? count : 32), carry);
        case ShiftType::ASR:
            return ArithmeticShiftRight(value, Imm8(count ? count : 32), carry);
        case ShiftType::ROR:
            return count ? RotateRight(value, Imm8(count), carry)
                         : RotateRightExtended(value, carry);
    }
    return {value, carry};
}

ShiftResult EmitRegShift(Value value, ShiftType type, Value amount, Value carry) {
    switch (type) {
        case ShiftType::LSL:
            return LogicalShiftLeft(value, amount, carry);
        case ShiftType::LSR:
            return LogicalShiftRight(value, amount, carry);
        case ShiftType::ASR:
            return ArithmeticShiftRight(value, amount, carry);
        case ShiftType::ROR:
            return RotateRight(value, amount, carry);
    }
    return {value, carry};
}

ImmAndCarry ArmExpandImm_C(int rotate, Imm<8> immediate, Value carry) {
    const auto value = std::rotr(immediate.ZeroExtend(), rotate * 2);
    return {value, rotate ? Imm1(value >> 31) : carry};
}

u32 ArmExpandImm(int rotate, Imm<8> immediate) {
    return std::rotr(immediate.ZeroExtend(), rotate * 2);
}

ImmAndCarry ThumbExpandImm_C(Imm<1> i, Imm<3> imm3, Imm<8> imm8, Value carry) {
    const auto immediate = concatenate(i, imm3, imm8);
    if (immediate.template Bits<10, 11>() == 0) {
        const u32 byte = imm8.ZeroExtend();
        const unsigned mode = immediate.template Bits<8, 9>();
        const u32 value = mode == 0   ? byte
                          : mode == 1 ? byte | (byte << 16)
                          : mode == 2 ? (byte << 8) | (byte << 24)
                                      : byte * 0x01010101u;
        return {value, carry};
    }
    const auto value =
            std::rotr(0x80u | immediate.template Bits<0, 6>(), immediate.template Bits<7, 11>());
    return {value, Imm1(value >> 31)};
}

u32 ThumbExpandImm(Imm<1> i, Imm<3> imm3, Imm<8> imm8) {
    return ThumbExpandImm_C(i, imm3, imm8, Imm1(false)).imm32;
}
