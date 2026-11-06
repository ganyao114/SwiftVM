// Members of the single Semantics<Sink> template. No instruction stream or owner.

static void Move(Emitter& builder,
                 StateLayout layout,
                 const InstructionData& instruction,
                 u64 pc,
                 u64 next_pc,
                 TargetInfo target) {
    const auto& destination = instruction.ops[0];
    const auto& source = instruction.ops[1];
    const auto bits = destination.size;
    const auto type = ScalarType(bits);
    if (instruction.ops[2].type != GuestOperandKind::None ||
        instruction.ops[3].type != GuestOperandKind::None ||
        (destination.type != GuestOperandKind::Register && !Memory(destination)) ||
        (source.type != GuestOperandKind::Register && source.type != GuestOperandKind::Immediate &&
         !Memory(source)) ||
        (Memory(destination) && Memory(source)))
        Unsupported();
    if (source.type != GuestOperandKind::Immediate && source.size != bits) Unsupported();
    // A legal MOV to a segment/control register is outside this subset. Reject
    // that form before reading its potentially faulting memory operand.
    unsigned destination_slot{};
    const auto destination_offset =
            destination.type == GuestOperandKind::Register
                    ? RegisterOffset(layout, destination.index, bits, destination_slot)
                    : 0;
    if (Memory(destination) || Memory(source)) PrepareFaultPC(builder, layout, pc, target);
    Value value;
    if (source.type == GuestOperandKind::Immediate) {
        value = Constant(builder, ImmediateBits(instruction, bits), type);
    } else
        value = ReadData(builder, layout, instruction, source, next_pc, target);
    if (destination.type == GuestOperandKind::Register)
        builder.WriteGpr(destination_offset, destination_slot, bits, value);
    else {
        u64 displacement{};
        const auto address = Address(
                builder, layout, instruction, destination, next_pc, target, {}, &displacement);
        StoreMemory(builder, address, value, bits, displacement);
    }
}

static void ExtendedMove(Emitter& builder,
                         StateLayout layout,
                         const InstructionData& instruction,
                         u64 pc,
                         u64 next_pc,
                         TargetInfo target) {
    const auto& destination = instruction.ops[0];
    const auto& source = instruction.ops[1];
    if (destination.type != GuestOperandKind::Register ||
        (source.type != GuestOperandKind::Register && !Memory(source)) ||
        instruction.ops[2].type != GuestOperandKind::None ||
        instruction.ops[3].type != GuestOperandKind::None ||
        (destination.size != 16 && destination.size != 32 && destination.size != 64))
        Unsupported();
    if (instruction.opcode == Op::MOVSXD) {
        if (source.size != 32 || destination.size < 32) Unsupported();
    } else if ((source.size != 8 && source.size != 16) || source.size >= destination.size)
        Unsupported();
    if (Memory(source)) PrepareFaultPC(builder, layout, pc, target);
    const auto source_value = ReadData(builder, layout, instruction, source, next_pc, target);
    // ReadData births a numeric load, whose carrier is already zero above the
    // source width. The wider store view needs no extra MOVZX value node.
    const auto value =
            instruction.opcode == Op::MOVZX
                    ? source_value
                    : Require(builder.SignExtend(
                              ScalarType(destination.size),
                              {Operand::ValueOf(source_value, ScalarType(source.size))}));
    WriteRegister(builder, layout, destination, value);
}
static void Complement(Emitter& builder,
                       StateLayout layout,
                       const InstructionData& instruction,
                       u64 pc,
                       u64 next_pc,
                       TargetInfo target) {
    OneOperand(instruction);
    const auto& destination = instruction.ops[0];
    if (destination.type != GuestOperandKind::Register && !Memory(destination)) Unsupported();
    const auto type = ScalarType(destination.size);
    Value address;
    if (Memory(destination)) {
        PrepareFaultPC(builder, layout, pc, target);
        address = Address(builder, layout, instruction, destination, next_pc, target);
    }
    if (instruction.flags & InstructionFlag::Locked) {
        if (!address) Unsupported();
        const auto ones = Constant(builder, UINT64_MAX >> (64 - destination.size), type),
                   carry = Constant(builder, 0, Type::U8);
        Require(builder.AtomicRMW(type,
                                  {Operand::Immediate(static_cast<u64>(AtomicRMWOp::Xor), Type::U8),
                                   Operand::ValueOf(address),
                                   Operand::ValueOf(ones),
                                   Operand::ValueOf(carry)}));
        return;  // NOT never writes flags.
    }
    const auto old = address ? Require(builder.LoadMemoryTSO(type, {Operand::ValueOf(address)}))
                             : ReadRegister(builder, layout, destination.index, destination.size);
    const auto result = Require(builder.Not(type, {Operand::ValueOf(old), Operand::ValueOf(old)}));
    if (address)
        StoreMemory(builder, address, result, destination.size);
    else
        WriteRegister(builder, layout, destination, result);
}
static void Exchange(Emitter& builder,
                     StateLayout layout,
                     const InstructionData& instruction,
                     u64 pc,
                     u64 next_pc,
                     TargetInfo target) {
    const auto& left = instruction.ops[0];
    const auto& right = instruction.ops[1];
    if (instruction.ops[2].type != GuestOperandKind::None ||
        instruction.ops[3].type != GuestOperandKind::None || left.size != right.size ||
        (left.type != GuestOperandKind::Register && !Memory(left)) ||
        (right.type != GuestOperandKind::Register && !Memory(right)) ||
        (Memory(left) && Memory(right)))
        Unsupported();
    const auto type = ScalarType(left.size);
    if (Memory(left) || Memory(right)) {
        const auto& memory = Memory(left) ? left : right;
        const auto& reg = Memory(left) ? right : left;
        PrepareFaultPC(builder, layout, pc, target);
        const auto address = Address(builder, layout, instruction, memory, next_pc, target);
        const auto value = ReadRegister(builder, layout, reg.index, reg.size);
        // XCHG with memory is implicitly locked, including without an F0 prefix.
        const auto old = Require(builder.AtomicExchange(
                type, {Operand::ValueOf(address), Operand::ValueOf(value, type)}));
        WriteRegister(builder, layout, reg, old);
    } else {
        const auto a = ReadRegister(builder, layout, left.index, left.size);
        const auto b = ReadRegister(builder, layout, right.index, right.size);
        WriteRegister(builder, layout, left, b);
        WriteRegister(builder, layout, right, a);
    }
}
static Value Widen(Emitter& builder, Value value, unsigned bits, bool sign) {
    if (bits == 64) return value;
    return sign ? Require(builder.SignExtend(Type::U64,
                                             {Operand::ValueOf(value, ScalarType(bits))}))
                : Require(builder.ZeroExtend64(Type::Void,
                                               {Operand::ValueOf(value, ScalarType(bits))}));
}
static void ExtendAccumulator(Emitter& builder,
                              StateLayout layout,
                              const InstructionData& instruction) {
    unsigned source_bits{}, destination_bits{};
    bool high{};
    switch (instruction.opcode) {
        case Op::CBW:
            source_bits = 8;
            destination_bits = 16;
            break;
        case Op::CWDE:
            source_bits = 16;
            destination_bits = 32;
            break;
        case Op::CDQE:
            source_bits = 32;
            destination_bits = 64;
            break;
        case Op::CWD:
            source_bits = destination_bits = 16;
            high = true;
            break;
        case Op::CDQ:
            source_bits = destination_bits = 32;
            high = true;
            break;
        case Op::CQO:
            source_bits = destination_bits = 64;
            high = true;
            break;
        default:
            Unsupported();
    }
    const auto source = ReadRegister(builder, layout, Gpr(0, source_bits).index, source_bits);
    const auto result = high ? Require(builder.AsrImm(
                                       Type::U64,
                                       {Operand::ValueOf(Widen(builder, source, source_bits, true)),
                                        Operand::Immediate(63, Type::U8)}))
                             : Require(builder.SignExtend(ScalarType(destination_bits),
                                                          {Operand::ValueOf(source)}));
    WriteRegister(builder, layout, Gpr(high ? 2 : 0, destination_bits), result);
}
struct PendingFlags {
    Producer producer;
    FlagProfile profile{FlagProfile::None};
    // Only the still-unobserved bits of this one real producer remain pending.
    // Published bits live in its actual metadata, not a second demand table.
    u64 outputs{FlagBits(Flag::All)};
    struct Recipe {
        Value left, right, result, high;
        unsigned bits{}, constant{};
        Op opcode{};
        bool wide_product{};
    } recipe;

