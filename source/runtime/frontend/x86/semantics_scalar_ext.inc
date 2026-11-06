// Scalar ISA extensions, using the same numerical/IR sink contract.
static Value WholeFlags(Emitter& builder) {
    const auto anchor = Constant(builder, 0);
    return Require(builder.GetFlags(Type::U64, {Operand::ValueOf(anchor), Operand::Flags(Flag::All)}));
}
static Value Extract(Emitter& builder, Value value, unsigned first, unsigned count, Type type = Type::U64) {
    return Require(builder.BitExtract(type, {Operand::ValueOf(value), Operand::Immediate(first, Type::U8),
                                           Operand::Immediate(count, Type::U8)}));
}
static Value Insert(Emitter& builder, Value parent, Value value, unsigned first, unsigned count) {
    return Require(builder.BitInsert(Type::U64, {Operand::ValueOf(parent), Operand::ValueOf(value),
            Operand::Immediate(first, Type::U8), Operand::Immediate(count, Type::U8)}));
}
static Value RflagsValue(Emitter& builder, bool low_byte = false) {
    auto result = Constant(builder, low_byte ? 2 : 0x202);
    const auto put = [&](Flag flag, unsigned bit) {
        const auto value = Require(builder.TestFlags(Type::Bool, {Operand::Flags(flag)}));
        result = Insert(builder, result, value, bit, 1);
    };
    put(Flag::Carry, 0); put(Flag::Parity, 2); put(Flag::AuxiliaryCarry, 4);
    put(Flag::Zero, 6); put(Flag::Negate, 7);
    if (!low_byte) {
        put(Flag::Overflow, 11);
        result = Insert(builder, result, Extract(builder, WholeFlags(builder), 10, 1), 10, 1);
    }
    return result;
}
static void ImportRflags(Emitter& builder, Value rflags, bool low_byte = false) {
    auto carrier = WholeFlags(builder);
    const auto put = [&](unsigned source, u64 portable) {
        carrier = Insert(builder, carrier, Extract(builder, rflags, source, 1), std::countr_zero(portable), 1);
    };
    put(0, runtime::x86::flags::Carry); put(4, runtime::x86::flags::Auxiliary);
    put(6, runtime::x86::flags::Zero); put(7, runtime::x86::flags::Sign);
    // The portable low byte carries even parity, not a boolean PF bit.
    const auto parity = Require(builder.Xor(Type::U64,
            {Operand::ValueOf(Extract(builder, rflags, 2, 1)), Operand::Immediate(1, Type::U64)}));
    carrier = Insert(builder, carrier, parity, 0, 8);
    u64 mask = FlagBits(Flag::All) & ~FlagBits(Flag::Overflow);
    if (!low_byte) {
        put(11, runtime::x86::flags::Overflow);
        carrier = Insert(builder, carrier, Extract(builder, rflags, 10, 1), 10, 1);
        mask = FlagBits(Flag::All) | FlagBits(Flag::Direction);
    }
    Require(builder.RestoreFlags(Type::Void, {Operand::ValueOf(carrier), Operand::Flags(mask)}));
}
static void FlagInstruction(Emitter& builder, StateLayout layout, const InstructionData& instruction,
                            PendingFlags& pending, u64 pc, TargetInfo target) {
    OneOperand(instruction);
    if (instruction.ops[0].type != GuestOperandKind::None) Unsupported();
    pending.Flush(builder);
    switch (instruction.opcode) {
        case Op::CLC: Require(builder.SetCarry(Type::Void, {Operand::ValueOf(Constant(builder, 0))})); break;
        case Op::STC: Require(builder.SetCarry(Type::Void, {Operand::ValueOf(Constant(builder, 1))})); break;
        case Op::CMC: Require(builder.InvertCarry(Type::Void, {})); break;
        case Op::CLD: case Op::STD: {
            const auto flags = Insert(builder, WholeFlags(builder), Constant(builder, instruction.opcode == Op::STD), 10, 1);
            Require(builder.RestoreFlags(Type::Void, {Operand::ValueOf(flags), Operand::Flags(Flag::Direction)}));
            break;
        }
        case Op::LAHF:
            WriteRegister(builder, layout, DecodedOperand{GuestOperandKind::Register, Register::AH, 8}, RflagsValue(builder, true));
            break;
        case Op::SAHF:
            ImportRflags(builder, ReadRegister(builder, layout, Register::AH, 8), true); break;
        case Op::PUSHF: {
            PrepareFaultPC(builder, layout, pc, target);
            const auto bits = StackBits(instruction);
            const auto value = RflagsValue(builder);
            const auto stack = ReadRegister(builder, layout, Register::RSP, 64);
            const auto changed = Add(builder, stack, Operand::Immediate(u64{} - bits / 8, Type::U64));
            StoreMemory(builder, changed, value, bits);
            WriteStack(builder, layout, changed);
            break;
        }
        case Op::POPF: {
            PrepareFaultPC(builder, layout, pc, target);
            const auto bits = StackBits(instruction);
            const auto stack = ReadRegister(builder, layout, Register::RSP, 64);
            const auto value = Require(builder.LoadMemoryTSO(ScalarType(bits), {Operand::ValueOf(stack)}));
            const auto changed = Add(builder, stack, Operand::Immediate(bits / 8, Type::U64));
            ImportRflags(builder, value);
            WriteStack(builder, layout, changed);
            break;
        }
        default: Unsupported();
    }
}
static void SwapBytes(Emitter& builder, StateLayout layout, const InstructionData& instruction,
                       u64 pc, u64 next_pc, TargetInfo target) {
    const auto& destination = instruction.ops[0];
    if (instruction.opcode == Op::BSWAP) {
        OneOperand(instruction);
        if (destination.type != GuestOperandKind::Register || (destination.size != 32 && destination.size != 64)) Unsupported();
        const auto source = ReadRegister(builder, layout, destination.index, destination.size);
        const auto result = Require(builder.ByteSwap(ScalarType(destination.size),
                {Operand::ValueOf(source), Operand::Immediate(destination.size, Type::U8)}));
        WriteRegister(builder, layout, destination, result);
        return;
    }
    const auto& source = instruction.ops[1];
    if (instruction.ops[2].type != GuestOperandKind::None || instruction.ops[3].type != GuestOperandKind::None ||
        destination.size != source.size || (destination.size != 16 && destination.size != 32 && destination.size != 64) ||
        !((destination.type == GuestOperandKind::Register && Memory(source)) ||
          (Memory(destination) && source.type == GuestOperandKind::Register))) Unsupported();
    unsigned slot{};
    if (destination.type == GuestOperandKind::Register) (void)RegisterOffset(layout, destination.index, destination.size, slot);
    PrepareFaultPC(builder, layout, pc, target);
    const auto value = ReadData(builder, layout, instruction, source, next_pc, target);
    const auto result = Require(builder.ByteSwap(ScalarType(destination.size),
            {Operand::ValueOf(value), Operand::Immediate(destination.size, Type::U8)}));
    if (destination.type == GuestOperandKind::Register) WriteRegister(builder, layout, destination, result);
    else StoreMemory(builder, Address(builder, layout, instruction, destination, next_pc, target), result, destination.size);
}
static void LeadingCount(Emitter& builder, StateLayout layout, const InstructionData& instruction,
                          PendingFlags& pending, u64 pc, u64 next_pc, TargetInfo target) {
    const auto& destination = instruction.ops[0]; const auto& source = instruction.ops[1];
    const auto bits = destination.size;
    if (destination.type != GuestOperandKind::Register || (bits != 16 && bits != 32 && bits != 64) || source.size != bits ||
        (source.type != GuestOperandKind::Register && !Memory(source)) || instruction.ops[2].type != GuestOperandKind::None ||
        instruction.ops[3].type != GuestOperandKind::None) Unsupported();
    unsigned slot{}; (void)RegisterOffset(layout, destination.index, bits, slot);
    pending.Flush(builder);
    if (Memory(source)) PrepareFaultPC(builder, layout, pc, target);
    const auto value = ReadData(builder, layout, instruction, source, next_pc, target);
    const auto input = bits == 16 ? Require(builder.ZeroExtend32(Type::U32, {Operand::ValueOf(value)})) : value;
    auto result = bits == 64 ? Require(builder.CountLeadingZeros64(Type::U64, {Operand::ValueOf(input)}))
                             : Require(builder.CountLeadingZeros32(Type::U32, {Operand::ValueOf(input)}));
    if (bits == 16) result = Require(builder.Sub(Type::U32, {Operand::ValueOf(result), Operand::Immediate(16, Type::U32)}));
    WriteRegister(builder, layout, destination, result);
    Require(builder.SetCarry(Type::Void, {Operand::ValueOf(Require(builder.TestZero(Type::Bool, {Operand::ValueOf(value)})))}));
    const auto flags = Require(builder.Sub(ScalarType(bits), {Operand::ValueOf(result, ScalarType(bits)), Operand::Immediate(0, ScalarType(bits))}));
    pending = {builder.ProducerOf(flags), FlagProfile::ArithmeticCanonicalCarry, FlagBits(Flag::Zero), {}};
}
static bool CounterConditional(Op opcode) {
    return opcode == Op::LOOP || opcode == Op::LOOPZ || opcode == Op::LOOPNZ ||
           opcode == Op::JRCXZ || opcode == Op::JECXZ || opcode == Op::JCXZ;
}
template<class Flow>
static void CounterBranch(Emitter& builder, StateLayout layout, const InstructionData& instruction,
                          PendingFlags& pending, u64 next_pc, Flow& flow, bool& ended) {
    if (!Direct(instruction) || (instruction.address_bits != 32 && instruction.address_bits != 64)) Unsupported();
    pending.Flush(builder);
    const unsigned bits = instruction.address_bits;
    auto count = ReadRegister(builder, layout, Gpr(1, bits).index, bits);
    const bool loop = instruction.opcode == Op::LOOP || instruction.opcode == Op::LOOPZ || instruction.opcode == Op::LOOPNZ;
    if (loop) {
        count = Require(builder.Sub(ScalarType(bits), {Operand::ValueOf(count), Operand::Immediate(1, ScalarType(bits))}));
        WriteRegister(builder, layout, Gpr(1, bits), count);
    }
    auto condition = loop ? Require(builder.TestNotZero(Type::Bool, {Operand::ValueOf(count)}))
                          : Require(builder.TestZero(Type::Bool, {Operand::ValueOf(count)}));
    if (instruction.opcode == Op::LOOPZ || instruction.opcode == Op::LOOPNZ) {
        const auto zf = instruction.opcode == Op::LOOPZ
                ? Require(builder.TestFlags(Type::Bool, {Operand::Flags(Flag::Zero)}))
                : Require(builder.TestNotFlags(Type::Bool, {Operand::Flags(Flag::Zero)}));
        condition = Require(builder.And(Type::U64, {Operand::ValueOf(condition), Operand::ValueOf(zf)}));
    }
    builder.PublishRegisters();
    flow.branch(condition, next_pc + instruction.immediate, next_pc);
    ended = true;
}
static Value StringAddress(Emitter& builder, StateLayout layout, Value index, unsigned address_bits, bool source,
                           const InstructionData& instruction) {
    auto address = address_bits == 32 ? Require(builder.ZeroExtend64(Type::U64, {Operand::ValueOf(index, Type::U32)})) : index;
    if (!source || (instruction.segment != Register::FS && instruction.segment != Register::GS)) return address;
    const auto offset = instruction.segment == Register::FS ? layout.fs_base_offset : layout.gs_base_offset;
    if (offset > UINT32_MAX - 7) Unsupported();
    const auto base = Require(builder.LoadUniform(Type::U64, {Operand::Uniform(offset, Type::U64)}));
    return Add(builder, address, Operand::ValueOf(base));
}
static void StringInstruction(Emitter& builder, StateLayout layout, const InstructionData& instruction,
                              u64 pc, u64 next_pc, TargetInfo target) {
    const auto bits = instruction.ops[0].size;
    const auto type = ScalarType(bits);
    const unsigned address_bits = instruction.address_bits;
    if ((address_bits != 32 && address_bits != 64) || instruction.ops[2].type != GuestOperandKind::None ||
        instruction.ops[3].type != GuestOperandKind::None) Unsupported();
    const bool move = instruction.opcode == Op::MOVS, load = instruction.opcode == Op::LODS;
    const bool compare = instruction.opcode == Op::CMPS, scan = instruction.opcode == Op::SCAS;
    const bool store = instruction.opcode == Op::STOS;
    if (!move && !load && !compare && !scan && !store) Unsupported();
    const auto memory = [&](const DecodedOperand& operand, unsigned slot) {
        return operand.type == GuestOperandKind::Memory && operand.size == bits &&
               operand.index == Gpr(slot, address_bits).index;
    };
    const auto accumulator = [&](const DecodedOperand& operand) {
        return operand.type == GuestOperandKind::Register && operand.size == bits && operand.index == Gpr(0, bits).index;
    };
    const auto& first = instruction.ops[0]; const auto& second = instruction.ops[1];
    const bool pair = move || compare
            ? ((memory(first, 6) && memory(second, 7)) || (memory(first, 7) && memory(second, 6)))
            : ((accumulator(first) && memory(second, load ? 6 : 7)) ||
               (accumulator(second) && memory(first, load ? 6 : 7)));
    if (!pair) Unsupported();
    const bool repeated = (instruction.flags & (InstructionFlag::Repeat | InstructionFlag::RepeatNotEqual)) != 0;
    Value count;
    if (repeated) {
        count = ReadRegister(builder, layout, Gpr(1, address_bits).index, address_bits);
        StorePC(builder, layout, Constant(builder, next_pc));
    }
    const auto body = [&] {
        PrepareFaultPC(builder, layout, pc, target);
        Value si, di, left, right;
        if (move || load || compare) si = ReadRegister(builder, layout, Gpr(6, address_bits).index, address_bits);
        if (move || store || compare || scan) di = ReadRegister(builder, layout, Gpr(7, address_bits).index, address_bits);
        if (store || scan) left = ReadRegister(builder, layout, Gpr(0, bits).index, bits);
        const auto df = Extract(builder, WholeFlags(builder), 10, 1);
        const auto backwards = address_bits == 32 ? u64{u32{} - bits / 8} : u64{} - bits / 8;
        const auto step = Require(builder.Select(ScalarType(address_bits), {Operand::ValueOf(df),
                Operand::ValueOf(Constant(builder, backwards, ScalarType(address_bits))),
                Operand::ValueOf(Constant(builder, bits / 8, ScalarType(address_bits)))}));
        if (move || load || compare) {
            const auto source = StringAddress(builder, layout, si, address_bits, true, instruction);
            right = Require(builder.LoadMemoryTSO(type, {Operand::ValueOf(source)}));
        }
        if (move || store) {
            const auto destination = StringAddress(builder, layout, di, address_bits, false, instruction);
            StoreMemory(builder, destination, move ? right : left, bits);
        } else if (compare || scan) {
            const auto destination = StringAddress(builder, layout, di, address_bits, false, instruction);
            const auto value = Require(builder.LoadMemoryTSO(type, {Operand::ValueOf(destination)}));
            const auto result = Require(builder.Sub(type, {Operand::ValueOf(compare ? right : left), Operand::ValueOf(value)},
                    {FlagBits(Flag::All), FlagProfile::ArithmeticCanonicalCarry, false}));
            (void)result;
        }
        // No architectural progress is published until every access belonging
        // to this element succeeds. A subsequent retry resumes this element.
        if (load) WriteRegister(builder, layout, Gpr(0, bits), right);
        if (si) WriteRegister(builder, layout, Gpr(6, address_bits),
                Require(builder.Add(ScalarType(address_bits), {Operand::ValueOf(si), Operand::ValueOf(step)})));
        if (di) WriteRegister(builder, layout, Gpr(7, address_bits),
                Require(builder.Add(ScalarType(address_bits), {Operand::ValueOf(di), Operand::ValueOf(step)})));
        Value continuation;
        if (repeated) {
            const auto remaining = Require(builder.Sub(ScalarType(address_bits),
                    {Operand::ValueOf(count), Operand::Immediate(1, ScalarType(address_bits))}));
            WriteRegister(builder, layout, Gpr(1, address_bits), remaining);
            continuation = remaining;
            if (compare || scan) {
                const auto nonzero = Require(builder.TestNotZero(Type::Bool, {Operand::ValueOf(remaining)}));
                const auto equal = instruction.flags & InstructionFlag::RepeatNotEqual
                        ? Require(builder.TestNotFlags(Type::Bool, {Operand::Flags(Flag::Zero)}))
                        : Require(builder.TestFlags(Type::Bool, {Operand::Flags(Flag::Zero)}));
                continuation = Require(builder.And(Type::U64, {Operand::ValueOf(nonzero), Operand::ValueOf(equal)}));
            }
        }
        const auto destination = repeated ? Require(builder.Select(Type::U64, {Operand::ValueOf(continuation),
                Operand::ValueOf(Constant(builder, pc)), Operand::ValueOf(Constant(builder, next_pc))})) : Constant(builder, next_pc);
        StorePC(builder, layout, destination);
    };
    if (repeated) builder.IfNonZero(count, body); else body();
    EndDispatch(builder);
}
static void BitString(Emitter& builder, StateLayout layout, const InstructionData& instruction,
                       PendingFlags& pending, u64 pc, u64 next_pc, TargetInfo target) {
    const auto& destination = instruction.ops[0]; const auto& amount = instruction.ops[1];
    const auto bits = destination.size; const auto type = ScalarType(bits);
    if ((bits != 16 && bits != 32 && bits != 64) ||
        (destination.type != GuestOperandKind::Register && !Memory(destination)) ||
        (amount.type != GuestOperandKind::Immediate && !(amount.type == GuestOperandKind::Register && amount.size == bits)) ||
        instruction.ops[2].type != GuestOperandKind::None || instruction.ops[3].type != GuestOperandKind::None) Unsupported();
    const bool change = instruction.opcode != Op::BT;
    const bool locked = instruction.flags & InstructionFlag::Locked;
    if (locked && (!change || !Memory(destination))) Unsupported();
    pending.Flush(builder);
    const auto index = amount.type == GuestOperandKind::Immediate
            ? Constant(builder, static_cast<std::uint8_t>(instruction.immediate), Type::U64)
            : ReadRegister(builder, layout, amount.index, bits);
    const auto bit = Require(builder.And(Type::U64, {Operand::ValueOf(index), Operand::Immediate(bits - 1, Type::U64)}));
    Value address;
    if (Memory(destination)) {
        PrepareFaultPC(builder, layout, pc, target);
        Value offset;
        if (amount.type == GuestOperandKind::Register) {
            const auto signed_index = Require(builder.SignExtend(Type::U64, {Operand::ValueOf(index, type)}));
            const auto words = Require(builder.AsrImm(Type::U64, {Operand::ValueOf(signed_index),
                    Operand::Immediate(bits == 16 ? 4 : bits == 32 ? 5 : 6, Type::U8)}));
            offset = Require(builder.LslImm(Type::U64, {Operand::ValueOf(words),
                    Operand::Immediate(bits == 16 ? 1 : bits == 32 ? 2 : 3, Type::U8)}));
        }
        address = Address(builder, layout, instruction, destination, next_pc, target, {}, nullptr, Type::U64, offset);
    }
    const auto one = Constant(builder, 1, Type::U64);
    auto mask = Require(builder.LslValue(Type::U64, {Operand::ValueOf(one), Operand::ValueOf(bit)}));
    if (instruction.opcode == Op::BTR)
        mask = Require(builder.Xor(Type::U64, {Operand::ValueOf(mask), Operand::Immediate(UINT64_MAX, Type::U64)}));
    Value old;
    if (locked) {
        const auto kind = instruction.opcode == Op::BTS ? AtomicRMWOp::Or :
                          instruction.opcode == Op::BTR ? AtomicRMWOp::And : AtomicRMWOp::Xor;
        old = Require(builder.AtomicRMW(type, {Operand::Immediate(static_cast<u64>(kind), Type::U8),
                Operand::ValueOf(address), Operand::ValueOf(mask, type), Operand::ValueOf(Constant(builder, 0, Type::U8))}));
    } else {
        old = address ? Require(builder.LoadMemoryTSO(type, {Operand::ValueOf(address)}))
                      : ReadRegister(builder, layout, destination.index, bits);
        if (change) {
            const auto result = instruction.opcode == Op::BTS
                    ? Require(builder.Or(type, {Operand::ValueOf(old), Operand::ValueOf(mask, type)}))
                    : instruction.opcode == Op::BTR
                            ? Require(builder.And(type, {Operand::ValueOf(old), Operand::ValueOf(mask, type)}))
                            : Require(builder.Xor(type, {Operand::ValueOf(old), Operand::ValueOf(mask, type)}));
            if (address) StoreMemory(builder, address, result, bits);
            else WriteRegister(builder, layout, destination, result);
        }
    }
    const auto shifted = Require(builder.LsrValue(Type::U64, {Operand::ValueOf(old), Operand::ValueOf(bit)}));
    const auto carry = Extract(builder, shifted, 0, 1);
    Require(builder.SetCarry(Type::Void, {Operand::ValueOf(carry)}));
}
static void DoubleShift(Emitter& builder, StateLayout layout, const InstructionData& instruction,
                         PendingFlags& pending, u64 pc, u64 next_pc, TargetInfo target) {
    const auto& destination = instruction.ops[0]; const auto& source = instruction.ops[1]; const auto& amount = instruction.ops[2];
    const auto bits = destination.size; const auto type = ScalarType(bits);
    if ((bits != 16 && bits != 32 && bits != 64) ||
        (destination.type != GuestOperandKind::Register && !Memory(destination)) || source.type != GuestOperandKind::Register || source.size != bits ||
        (amount.type != GuestOperandKind::Immediate && !(amount.type == GuestOperandKind::Register && amount.index == Register::CL && amount.size == 8)) ||
        instruction.ops[3].type != GuestOperandKind::None) Unsupported();
    pending.Flush(builder);
    const auto mask = bits == 64 ? 63u : 31u;
    auto count = amount.type == GuestOperandKind::Immediate ? Constant(builder, static_cast<std::uint8_t>(instruction.immediate) & mask)
            : Require(builder.And(Type::U64, {Operand::ValueOf(ReadRegister(builder, layout, Register::CL, 8)), Operand::Immediate(mask, Type::U64)}));
    const auto other = ReadRegister(builder, layout, source.index, bits);
    Value address;
    if (Memory(destination)) { PrepareFaultPC(builder, layout, pc, target); address = Address(builder, layout, instruction, destination, next_pc, target); }
    const auto old = address ? Require(builder.LoadMemoryTSO(type, {Operand::ValueOf(address)}))
                            : ReadRegister(builder, layout, destination.index, bits);
    const auto inverse = Require(builder.Sub(Type::U64, {Operand::ValueOf(Constant(builder, bits)), Operand::ValueOf(count)}));
    const auto left = Require(builder.ZeroExtend64(Type::U64, {Operand::ValueOf(old)}));
    const auto right = Require(builder.ZeroExtend64(Type::U64, {Operand::ValueOf(other)}));
    const bool shld = instruction.opcode == Op::SHLD;
    const auto first = shld ? Require(builder.LslValue(Type::U64, {Operand::ValueOf(left), Operand::ValueOf(count)}))
                            : Require(builder.LsrValue(Type::U64, {Operand::ValueOf(left), Operand::ValueOf(count)}));
    const auto second = shld ? Require(builder.LsrValue(Type::U64, {Operand::ValueOf(right), Operand::ValueOf(inverse)}))
                             : Require(builder.LslValue(Type::U64, {Operand::ValueOf(right), Operand::ValueOf(inverse)}));
    const auto combined = Require(builder.Or(Type::U64, {Operand::ValueOf(first), Operand::ValueOf(second)}));
    const auto zero = Require(builder.TestZero(Type::Bool, {Operand::ValueOf(count)}));
    const auto selected = Require(builder.Select(Type::U64, {Operand::ValueOf(zero), Operand::ValueOf(left), Operand::ValueOf(combined)}));
    const auto result = Extract(builder, selected, 0, bits, type);
    if (address) StoreMemory(builder, address, result, bits); else WriteRegister(builder, layout, destination, result);
    builder.IfNonZero(count, [&] {
        const auto carry_shift = shld ? inverse : Require(builder.Sub(Type::U64, {Operand::ValueOf(count), Operand::Immediate(1, Type::U64)}));
        const auto shifted = Require(builder.LsrValue(Type::U64, {Operand::ValueOf(left), Operand::ValueOf(carry_shift)}));
        const auto carry = Extract(builder, shifted, 0, 1);
        // NZP reflect the architectural width; AF and multi-count OF are undefined
        // and retain their previous values. A zero count executes no flag writer.
        Require(builder.Sub(type, {Operand::ValueOf(result), Operand::Immediate(0, type)},
                {FlagBits(Flag::Negate | Flag::Zero | Flag::Parity), FlagProfile::ArithmeticCanonicalCarry, false}));
        const auto overflow = shld ? Require(builder.Xor(Type::U64, {Operand::ValueOf(Extract(builder, result, bits - 1, 1)), Operand::ValueOf(carry)}))
                                  : Require(builder.Xor(Type::U64, {Operand::ValueOf(Extract(builder, old, bits - 1, 1)), Operand::ValueOf(Extract(builder, result, bits - 1, 1))}));
        const auto single = Require(builder.TestZero(Type::Bool, {Operand::ValueOf(Require(builder.Sub(Type::U64, {Operand::ValueOf(count), Operand::Immediate(1, Type::U64)})))}));
        const auto previous = Require(builder.TestFlags(Type::Bool, {Operand::Flags(Flag::Overflow)}));
        const auto of = Require(builder.Select(Type::U64, {Operand::ValueOf(single), Operand::ValueOf(overflow), Operand::ValueOf(previous)}));
        Require(builder.SetCarry(Type::Void, {Operand::ValueOf(carry)}));
        Require(builder.SetOverflow(Type::Void, {Operand::ValueOf(of)}));
    });
}
static void CarryRotate(Emitter& builder, StateLayout layout, const InstructionData& instruction,
                         PendingFlags& pending, u64 pc, u64 next_pc, TargetInfo target) {
    const auto& destination = instruction.ops[0]; const auto& amount = instruction.ops[1];
    const auto bits = destination.size; const auto type = ScalarType(bits);
    if ((destination.type != GuestOperandKind::Register && !Memory(destination)) ||
        (amount.type != GuestOperandKind::Immediate && !(amount.type == GuestOperandKind::Register && amount.index == Register::CL && amount.size == 8)) ||
        instruction.ops[2].type != GuestOperandKind::None || instruction.ops[3].type != GuestOperandKind::None) Unsupported();
    pending.Flush(builder);
    auto count = amount.type == GuestOperandKind::Immediate ? Constant(builder, static_cast<std::uint8_t>(instruction.immediate) & (bits == 64 ? 63 : 31))
            : Require(builder.And(Type::U64, {Operand::ValueOf(ReadRegister(builder, layout, Register::CL, 8)), Operand::Immediate(bits == 64 ? 63 : 31, Type::U64)}));
    // Only 9/17-bit rotates need a modulo beyond the architectural count mask.
    const auto reduce = [&](unsigned modulus) {
        const auto difference = Require(builder.Sub(Type::U64, {Operand::ValueOf(count), Operand::Immediate(modulus, Type::U64)}));
        const auto below = Extract(builder, difference, 63, 1);
        count = Require(builder.Select(Type::U64, {Operand::ValueOf(below), Operand::ValueOf(count), Operand::ValueOf(difference)}));
    };
    if (bits == 8) { reduce(18); reduce(9); } else if (bits == 16) reduce(17);
    const auto carry_in = Require(builder.TestFlags(Type::Bool, {Operand::Flags(Flag::Carry)}));
    Value address;
    if (Memory(destination)) { PrepareFaultPC(builder, layout, pc, target); address = Address(builder, layout, instruction, destination, next_pc, target); }
    const auto old = address ? Require(builder.LoadMemoryTSO(type, {Operand::ValueOf(address)}))
                            : ReadRegister(builder, layout, destination.index, bits);
    const auto value = Require(builder.ZeroExtend64(Type::U64, {Operand::ValueOf(old)}));
    const bool left = instruction.opcode == Op::RCL;
    Value result, carry;
    if (bits < 64) {
        const auto packed = Insert(builder, value, carry_in, bits, 1);
        const auto inverse = Require(builder.Sub(Type::U64, {Operand::ValueOf(Constant(builder, bits + 1)), Operand::ValueOf(count)}));
        const auto first = left ? Require(builder.LslValue(Type::U64, {Operand::ValueOf(packed), Operand::ValueOf(count)}))
                                : Require(builder.LsrValue(Type::U64, {Operand::ValueOf(packed), Operand::ValueOf(count)}));
        const auto second = left ? Require(builder.LsrValue(Type::U64, {Operand::ValueOf(packed), Operand::ValueOf(inverse)}))
                                 : Require(builder.LslValue(Type::U64, {Operand::ValueOf(packed), Operand::ValueOf(inverse)}));
        const auto rotated = Require(builder.Or(Type::U64, {Operand::ValueOf(first), Operand::ValueOf(second)}));
        result = Extract(builder, rotated, 0, bits, type); carry = Extract(builder, rotated, bits, 1);
    } else {
        const auto previous = Require(builder.Sub(Type::U64, {Operand::ValueOf(count), Operand::Immediate(1, Type::U64)}));
        const auto inverse = Require(builder.Sub(Type::U64, {Operand::ValueOf(Constant(builder, 64)), Operand::ValueOf(count)}));
        const auto wrapped_count = Require(builder.Add(Type::U64, {Operand::ValueOf(inverse), Operand::Immediate(1, Type::U64)}));
        const auto single = Require(builder.TestZero(Type::Bool, {Operand::ValueOf(previous)}));
        const auto main = left ? Require(builder.LslValue(Type::U64, {Operand::ValueOf(value), Operand::ValueOf(count)}))
                               : Require(builder.LsrValue(Type::U64, {Operand::ValueOf(value), Operand::ValueOf(count)}));
        const auto carry_term = left ? Require(builder.LslValue(Type::U64, {Operand::ValueOf(carry_in, Type::U64), Operand::ValueOf(previous)}))
                                     : Require(builder.LslValue(Type::U64, {Operand::ValueOf(carry_in, Type::U64), Operand::ValueOf(inverse)}));
        const auto wrapped = left ? Require(builder.LsrValue(Type::U64, {Operand::ValueOf(value), Operand::ValueOf(wrapped_count)}))
                                  : Require(builder.LslValue(Type::U64, {Operand::ValueOf(value), Operand::ValueOf(wrapped_count)}));
        const auto high = Require(builder.Select(Type::U64, {Operand::ValueOf(single), Operand::ValueOf(Constant(builder, 0)), Operand::ValueOf(wrapped)}));
        const auto joined = Require(builder.Or(Type::U64, {Operand::ValueOf(main), Operand::ValueOf(carry_term)}));
        const auto all = Require(builder.Or(Type::U64, {Operand::ValueOf(joined), Operand::ValueOf(high)}));
        const auto zero = Require(builder.TestZero(Type::Bool, {Operand::ValueOf(count)}));
        result = Require(builder.Select(Type::U64, {Operand::ValueOf(zero), Operand::ValueOf(value), Operand::ValueOf(all)}));
        const auto carried = left ? Require(builder.LsrValue(Type::U64, {Operand::ValueOf(value), Operand::ValueOf(inverse)}))
                                  : Require(builder.LsrValue(Type::U64, {Operand::ValueOf(value), Operand::ValueOf(previous)}));
        carry = Extract(builder, carried, 0, 1);
    }
    if (address) StoreMemory(builder, address, result, bits); else WriteRegister(builder, layout, destination, result);
    builder.IfNonZero(count, [&] {
        const auto overflow = Require(builder.Xor(Type::U64, {Operand::ValueOf(Extract(builder, result, bits - 1, 1)),
                Operand::ValueOf(left ? carry : Extract(builder, result, bits - 2, 1))}));
        const auto single = Require(builder.TestZero(Type::Bool, {Operand::ValueOf(Require(builder.Sub(Type::U64, {Operand::ValueOf(count), Operand::Immediate(1, Type::U64)})))}));
        const auto previous = Require(builder.TestFlags(Type::Bool, {Operand::Flags(Flag::Overflow)}));
        const auto of = Require(builder.Select(Type::U64, {Operand::ValueOf(single), Operand::ValueOf(overflow), Operand::ValueOf(previous)}));
        Require(builder.SetCarry(Type::Void, {Operand::ValueOf(carry)}));
        Require(builder.SetOverflow(Type::Void, {Operand::ValueOf(of)}));
    });
}
static void EnterFrame(Emitter& builder, StateLayout layout, const InstructionData& instruction,
                        PendingFlags& pending, u64 pc, TargetInfo target) {
    if (instruction.ops[0].type != GuestOperandKind::ImmediateSecond || instruction.ops[1].type != GuestOperandKind::ImmediateThird ||
        instruction.ops[2].type != GuestOperandKind::None || instruction.ops[3].type != GuestOperandKind::None) Unsupported();
    const auto bits = StackBits(instruction); const auto type = ScalarType(bits);
    const unsigned nesting = static_cast<unsigned>(instruction.immediate >> 32) & 31;
    const auto allocation = instruction.immediate & 0xffff;
    pending.Flush(builder); PrepareFaultPC(builder, layout, pc, target);
    const auto stack = ReadRegister(builder, layout, Register::RSP, 64);
    const auto base = ReadRegister(builder, layout, bits == 64 ? Register::RBP : Register::BP, bits);
    auto changed = Add(builder, stack, Operand::Immediate(u64{} - bits / 8, Type::U64));
    StoreMemory(builder, changed, base, bits);
    const auto frame = changed;
    if (nesting) {
        auto cursor = ReadRegister(builder, layout, Register::RBP, 64);
        for (unsigned level = 1; level < nesting; ++level) {
            cursor = Add(builder, cursor, Operand::Immediate(u64{} - bits / 8, Type::U64));
            const auto value = Require(builder.LoadMemoryTSO(type, {Operand::ValueOf(cursor)}));
            changed = Add(builder, changed, Operand::Immediate(u64{} - bits / 8, Type::U64));
            StoreMemory(builder, changed, value, bits);
        }
        changed = Add(builder, changed, Operand::Immediate(u64{} - bits / 8, Type::U64));
        StoreMemory(builder, changed, frame, bits);
    }
    const auto final_stack = Add(builder, changed, Operand::Immediate(u64{} - allocation, Type::U64));
    // Commit both progress registers only after the instruction's memory work.
    WriteRegister(builder, layout, Gpr(5, 64), frame);
    WriteStack(builder, layout, final_stack);
}

