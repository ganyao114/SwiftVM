// BMI/ADX scalar semantics in the same Semantics owner.
static bool BmiMnemonic(Op opcode) {
    switch (opcode) {
        case Op::ANDN:
        case Op::BLSR:
        case Op::BLSMSK:
        case Op::BLSI:
        case Op::BZHI:
        case Op::BEXTR:
        case Op::SHLX:
        case Op::SHRX:
        case Op::SARX:
        case Op::RORX:
        case Op::MULX:
        case Op::PDEP:
        case Op::PEXT:
            return true;
        default:
            return false;
    }
}
static bool BmiInstruction(Emitter& builder,
                           StateLayout layout,
                           const InstructionData& instruction,
                           PendingFlags& pending,
                           u64 pc,
                           u64 next_pc,
                           TargetInfo target) {
    const bool adcx = instruction.opcode == Op::ADCX, adox = instruction.opcode == Op::ADOX;
    if (!BmiMnemonic(instruction.opcode) && !adcx && !adox) return false;
    const auto& destination = instruction.ops[0];
    const auto& first = instruction.ops[1];
    const auto& second = instruction.ops[2];
    const auto bits = destination.size;
    const auto type = ScalarType(bits);
    const bool bls = instruction.opcode == Op::BLSR || instruction.opcode == Op::BLSMSK ||
                     instruction.opcode == Op::BLSI;
    const bool rotate = instruction.opcode == Op::RORX, multiply = instruction.opcode == Op::MULX;
    const bool two = bls || adcx || adox;
    const bool invert = instruction.opcode == Op::ANDN;
    const bool parallel = instruction.opcode == Op::PDEP || instruction.opcode == Op::PEXT;
    if ((bits != 32 && bits != 64) || destination.type != GuestOperandKind::Register ||
        instruction.ops[3].type != GuestOperandKind::None)
        Unsupported();
    const auto data = [&](const DecodedOperand& operand) {
        return operand.size == bits &&
               (operand.type == GuestOperandKind::Register || Memory(operand));
    };
    if (!data(first) ||
        (two      ? second.type != GuestOperandKind::None
         : rotate ? second.type != GuestOperandKind::Immediate || second.size != 8
                  : !data(second)) ||
        (multiply && first.type != GuestOperandKind::Register) ||
        (!two && !rotate && !multiply &&
         (invert || parallel ? first.type != GuestOperandKind::Register
                             : second.type != GuestOperandKind::Register)))
        Unsupported();
    // Every register form is admitted before a memory source is touched.
    unsigned slot{};
    for (const auto& operand : instruction.ops)
        if (operand.type == GuestOperandKind::Register)
            (void)RegisterOffset(layout, operand.index, operand.size, slot);
    const bool writes_flags = bls || invert || instruction.opcode == Op::BZHI ||
                              instruction.opcode == Op::BEXTR || adcx || adox;
    if (writes_flags || Memory(first) || Memory(second)) pending.Flush(builder);
    if (Memory(first) || Memory(second)) PrepareFaultPC(builder, layout, pc, target);

    if (multiply) {
        const auto left = ReadRegister(builder, layout, Gpr(2, bits).index, bits);
        const auto right = ReadData(builder, layout, instruction, second, next_pc, target);
        Value low, high;
        if (bits == 64) {
            low = Require(
                    builder.Mul(Type::U64, {Operand::ValueOf(left), Operand::ValueOf(right)}));
            high = Require(builder.MulHigh(Type::U64,
                                           {Operand::ValueOf(left),
                                            Operand::ValueOf(right),
                                            Operand::Immediate(0, Type::U8)}));
        } else {
            const auto a =
                    Require(builder.ZeroExtend64(Type::U64, {Operand::ValueOf(left, Type::U32)}));
            const auto b =
                    Require(builder.ZeroExtend64(Type::U64, {Operand::ValueOf(right, Type::U32)}));
            const auto product =
                    Require(builder.Mul(Type::U64, {Operand::ValueOf(a), Operand::ValueOf(b)}));
            low = Require(builder.ZeroExtend32(Type::U32, {Operand::ValueOf(product, Type::U32)}));
            high = Extract(builder, product, 32, 32, Type::U32);
        }
        // With equal destinations the architecturally high half wins.
        WriteRegister(builder, layout, first, low);
        WriteRegister(builder, layout, destination, high);
        return true;
    }
    const auto left = ReadData(builder, layout, instruction, first, next_pc, target);
    Value right;
    if (!two && !rotate) right = ReadData(builder, layout, instruction, second, next_pc, target);
    Value result;
    if (adcx || adox) {
        const auto previous = ReadRegister(builder, layout, destination.index, bits);
        Value old_carry;
        if (adox) {
            old_carry = Require(builder.TestFlags(Type::Bool, {Operand::Flags(Flag::Carry)}));
            const auto overflow =
                    Require(builder.TestFlags(Type::Bool, {Operand::Flags(Flag::Overflow)}));
            Require(builder.SetCarry(Type::Void, {Operand::ValueOf(overflow)}));
        }
        result = Require(
                builder.Adc(type,
                            {Operand::ValueOf(previous), Operand::ValueOf(left)},
                            {FlagBits(Flag::Carry), FlagProfile::ArithmeticCanonicalCarry, false}));
        if (adox) {
            const auto overflow =
                    Require(builder.TestFlags(Type::Bool, {Operand::Flags(Flag::Carry)}));
            Require(builder.SetOverflow(Type::Void, {Operand::ValueOf(overflow)}));
            Require(builder.SetCarry(Type::Void, {Operand::ValueOf(old_carry)}));
        }
    } else if (invert) {
        result = Require(
                builder.AndNot(type,
                               {Operand::ValueOf(right), Operand::ValueOf(left)},
                               {FlagBits(Flag::Carry | Flag::Overflow | Flag::Negate | Flag::Zero),
                                FlagProfile::LogicalZeroCVA,
                                false}));
    } else if (bls) {
        const auto decremented =
                Require(builder.Sub(type, {Operand::ValueOf(left), Operand::Immediate(1, type)}));
        if (instruction.opcode == Op::BLSMSK)
            result = Require(
                    builder.Xor(type, {Operand::ValueOf(decremented), Operand::ValueOf(left)}));
        else {
            const auto source = instruction.opcode == Op::BLSI
                                        ? Require(builder.Neg(type, {Operand::ValueOf(left)}))
                                        : decremented;
            result = Require(builder.And(type, {Operand::ValueOf(left), Operand::ValueOf(source)}));
        }
        Require(builder.Or(type,
                           {Operand::ValueOf(result), Operand::Immediate(0, type)},
                           {FlagBits(Flag::Negate | Flag::Zero | Flag::Overflow),
                            FlagProfile::LogicalZeroCVA,
                            false}));
        const auto carry =
                instruction.opcode == Op::BLSI
                        ? Require(builder.TestNotZero(Type::Bool, {Operand::ValueOf(left)}))
                        : Require(builder.TestZero(Type::Bool, {Operand::ValueOf(left)}));
        Require(builder.SetCarry(Type::Void, {Operand::ValueOf(carry)}));
    } else if (instruction.opcode == Op::BZHI || instruction.opcode == Op::BEXTR) {
        const auto start = Extract(builder, right, 0, 8);
        const auto too_large = [&](Value value) {
            return Require(builder.TestNotZero(
                    Type::Bool,
                    {Operand::ValueOf(Require(
                            builder.And(Type::U64,
                                        {Operand::ValueOf(value),
                                         Operand::Immediate(255u & ~(bits - 1u), Type::U64)})))}));
        };
        const auto past_start = too_large(start);
        const auto length = instruction.opcode == Op::BEXTR ? Extract(builder, right, 8, 8) : start;
        const auto past_length = too_large(length);
        const auto power = Require(builder.LslValue(
                type, {Operand::ValueOf(Constant(builder, 1, type)), Operand::ValueOf(length)}));
        auto mask =
                Require(builder.Sub(type, {Operand::ValueOf(power), Operand::Immediate(1, type)}));
        mask = Require(builder.Select(
                type,
                {Operand::ValueOf(past_length),
                 Operand::ValueOf(Constant(builder, bits == 64 ? UINT64_MAX : UINT32_MAX, type)),
                 Operand::ValueOf(mask)}));
        auto shifted = left;
        if (instruction.opcode == Op::BEXTR) {
            shifted = Require(
                    builder.LsrValue(type, {Operand::ValueOf(left), Operand::ValueOf(start)}));
            shifted = Require(builder.Select(type,
                                             {Operand::ValueOf(past_start),
                                              Operand::ValueOf(Constant(builder, 0, type)),
                                              Operand::ValueOf(shifted)}));
        }
        result =
                Require(builder.And(type,
                                    {Operand::ValueOf(shifted), Operand::ValueOf(mask)},
                                    {FlagBits(instruction.opcode == Op::BEXTR
                                                      ? Flag::Zero | Flag::Carry | Flag::Overflow
                                                      : Flag::Negate | Flag::Zero | Flag::Overflow),
                                     FlagProfile::LogicalZeroCVA,
                                     false}));
        if (instruction.opcode == Op::BZHI)
            Require(builder.SetCarry(Type::Void, {Operand::ValueOf(past_start)}));
    } else if (parallel) {
        result = instruction.opcode == Op::PDEP
                         ? Require(builder.ParallelBitDeposit(
                                   type, {Operand::ValueOf(left), Operand::ValueOf(right)}))
                         : Require(builder.ParallelBitExtract(
                                   type, {Operand::ValueOf(left), Operand::ValueOf(right)}));
    } else if (rotate) {
        result = Require(builder.RorImm(
                type,
                {Operand::ValueOf(left),
                 Operand::Immediate(static_cast<std::uint8_t>(instruction.immediate) & (bits - 1),
                                    Type::U8)}));
    } else if (instruction.opcode == Op::SHLX)
        result = Require(builder.LslValue(type, {Operand::ValueOf(left), Operand::ValueOf(right)}));
    else if (instruction.opcode == Op::SHRX)
        result = Require(builder.LsrValue(type, {Operand::ValueOf(left), Operand::ValueOf(right)}));
    else
        result = Require(builder.AsrValue(type, {Operand::ValueOf(left), Operand::ValueOf(right)}));
    WriteRegister(builder, layout, destination, result);
    return true;
}