    void PublishShift(Emitter& builder) {
        const auto& [original, count, result, high, bits, constant, opcode, wide_product] = recipe;
        const bool variable = bool(count);
        const bool rotate = opcode == Op::ROL || opcode == Op::ROR;
        const bool left = opcode == Op::SHL || opcode == Op::SAL || opcode == Op::ROL;
        const bool arithmetic = opcode == Op::SAR;
        Value old_flags;
        if (variable)
            old_flags = Require(builder.GetFlags(
                    Type::U64, {Operand::ValueOf(count), Operand::Flags(Flag::All)}));

        // All new flags are produced after a possible memory store has succeeded.
        // AF and undefined OF values are preserved; no packed backend layout leaks
        // into the frontend. Narrow shifts use x86's five-bit count, not modulo N.
        if (!rotate) {
            const auto mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
            Require(builder.And(ScalarType(bits),
                                {Operand::ValueOf(result, ScalarType(bits)),
                                 Operand::Immediate(mask, ScalarType(bits))},
                                {FlagBits(Flag::Negate | Flag::Zero | Flag::Parity),
                                 FlagProfile::LogicalZeroCVA,
                                 false}));
        }
        Value carry;
        if (rotate)
            carry = Bit(builder, result, left ? 0 : bits - 1);
        else if (!variable) {
            if (constant <= bits)
                carry = Bit(builder, original, left ? bits - constant : constant - 1);
            else
                carry = arithmetic ? Bit(builder, original, bits - 1) : Constant(builder, 0);
        } else {
            auto wide = Widen(builder, original, bits, arithmetic);
            auto index =
                    left ? Binary<OpCode::Sub>(
                                   builder, Constant(builder, bits), Operand::ValueOf(count))
                         : Binary<OpCode::Sub>(builder, count, Operand::Immediate(1, Type::U8));
            wide = Binary<OpCode::LsrValue>(builder, wide, Operand::ValueOf(index));
            carry = Bit(builder, wide, 0);
        }
        Require(builder.SetCarry(Type::Void, {Operand::ValueOf(carry)}));
        if (variable || constant == 1) {
            Value overflow;
            if (arithmetic)
                overflow = Constant(builder, 0);
            else if (!left && !rotate)
                overflow = Bit(builder, original, bits - 1);
            else
                overflow = Binary<OpCode::Xor>(
                        builder,
                        Bit(builder, result, bits - 1),
                        Operand::ValueOf(left ? carry : Bit(builder, result, bits - 2)));
            if (variable) {
                const auto old_overflow =
                        Require(builder.TestFlags(Type::Void, {Operand::Flags(Flag::Overflow)}));
                const auto different =
                        Binary<OpCode::Xor>(builder, count, Operand::Immediate(1, Type::U8));
                const auto one =
                        Require(builder.TestZero(Type::Void, {Operand::ValueOf(different)}));
                overflow = Require(builder.Select(Type::U64,
                                                  {Operand::ValueOf(one),
                                                   Operand::ValueOf(overflow),
                                                   Operand::ValueOf(old_overflow)}));
            }
            Require(builder.SetOverflow(Type::Void, {Operand::ValueOf(overflow)}));
        }
        if (variable) {
            const auto new_flags = Require(builder.GetFlags(
                    Type::U64, {Operand::ValueOf(result), Operand::Flags(Flag::All)}));
            const auto chosen = Require(builder.Select(Type::U64,
                                                       {Operand::ValueOf(count),
                                                        Operand::ValueOf(new_flags),
                                                        Operand::ValueOf(old_flags)}));
            Require(builder.RestoreFlags(Type::Void,
                                         {Operand::ValueOf(chosen), Operand::Flags(Flag::All)}));
        }
    }
    void PublishMultiply(Emitter& builder) {
        const auto& [left, right, low, data_high, bits, constant, opcode, wide_product] = recipe;
        const bool sign = opcode == Op::IMUL;
        Value different;
        if (sign && bits < 64) {
            auto full = low;
            if (!wide_product) {
                const auto a = Widen(builder, left, bits, true),
                           b = Widen(builder, right, bits, true);
                full = Require(builder.Mul(Type::U64, {Operand::ValueOf(a), Operand::ValueOf(b)}));
            }
            const auto expected = Widen(builder, low, bits, true);
            different = Require(
                    builder.Xor(Type::U64, {Operand::ValueOf(full), Operand::ValueOf(expected)}));
        } else {
            auto high = data_high;
            if (!high)
                high = bits == 64
                               ? Require(builder.MulHigh(Type::U64,
                                                         {Operand::ValueOf(left),
                                                          Operand::ValueOf(right),
                                                          Operand::Immediate(sign, Type::U8)}))
                               : Require(builder.BitExtract(Type::U64,
                                                            {Operand::ValueOf(low),
                                                             Operand::Immediate(bits, Type::U8),
                                                             Operand::Immediate(bits, Type::U8)}));
            different = high;
            if (sign) {
                const auto expected = Require(builder.AsrImm(
                        Type::U64, {Operand::ValueOf(low), Operand::Immediate(63, Type::U8)}));
                different = Require(builder.Xor(
                        Type::U64, {Operand::ValueOf(high), Operand::ValueOf(expected)}));
            }
        }
        const auto overflow =
                Require(builder.TestNotZero(Type::Void, {Operand::ValueOf(different)}));
        Require(builder.SetCarry(Type::Void, {Operand::ValueOf(overflow)}));
        Require(builder.SetOverflow(Type::Void, {Operand::ValueOf(overflow)}));
    }
    void Observe(Emitter& builder, u64 needed) {
        if (recipe.result && needed) {
            const bool multiply = recipe.opcode == Op::MUL || recipe.opcode == Op::IMUL;
            if (!multiply || (needed & FlagBits(Flag::Carry | Flag::Overflow))) {
                if (multiply)
                    PublishMultiply(builder);
                else
                    PublishShift(builder);
                recipe = {};
            }
        }
        if (!producer) return;
        const auto publish = outputs & needed;
        if (publish) {
            const auto metadata = builder.Metadata(producer);
            Require(builder.SetMetadata(producer,
                                        {metadata.flag_outputs | publish, profile, false}));
            outputs &= ~publish;
        }
        if (!outputs) producer = {};
    }
    void Observe(Emitter& builder, const InstructionData& instruction) {
        // The actual trusted decoder record already describes this consumer's
        // EFLAGS inputs. Reuse it rather than duplicate its condition table.
        const auto flags = instruction.flags_read;
        Observe(builder,
                (flags & GuestFlag::Carry ? FlagBits(Flag::Carry) : 0) |
                        (flags & GuestFlag::Parity ? FlagBits(Flag::Parity) : 0) |
                        (flags & GuestFlag::Auxiliary ? FlagBits(Flag::AuxiliaryCarry) : 0) |
                        (flags & GuestFlag::Zero ? FlagBits(Flag::Zero) : 0) |
                        (flags & GuestFlag::Sign ? FlagBits(Flag::Negate) : 0) |
                        (flags & GuestFlag::Overflow ? FlagBits(Flag::Overflow) : 0));
    }
    void Flush(Emitter& builder) { Observe(builder, FlagBits(Flag::All)); }
};

static void BitScan(Emitter& builder,
                    StateLayout layout,
                    const InstructionData& instruction,
                    PendingFlags& pending,
                    u64 pc,
                    u64 next_pc,
                    TargetInfo target) {
    if (instruction.ops[2].type != GuestOperandKind::None ||
        instruction.ops[3].type != GuestOperandKind::None)
        Unsupported();
    const auto& destination = instruction.ops[0];
    const auto& source = instruction.ops[1];
    const auto bits = destination.size;
    if (destination.type != GuestOperandKind::Register ||
        (source.type != GuestOperandKind::Register && !Memory(source)) ||
        (bits != 16 && bits != 32 && bits != 64) || source.size != bits)
        Unsupported();
    if (Memory(source)) {
        pending.Flush(builder);
        PrepareFaultPC(builder, layout, pc, target);
    }
    const auto value = ReadData(builder, layout, instruction, source, next_pc, target);
    const auto count_type = bits == 64 ? Type::U64 : Type::U32;
    const auto input =
            bits == 16
                    ? Require(builder.ZeroExtend32(Type::U32, {Operand::ValueOf(value, Type::U16)}))
                    : value;
    if (instruction.opcode == Op::TZCNT) {
        const auto carry = Require(builder.TestZero(Type::Void, {Operand::ValueOf(input)}));
        // A set sentinel above the word leaves every nonzero word's trailing
        // count intact, and makes its zero result 16 rather than CTZ32's 32.
        const auto counted =
                bits == 16 ? Require(builder.Or(Type::U32,
                                                {Operand::ValueOf(input),
                                                 Operand::Immediate(1u << 16, Type::U32)}))
                           : input;
        const auto result = bits == 64 ? Require(builder.CountTrailingZeros64(
                                                 Type::U64, {Operand::ValueOf(counted)}))
                                       : Require(builder.CountTrailingZeros32(
                                                 Type::U32, {Operand::ValueOf(counted)}));
        WriteRegister(builder, layout, destination, result);
        Require(builder.SetCarry(Type::Void, {Operand::ValueOf(carry)}));
        const auto flags = Require(builder.Sub(
                count_type, {Operand::ValueOf(result), Operand::Immediate(0, count_type)}));
        pending = {builder.ProducerOf(flags),
                   FlagProfile::ArithmeticCanonicalCarry,
                   FlagBits(Flag::Zero),
                   {}};
        return;
    }
    // Consume the old source before the count can take its destination pin.
    const auto flags = Require(builder.Sub(
            ScalarType(bits), {Operand::ValueOf(value), Operand::Immediate(0, ScalarType(bits))}));
    const bool reverse = instruction.opcode == Op::BSR;
    auto result =
            bits == 64 ? reverse ? Require(builder.CountLeadingZeros64(Type::U64,
                                                                       {Operand::ValueOf(input)}))
                                 : Require(builder.CountTrailingZeros64(Type::U64,
                                                                        {Operand::ValueOf(input)}))
            : reverse ? Require(builder.CountLeadingZeros32(Type::U32, {Operand::ValueOf(input)}))
                      : Require(builder.CountTrailingZeros32(Type::U32, {Operand::ValueOf(input)}));
    if (reverse)
        result = Require(builder.Xor(
                count_type,
                {Operand::ValueOf(result), Operand::Immediate(bits == 64 ? 63 : 31, count_type)}));
    WriteRegister(builder, layout, destination, result);
    // ZF tests the original source, not the scan index. A zero source leaves
    // the destination and the other arithmetic flags architecturally undefined.
    pending = {builder.ProducerOf(flags),
               FlagProfile::ArithmeticCanonicalCarry,
               FlagBits(Flag::Zero),
               {}};
}

static void PopulationCount(Emitter& builder,
                            StateLayout layout,
                            const InstructionData& instruction,
                            PendingFlags& pending,
                            u64 pc,
                            u64 next_pc,
                            TargetInfo target) {
    if (instruction.ops[2].type != GuestOperandKind::None ||
        instruction.ops[3].type != GuestOperandKind::None)
        Unsupported();
    const auto& destination = instruction.ops[0];
    const auto& source = instruction.ops[1];
    const auto bits = destination.size;
    if (destination.type != GuestOperandKind::Register ||
        (source.type != GuestOperandKind::Register && !Memory(source)) ||
        (bits != 16 && bits != 32 && bits != 64) || source.size != bits)
        Unsupported();
    if (Memory(source)) {
        pending.Flush(builder);
        PrepareFaultPC(builder, layout, pc, target);
    }
    auto input = ReadData(builder, layout, instruction, source, next_pc, target);
    // PopCount consumes the whole raw container, so normalize a narrow source
    // explicitly rather than merely change the view of a wider carrier.
    if (bits != 64)
        input = Require(
                builder.ZeroExtend64(Type::U64, {Operand::ValueOf(input, ScalarType(bits))}));
    const auto result = Require(builder.PopCount(Type::U64, {Operand::ValueOf(input)}));
    WriteRegister(builder, layout, destination, result);
    Require(builder.ClearFlags(Type::Void,
                               {Operand::Flags(FlagBits(Flag::All) & ~FlagBits(Flag::Zero))}));
    // A zero count is exactly a zero source; only ZF remains deferred.
    const auto flags = Require(
            builder.Sub(Type::U64, {Operand::ValueOf(result), Operand::Immediate(0, Type::U64)}));
    pending = {builder.ProducerOf(flags),
               FlagProfile::ArithmeticCanonicalCarry,
               FlagBits(Flag::Zero),
               {}};
}

static void Crc32(Emitter& builder,
                  StateLayout layout,
                  const InstructionData& instruction,
                  PendingFlags& pending,
                  u64 pc,
                  u64 next_pc,
                  TargetInfo target) {
    auto destination = instruction.ops[0], source = instruction.ops[1];
    if (instruction.ops[2].type != GuestOperandKind::None ||
        instruction.ops[3].type != GuestOperandKind::None ||
        destination.type != GuestOperandKind::Register ||
        (destination.size != 16 && destination.size != 32 && destination.size != 64) ||
        (source.type != GuestOperandKind::Register && !Memory(source)))
        Unsupported();
    ScalarType(source.size);
    // The adapter supplies the effective 32-bit destination and encoded data
    // width, including 66 and REX.W interactions.
    unsigned destination_slot{};
    RegisterOffset(layout, destination.index, destination.size, destination_slot);
    const unsigned bits = source.size;
    if (Memory(source)) {
        pending.Flush(builder);
        PrepareFaultPC(builder, layout, pc, target);
    }
    const auto accumulator = ReadRegister(builder, layout, Gpr(destination_slot, 32).index, 32);
    const auto data = ReadData(builder, layout, instruction, source, next_pc, target);
    const auto result = Require(builder.Crc32c(Type::U32,
                                               {Operand::ValueOf(accumulator, Type::U32),
                                                Operand::ValueOf(data, ScalarType(bits)),
                                                Operand::Immediate(bits, Type::U8)}));
    WriteRegister(builder, layout, destination, result);
}

static void CompareExchange(Emitter& builder,
                            StateLayout layout,
                            const InstructionData& instruction,
                            PendingFlags& pending,
                            u64 pc,
                            u64 next_pc,
                            TargetInfo target) {
    if (instruction.ops[2].type != GuestOperandKind::None ||
        instruction.ops[3].type != GuestOperandKind::None ||
        (instruction.ops[0].type != GuestOperandKind::Register && !Memory(instruction.ops[0])) ||
        instruction.ops[1].type != GuestOperandKind::Register)
        Unsupported();
    const auto& destination = instruction.ops[0];
    const auto& source = instruction.ops[1];
    const auto bits = destination.size;
    const auto type = ScalarType(bits);
    const bool locked = instruction.flags & InstructionFlag::Locked;
    Value address;
    if (Memory(destination)) {
        pending.Flush(builder);
        PrepareFaultPC(builder, layout, pc, target);
        address = Address(builder, layout, instruction, destination, next_pc, target);
    }
    // Capture complete 32-bit parents before any conditional writes. Their
    // high halves survive a non-written branch, including operand aliases.
    const auto accumulator = ReadRegister(
            builder, layout, Gpr(0, bits == 32 ? 64 : bits).index, bits == 32 ? 64 : bits);
    const auto desired = ReadRegister(builder, layout, source.index, bits);
    Value old;
    unsigned destination_slot{};
    if (address)
        old = locked ? Require(builder.CompareAndSwap(type,
                                                      {Operand::ValueOf(address),
                                                       Operand::ValueOf(accumulator, type),
                                                       Operand::ValueOf(desired, type)}))
                     : Require(builder.LoadMemoryTSO(type, {Operand::ValueOf(address)}));
    else {
        RegisterOffset(layout, destination.index, bits, destination_slot);
        old = bits == 32 ? ReadRegister(builder, layout, Gpr(destination_slot, 64).index, 64)
                         : ReadRegister(builder, layout, destination.index, bits);
    }
    const auto compare = [&] {
        return Require(builder.Sub(
                type, {Operand::ValueOf(accumulator, type), Operand::ValueOf(old, type)}));
    };
    auto flags = compare();
    const auto equal = Require(builder.TestZero(Type::Bool, {Operand::ValueOf(flags)}));
    const auto select = [&](Type result_type, Value yes, Value no) {
        return Require(builder.Select(
                result_type,
                {Operand::ValueOf(equal), Operand::ValueOf(yes), Operand::ValueOf(no)}));
    };
    if (address && !locked) {
        // Keep ordinary unaligned memory legal, including the failed compare's
        // write cycle. Pure selection restores guest NZCV before this store.
        StoreMemory(builder, address, select(type, desired, old), bits);
        flags = compare();  // Old flags remain live until the store succeeds.
    }
    if (bits == 32) {
        const auto observed64 =
                Require(builder.ZeroExtend64(Type::U64, {Operand::ValueOf(old, Type::U32)}));
        WriteRegister(builder, layout, Gpr(0, 64), select(Type::U64, accumulator, observed64));
        if (!address) {
            const auto desired64 = Require(
                    builder.ZeroExtend64(Type::U64, {Operand::ValueOf(desired, Type::U32)}));
            WriteRegister(
                    builder, layout, Gpr(destination_slot, 64), select(Type::U64, desired64, old));
        }
    } else {
        // Partial writes preserve other lanes. Accumulator first also handles
        // AL/AH aliases; a destination equal to the accumulator wins last.
        WriteRegister(builder, layout, Gpr(0, bits), select(type, accumulator, old));
        if (!address) WriteRegister(builder, layout, destination, select(type, desired, old));
    }
    pending = {builder.ProducerOf(flags),
               FlagProfile::ArithmeticCanonicalCarry,
               FlagBits(Flag::All),
               {}};
}

static void ExchangeAdd(Emitter& builder,
                        StateLayout layout,
                        const InstructionData& instruction,
                        PendingFlags& pending,
                        u64 pc,
                        u64 next_pc,
                        TargetInfo target) {
    if (instruction.ops[2].type != GuestOperandKind::None ||
        instruction.ops[3].type != GuestOperandKind::None ||
        (instruction.ops[0].type != GuestOperandKind::Register && !Memory(instruction.ops[0])) ||
        instruction.ops[1].type != GuestOperandKind::Register)
        Unsupported();
    const auto& destination = instruction.ops[0];
    const auto& source = instruction.ops[1];
    const auto type = ScalarType(destination.size);
    Value address;
    if (Memory(destination)) {
        pending.Flush(builder);
        PrepareFaultPC(builder, layout, pc, target);
        address = Address(builder, layout, instruction, destination, next_pc, target);
    }
    const auto addend = ReadRegister(builder, layout, source.index, source.size);
    const bool locked = instruction.flags & InstructionFlag::Locked;
    const auto old =
            address ? (locked ? Require(builder.AtomicFetchAdd(
                                        type,
                                        {Operand::ValueOf(address), Operand::ValueOf(addend)}))
                              : Require(builder.LoadMemoryTSO(type, {Operand::ValueOf(address)})))
                    : ReadRegister(builder, layout, destination.index, destination.size);
    const auto calculate = [&] {
        return Require(
                builder.Add(type, {Operand::ValueOf(old, type), Operand::ValueOf(addend, type)}));
    };
    const auto result = calculate();
    auto flags = result;
    if (address && !locked) {
        StoreMemory(builder, address, result, destination.size);
        flags = calculate();  // A fault must still observe the preceding flags.
    }
    // Source first, destination last: XADD eax,eax and AL/AH overlap retain
    // the architectural result rather than the intermediate old-value copy.
    WriteRegister(builder, layout, source, old);
    if (!address) WriteRegister(builder, layout, destination, result);
    pending = {builder.ProducerOf(flags),
               FlagProfile::ArithmeticCanonicalCarry,
               FlagBits(Flag::All),
               {}};
}

static void Multiply(Emitter& builder,
                     StateLayout layout,
                     const InstructionData& instruction,
                     PendingFlags& pending,
                     u64 pc,
                     u64 next_pc,
                     TargetInfo target) {
    const bool sign = instruction.opcode == Op::IMUL;
    const auto& destination = instruction.ops[0];
    const bool implicit = instruction.ops[1].type == GuestOperandKind::None;
    const auto bits = destination.size;
    ScalarType(bits);
    if (instruction.ops[3].type != GuestOperandKind::None || (!sign && !implicit)) Unsupported();
    Value left, right;
    if (implicit) {
        if ((destination.type != GuestOperandKind::Register && !Memory(destination)) ||
            instruction.ops[2].type != GuestOperandKind::None)
            Unsupported();
        if (Memory(destination)) PrepareFaultPC(builder, layout, pc, target);
        left = ReadRegister(builder, layout, Gpr(0, bits).index, bits);
        right = ReadData(builder, layout, instruction, destination, next_pc, target);
    } else {
        const auto& source = instruction.ops[1];
        const auto& immediate = instruction.ops[2];
        if (destination.type != GuestOperandKind::Register || bits == 8 || source.size != bits ||
            (source.type != GuestOperandKind::Register && !Memory(source)) ||
            (immediate.type != GuestOperandKind::None &&
             immediate.type != GuestOperandKind::Immediate))
            Unsupported();
        if (Memory(source)) PrepareFaultPC(builder, layout, pc, target);
        if (immediate.type == GuestOperandKind::Immediate) {
            left = ReadData(builder, layout, instruction, source, next_pc, target);
            right = Constant(builder, ImmediateBits(instruction, bits), ScalarType(bits));
        } else {
            left = ReadRegister(builder, layout, destination.index, bits);
            right = ReadData(builder, layout, instruction, source, next_pc, target);
        }
    }
    // Explicit IMUL needs only its N-bit low product. Widening and signed
    // overflow work are flags-only; implicit AX / high:low outputs retain
    // their actual wider data product.
    const bool wide_product = implicit || bits == 64;
    Value low, high;
    if (wide_product) {
        const auto a = Widen(builder, left, bits, sign), b = Widen(builder, right, bits, sign);
        low = Require(builder.Mul(Type::U64, {Operand::ValueOf(a), Operand::ValueOf(b)}));
        if (implicit && bits != 8)
            high = bits == 64 ? Require(builder.MulHigh(Type::U64,
                                                        {Operand::ValueOf(a),
                                                         Operand::ValueOf(b),
                                                         Operand::Immediate(sign, Type::U8)}))
                              : Require(builder.BitExtract(Type::U64,
                                                           {Operand::ValueOf(low),
                                                            Operand::Immediate(bits, Type::U8),
                                                            Operand::Immediate(bits, Type::U8)}));
    } else {
        const auto type = ScalarType(bits);
        low = Require(
                builder.Mul(type, {Operand::ValueOf(left, type), Operand::ValueOf(right, type)}));
    }
    if (!implicit)
        WriteRegister(builder, layout, destination, low);
    else if (bits == 8)
        WriteRegister(builder, layout, Gpr(0, 16), low);
    else {
        WriteRegister(builder, layout, Gpr(0, bits), low);
        WriteRegister(builder, layout, Gpr(2, bits), high);
    }
    pending.recipe = {left, right, low, high, bits, 0, instruction.opcode, wide_product};
}

static void Divide(Emitter& builder,
                   StateLayout layout,
                   const InstructionData& instruction,
                   u64 pc,
                   u64 next_pc,
                   TargetInfo target) {
    OneOperand(instruction);
    const auto& source = instruction.ops[0];
    if (source.type != GuestOperandKind::Register && !Memory(source)) Unsupported();
    const auto bits = source.size;
    const auto type = ScalarType(bits);
    PrepareFaultPC(builder, layout, pc, target, true);
    const auto divisor = ReadData(builder, layout, instruction, source, next_pc, target);
    const auto low = ReadRegister(builder, layout, Gpr(0, bits).index, bits);
    const auto high = ReadRegister(
            builder, layout, bits == 8 ? unsigned(Register::AH) : Gpr(2, bits).index, bits);
    // Same-width halves and divisor define the actual 2N/N operation. The
    // backend checks division by zero and quotient overflow at N, including
    // arbitrary high halves; there is no high==0 shortcut.
    const auto quotient =
            Require(builder.Div128(Type::Void,
                                   {Operand::ValueOf(high, type),
                                    Operand::ValueOf(low, type),
                                    Operand::ValueOf(divisor, type),
                                    Operand::Immediate(instruction.opcode == Op::IDIV, Type::U8)}));
    const auto remainder =
            Require(builder.Div128Remainder(Type::Void, {Operand::ValueOf(quotient)}));
    WriteRegister(builder, layout, Gpr(0, bits), quotient);
    WriteRegister(
            builder,
            layout,
            bits == 8 ? DecodedOperand{GuestOperandKind::Register, Register::AH, 8} : Gpr(2, bits),
            remainder);
}

template <OpCode op>
static Value Binary(Emitter& builder, Value left, Operand right, Type type = Type::U64) {
    return Require(builder.template Apply<op>(type, {Operand::ValueOf(left), right}));
}
static Value Bit(Emitter& builder, Value value, unsigned bit) {
    return Require(builder.TestBit(Type::Void,
                                   {Operand::ValueOf(value), Operand::Immediate(bit, Type::U8)}));
}
static void BitTest(Emitter& builder, StateLayout layout, const InstructionData& instruction) {
    const auto& source = instruction.ops[0];
    const auto& index = instruction.ops[1];
    if (source.type != GuestOperandKind::Register ||
        (source.size != 16 && source.size != 32 && source.size != 64) ||
        instruction.ops[2].type != GuestOperandKind::None ||
        instruction.ops[3].type != GuestOperandKind::None ||
        (index.type != GuestOperandKind::Immediate &&
         !(index.type == GuestOperandKind::Register && index.size == source.size)))
        Unsupported();
    const auto value = ReadRegister(builder, layout, source.index, source.size);
    Value carry;
    if (index.type == GuestOperandKind::Immediate)
        carry = Bit(builder,
                    value,
                    static_cast<std::uint8_t>(instruction.immediate) & (source.size - 1));
    else {
        const auto shift = ReadRegister(builder, layout, index.index, index.size);
        const auto selected = Binary<OpCode::LsrValue>(
                builder, value, Operand::ValueOf(shift), ScalarType(source.size));
        carry = Bit(builder, selected, 0);
    }
    Require(builder.SetCarry(Type::Void, {Operand::ValueOf(carry)}));
}
static void Shift(Emitter& builder,
                  StateLayout layout,
                  const InstructionData& instruction,
                  PendingFlags& pending,
                  u64 pc,
                  u64 next_pc,
                  TargetInfo target) {
    const auto& destination = instruction.ops[0];
    const auto& amount = instruction.ops[1];
    const auto bits = destination.size;
    ScalarType(bits);
    if ((destination.type != GuestOperandKind::Register && !Memory(destination)) ||
        instruction.ops[2].type != GuestOperandKind::None ||
        instruction.ops[3].type != GuestOperandKind::None ||
        (amount.type != GuestOperandKind::Immediate &&
         !(amount.type == GuestOperandKind::Register && amount.index == Register::CL &&
           amount.size == 8)))
        Unsupported();
    const bool variable = amount.type == GuestOperandKind::Register;
    const unsigned constant =
            static_cast<unsigned>(static_cast<std::uint8_t>(instruction.immediate)) &
            (bits == 64 ? 63 : 31);
    const bool rotate = instruction.opcode == Op::ROL || instruction.opcode == Op::ROR;
    const bool left = instruction.opcode == Op::SHL || instruction.opcode == Op::SAL ||
                      instruction.opcode == Op::ROL;
    const bool arithmetic = instruction.opcode == Op::SAR;
    Value count;
    if (variable) {
        count = ReadRegister(builder, layout, Register::CL, 8);
        count = Binary<OpCode::And>(
                builder, count, Operand::Immediate(bits == 64 ? 63 : 31, Type::U8));
    }
    Value address;
    if (Memory(destination)) {
        PrepareFaultPC(builder, layout, pc, target);
        address = Address(builder, layout, instruction, destination, next_pc, target);
    }
    const auto original =
            address ? Require(builder.LoadMemoryTSO(ScalarType(bits), {Operand::ValueOf(address)}))
                    : ReadRegister(builder, layout, destination.index, bits);
    Value input = original;
    const auto work_type = bits == 64 ? Type::U64 : Type::U32;
    if (bits < 32)
        input = arithmetic
                        ? Require(builder.SignExtend(Type::U32, {Operand::ValueOf(original)}))
                        : Require(builder.ZeroExtend32(Type::Void, {Operand::ValueOf(original)}));
    Value result;
    if (rotate) {
        if (bits < 32) {
            // Repeat the narrow bit pattern to fill the numeric 32-bit rotate.
            const auto doubled = Binary<OpCode::LslImm>(
                    builder, input, Operand::Immediate(bits, Type::U8), Type::U32);
            input = Binary<OpCode::Or>(builder, input, Operand::ValueOf(doubled), Type::U32);
            if (bits == 8) {
                const auto upper = Binary<OpCode::LslImm>(
                        builder, input, Operand::Immediate(16, Type::U8), Type::U32);
                input = Binary<OpCode::Or>(builder, input, Operand::ValueOf(upper), Type::U32);
            }
        }
        if (variable) {
            auto rotation = count;
            if (left)
                rotation =
                        Binary<OpCode::Sub>(builder, Constant(builder, 0), Operand::ValueOf(count));
            result =
                    Binary<OpCode::RorValue>(builder, input, Operand::ValueOf(rotation), work_type);
        } else {
            const auto rotation = (left ? 0u - constant : constant) & (bits == 64 ? 63u : 31u);
            result = Binary<OpCode::RorImm>(
                    builder, input, Operand::Immediate(rotation, Type::U8), work_type);
        }
    } else if (variable) {
        if (left)
            result = Binary<OpCode::LslValue>(builder, input, Operand::ValueOf(count), work_type);
        else if (arithmetic)
            result = Binary<OpCode::AsrValue>(builder, input, Operand::ValueOf(count), work_type);
        else
            result = Binary<OpCode::LsrValue>(builder, input, Operand::ValueOf(count), work_type);
    } else {
        const auto immediate = Operand::Immediate(constant, Type::U8);
        if (left)
            result = Binary<OpCode::LslImm>(builder, input, immediate, work_type);
        else if (arithmetic)
            result = Binary<OpCode::AsrImm>(builder, input, immediate, work_type);
        else
            result = Binary<OpCode::LsrImm>(builder, input, immediate, work_type);
    }
    if (address)
        StoreMemory(builder, address, result, bits);
    else
        WriteRegister(builder, layout, destination, result);
    if (!variable && !constant) return;  // Count zero preserves every flag.

    // The incoming flags were flushed before this instruction. Until a real
    // observer, no flags writer runs without consuming or replacing this one
    // recipe. Captured SSA inputs survive through their eventual actual uses;
    // a full writer can discard the recipe without birthing its flag chain.
    pending.recipe = {original, count, result, {}, bits, constant, instruction.opcode, false};
}

template <OpCode op, bool step = false> static void Alu(Emitter& builder,
                                                        StateLayout layout,
                                                        const InstructionData& instruction,
                                                        PendingFlags& pending,
                                                        u64 pc,
                                                        u64 next_pc,
                                                        TargetInfo target,
                                                        bool write_result = true) {
    const auto& destination = instruction.ops[0];
    const auto& source = instruction.ops[1];
    const auto bits = destination.size;
    const auto type = ScalarType(bits);
    if ((destination.type != GuestOperandKind::Register && !Memory(destination)) ||
        instruction.ops[2].type != GuestOperandKind::None ||
        instruction.ops[3].type != GuestOperandKind::None)
        Unsupported();
    if constexpr (op == OpCode::Neg || step) {
        if (source.type != GuestOperandKind::None) Unsupported();
    } else {
        if ((source.type != GuestOperandKind::Register &&
             source.type != GuestOperandKind::Immediate && !Memory(source)) ||
            (source.type != GuestOperandKind::Immediate && source.size != bits) ||
            (Memory(destination) && Memory(source)))
            Unsupported();
    }
    if (Memory(destination) || Memory(source)) {
        pending.Flush(builder);
        PrepareFaultPC(builder, layout, pc, target);
    }
    // These writers consume/preserve the incoming carry, then replace every
    // other defined arithmetic bit. Unobserved old non-carry bits die here.
    if constexpr (step || op == OpCode::Adc || op == OpCode::Sbb)
        pending.Observe(builder, FlagBits(Flag::Carry));
    const bool locked = instruction.flags & InstructionFlag::Locked;
    Value address, left, incoming_carry;
    Operand right;
    if (locked) {
        if (!Memory(destination) || !write_result) Unsupported();
        address = Address(builder, layout, instruction, destination, next_pc, target);
        Value operand;
        if constexpr (op == OpCode::Neg)
            operand = Constant(builder, 0, type);
        else if constexpr (step)
            operand = Constant(builder, 1, type);
        else if (source.type == GuestOperandKind::Immediate)
            operand = Constant(builder, ImmediateBits(instruction, bits), type);
        else
            operand = ReadRegister(builder, layout, source.index, bits);
        if constexpr (op != OpCode::Neg) right = Operand::ValueOf(operand, type);
        Value carry_input;
        if constexpr (op == OpCode::Adc || op == OpCode::Sbb) {
            incoming_carry = Require(builder.TestFlags(Type::Void, {Operand::Flags(Flag::Carry)}));
            carry_input = incoming_carry;
        } else
            carry_input = Constant(builder, 0, Type::U8);
        constexpr auto kind = [] {
            if constexpr (op == OpCode::Add)
                return AtomicRMWOp::Add;
            else if constexpr (op == OpCode::Sub)
                return AtomicRMWOp::Sub;
            else if constexpr (op == OpCode::And)
                return AtomicRMWOp::And;
            else if constexpr (op == OpCode::Or)
                return AtomicRMWOp::Or;
            else if constexpr (op == OpCode::Xor)
                return AtomicRMWOp::Xor;
            else if constexpr (op == OpCode::Adc)
                return AtomicRMWOp::AddCarry;
            else if constexpr (op == OpCode::Sbb)
                return AtomicRMWOp::SubBorrow;
            else
                return AtomicRMWOp::Neg;
        }();
        left = Require(builder.AtomicRMW(type,
                                         {Operand::Immediate(static_cast<u64>(kind), Type::U8),
                                          Operand::ValueOf(address),
                                          Operand::ValueOf(operand, type),
                                          Operand::ValueOf(carry_input)}));
        if (incoming_carry)
            Require(builder.SetCarry(Type::Void, {Operand::ValueOf(incoming_carry)}));
    } else {
        left = destination.type == GuestOperandKind::Register
                       ? ReadRegister(builder, layout, destination.index, bits)
                       : [&] {
                             address = Address(
                                     builder, layout, instruction, destination, next_pc, target);
                             return Require(
                                     builder.LoadMemoryTSO(type, {Operand::ValueOf(address)}));
                         }();
        if constexpr (op != OpCode::Neg) {
            if constexpr (step)
                right = Operand::Immediate(1, type);
            else if (source.type == GuestOperandKind::Immediate)
                right = Operand::Immediate(ImmediateBits(instruction, bits), type);
            else if (destination.type == GuestOperandKind::Register &&
                     source.type == GuestOperandKind::Register &&
                     destination.index == source.index && destination.size == source.size)
                right = Operand::ValueOf(left);
            else
                right = Operand::ValueOf(
                        ReadData(builder, layout, instruction, source, next_pc, target));
        }
        if constexpr (op == OpCode::Adc || op == OpCode::Sbb)
            if (write_result && Memory(destination))
                incoming_carry =
                        Require(builder.TestFlags(Type::Void, {Operand::Flags(Flag::Carry)}));
    }
    const auto calculate = [&] {
        if constexpr (op == OpCode::Neg)
            return Require(builder.template Apply<op>(type, {Operand::ValueOf(left)}));
        else
            return Require(builder.template Apply<op>(type, {Operand::ValueOf(left), right}));
    };
    const auto result = calculate();
    Value flags = result;
    if (write_result && !locked) {
        if (destination.type == GuestOperandKind::Register)
            WriteRegister(builder, layout, destination, result);
        else {
            StoreMemory(builder, address, result, bits);
            if (incoming_carry)
                Require(builder.SetCarry(Type::Void, {Operand::ValueOf(incoming_carry)}));
            // The store may fault. Keep old flags until it succeeds, then
            // produce the new flags from the same real operand edges. A later
            // full writer can discard this otherwise unused second result.
            flags = calculate();
        }
    }
    // Every accepted ALU here replaces all arithmetic flags. With no observer
    // since the preceding writer, only the latest producer needs publication.
    pending = {builder.ProducerOf(flags),
               op == OpCode::And || op == OpCode::Or || op == OpCode::Xor
                       ? FlagProfile::LogicalZeroCVA
                       : FlagProfile::ArithmeticCanonicalCarry,
               step ? FlagBits(Flag::All) & ~FlagBits(Flag::Carry) : FlagBits(Flag::All),
               {}};
}
