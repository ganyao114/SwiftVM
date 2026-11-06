// MMX is the low64 of the eight physical x87 registers, not a second state
// bank. Numerical vector work uses existing neutral V128 operations; only
// the low64 is committed and every packing operation uses MMX geometry.
static bool MmxRegister(const DecodedOperand& operand) {
    return operand.type == GuestOperandKind::Register && operand.index >= Register::MM0 &&
           operand.index <= Register::MM7;
}
static u32 MmxOffset(StateLayout layout, const DecodedOperand& operand) {
    if (!MmxRegister(operand) || layout.x87_offset == UINT32_MAX) Unsupported();
    return layout.x87_offset + (operand.index - Register::MM0) * sizeof(runtime::x86::X87Value);
}
static void MmxEnter(Emitter& builder, StateLayout layout) {
    using State = runtime::x86::X87State;
    const auto status = X87Query(builder, layout, offsetof(State, status), Type::U16);
    const auto top_zero = Require(builder.And(
            Type::U16, {Operand::ValueOf(status), Operand::Immediate(0xc7ff, Type::U16)}));
    X87Field(builder, layout.x87_offset + offsetof(State, status), Type::U16, top_zero);
    X87Field(builder,
             layout.x87_offset + offsetof(State, tags),
             Type::U16,
             Constant(builder, 0, Type::U16));
}
static Value MmxRead(Emitter& builder,
                     StateLayout layout,
                     const InstructionData& instruction,
                     const DecodedOperand& operand,
                     u64 pc,
                     u64 next_pc,
                     TargetInfo target) {
    if (MmxRegister(operand))
        return Require(builder.LoadUniform(
                Type::U64, {Operand::Uniform(MmxOffset(layout, operand), Type::U64)}));
    if (!Memory(operand) || operand.size != 64) Unsupported();
    PrepareFaultPC(builder, layout, pc, target);
    return ReadData(builder, layout, instruction, operand, next_pc, target);
}
static void MmxWrite(Emitter& builder,
                     StateLayout layout,
                     const DecodedOperand& operand,
                     Value value) {
    const auto offset = MmxOffset(layout, operand);
    X87Field(builder, offset, Type::U64, value);
    X87Field(builder,
             offset + offsetof(runtime::x86::X87Value, sign_exp),
             Type::U16,
             Constant(builder, 0xffff, Type::U16));
}

static bool Mmx(Emitter& builder,
                StateLayout layout,
                const InstructionData& instruction,
                PendingFlags& pending,
                u64 pc,
                u64 next_pc,
                TargetInfo target) {
    const auto destination = instruction.ops[0], source = instruction.ops[1];
    // The memory-source integer conversions have no MMX register operand:
    // retain x87 tags/TOP and do not observe pending x87 exceptions.
    if ((instruction.opcode == Op::CVTPI2PS || instruction.opcode == Op::CVTPI2PD) &&
        Memory(source)) {
        if (!VRegister(destination) || source.size != 64 ||
            instruction.ops[2].type != GuestOperandKind::None ||
            instruction.ops[3].type != GuestOperandKind::None)
            Unsupported();
        pending.Flush(builder);
        PrepareFaultPC(builder, layout, pc, target);
        const auto input = VLowScalar(
                builder, ReadData(builder, layout, instruction, source, next_pc, target), 64);
        const bool packed_double = instruction.opcode == Op::CVTPI2PD;
        auto result = Require(builder.VecFCvtPacked(
                Type::V128,
                {Operand::ValueOf(input), Operand::Immediate(packed_double ? 1 : 0, Type::U8)}));
        if (!packed_double) {
            const auto bits = Require(builder.VecExtract64(
                    Type::U64, {Operand::ValueOf(result), Operand::Immediate(0, Type::U8)}));
            result = VReplaceLane(
                    builder,
                    VRead(builder, layout, instruction, destination, pc, next_pc, target),
                    bits,
                    64,
                    0);
        }
        VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        return true;
    }
    if (instruction.opcode != Op::EMMS && !MmxRegister(destination) && !MmxRegister(source))
        return false;
    if (layout.x87_offset == UINT32_MAX || instruction.ops[3].type != GuestOperandKind::None)
        Unsupported();
    pending.Flush(builder);
    PrepareFaultPC(builder, layout, pc, target);
    const auto zero = builder.X87Zero();
    const auto state = builder.X87Address(layout.x87_offset);
    // MMX observes pending x87 exceptions before any guest memory access.
    X87Call(builder, state, runtime::x86::X87Encode(runtime::x86::X87Command::Wait), zero);
    const auto raw = [&](const DecodedOperand& operand) {
        return MmxRead(builder, layout, instruction, operand, pc, next_pc, target);
    };
    const auto vector = [&](const DecodedOperand& operand) {
        return VLowScalar(builder, raw(operand), 64);
    };
    const auto low = [&](Value value) {
        return Require(builder.VecExtract64(
                Type::U64, {Operand::ValueOf(value), Operand::Immediate(0, Type::U8)}));
    };
    const auto commit = [&](Value value) {
        MmxEnter(builder, layout);
        MmxWrite(builder, layout, destination, value);
    };
    const auto enter = [&] { MmxEnter(builder, layout); };
    const auto no_third = [&] {
        if (instruction.ops[2].type != GuestOperandKind::None) Unsupported();
    };
    if (instruction.opcode == Op::EMMS) {
        if (destination.type != GuestOperandKind::None || source.type != GuestOperandKind::None)
            Unsupported();
        no_third();
        X87Field(builder,
                 layout.x87_offset + offsetof(runtime::x86::X87State, tags),
                 Type::U16,
                 Constant(builder, 0xffff, Type::U16));
        return true;
    }
    if (instruction.opcode == Op::MOVD || instruction.opcode == Op::MOVQ ||
        instruction.opcode == Op::MOVNTQ) {
        no_third();
        const bool narrow = instruction.opcode == Op::MOVD;
        const unsigned bits = narrow ? 32 : 64;
        if (MmxRegister(destination)) {
            Value value;
            if (!narrow && MmxRegister(source))
                value = raw(source);
            else {
                if ((!Memory(source) && source.type != GuestOperandKind::Register) ||
                    MmxRegister(source) || VRegister(source) || source.size != bits)
                    Unsupported();
                value = ReadData(builder, layout, instruction, source, next_pc, target);
                if (narrow)
                    value = Require(
                            builder.ZeroExtend64(Type::U64, {Operand::ValueOf(value, Type::U32)}));
            }
            if (instruction.opcode == Op::MOVNTQ) Unsupported();
            commit(value);
        } else {
            if (!MmxRegister(source)) Unsupported();
            const auto value = raw(source);
            if (Memory(destination)) {
                if (destination.size != bits) Unsupported();
                const auto address =
                        Address(builder, layout, instruction, destination, next_pc, target);
                if (instruction.opcode == Op::MOVNTQ)
                    StoreMemory<OpCode::StoreMemory>(builder, address, value, bits);
                else
                    StoreMemory(builder, address, value, bits);
            } else {
                if (instruction.opcode == Op::MOVNTQ ||
                    destination.type != GuestOperandKind::Register || VRegister(destination) ||
                    destination.size != bits)
                    Unsupported();
                WriteRegister(builder, layout, destination, value);
            }
            enter();
        }
        return true;
    }
    if (instruction.opcode == Op::MOVDQ2Q || instruction.opcode == Op::MOVQ2DQ) {
        no_third();
        if (instruction.opcode == Op::MOVDQ2Q) {
            if (!MmxRegister(destination) || !VRegister(source)) Unsupported();
            commit(low(VRead(builder, layout, instruction, source, pc, next_pc, target)));
        } else {
            if (!VRegister(destination) || !MmxRegister(source)) Unsupported();
            const auto value = vector(source);
            VWrite(builder, layout, instruction, destination, value, pc, next_pc, target);
            enter();
        }
        return true;
    }
    if (instruction.opcode == Op::CVTPI2PS || instruction.opcode == Op::CVTPI2PD) {
        no_third();
        if (!VRegister(destination) ||
            (!MmxRegister(source) && (!Memory(source) || source.size != 64)))
            Unsupported();
        const auto input = vector(source);
        const bool packed_double = instruction.opcode == Op::CVTPI2PD;
        auto result = Require(builder.VecFCvtPacked(
                Type::V128,
                {Operand::ValueOf(input), Operand::Immediate(packed_double ? 1 : 0, Type::U8)}));
        if (!packed_double)
            result = VReplaceLane(
                    builder,
                    VRead(builder, layout, instruction, destination, pc, next_pc, target),
                    low(result),
                    64,
                    0);
        VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        enter();
        return true;
    }
    if (instruction.opcode == Op::CVTPS2PI || instruction.opcode == Op::CVTTPS2PI ||
        instruction.opcode == Op::CVTPD2PI || instruction.opcode == Op::CVTTPD2PI) {
        no_third();
        if (!MmxRegister(destination)) Unsupported();
        const bool packed_double =
                instruction.opcode == Op::CVTPD2PI || instruction.opcode == Op::CVTTPD2PI;
        const bool truncate =
                instruction.opcode == Op::CVTTPS2PI || instruction.opcode == Op::CVTTPD2PI;
        Value input;
        if (VRegister(source))
            input = VRead(builder, layout, instruction, source, pc, next_pc, target);
        else if (packed_double)
            input = VRead(builder, layout, instruction, source, pc, next_pc, target, true);
        else {
            if (!Memory(source) || source.size != 64) Unsupported();
            input = VLowScalar(
                    builder, ReadData(builder, layout, instruction, source, next_pc, target), 64);
        }
        const auto converted = Require(builder.VecFCvtPacked(
                Type::V128,
                {Operand::ValueOf(input),
                 Operand::Immediate((packed_double ? 4 : 2) + truncate, Type::U8)}));
        commit(low(converted));
        return true;
    }
    if (instruction.opcode == Op::PMOVMSKB || instruction.opcode == Op::PEXTRW) {
        if (!MmxRegister(source) || destination.type != GuestOperandKind::Register ||
            destination.size != 32 || VRegister(destination))
            Unsupported();
        const auto input = vector(source);
        Value result;
        if (instruction.opcode == Op::PMOVMSKB) {
            no_third();
            result = Require(builder.VecMovMask(
                    Type::U32, {Operand::ValueOf(input), Operand::Immediate(8, Type::U8)}));
        } else {
            if (instruction.ops[2].type != GuestOperandKind::Immediate) Unsupported();
            result = Require(builder.VecExtract16(
                    Type::U32,
                    {Operand::ValueOf(input),
                     Operand::Immediate(instruction.immediate & 3, Type::U8)}));
        }
        WriteRegister(builder, layout, destination, result);
        enter();
        return true;
    }
    if (instruction.opcode == Op::MASKMOVQ) {
        no_third();
        if (!MmxRegister(destination) || !MmxRegister(source)) Unsupported();
        builder.PublishRegisters();
        const auto data = raw(destination), mask = raw(source);
        auto memory_instruction = instruction;
        memory_instruction.disp = 0;
        memory_instruction.base = Register::None;
        memory_instruction.scale = 1;
        const DecodedOperand memory{
                GuestOperandKind::Memory,
                static_cast<std::uint8_t>(instruction.address_bits == 32 ? Register::EDI
                                                                         : Register::RDI),
                8};
        for (unsigned i = 0; i < 8; ++i) {
            const auto selected = Require(builder.TestBit(
                    Type::Bool, {Operand::ValueOf(mask), Operand::Immediate(i * 8 + 7, Type::U8)}));
            builder.IfNonZero(selected, [&] {
                PrepareFaultPC(builder, layout, pc, target);
                const auto address = Address(builder,
                                             layout,
                                             memory_instruction,
                                             memory,
                                             next_pc,
                                             target,
                                             {},
                                             nullptr,
                                             Type::U64,
                                             Constant(builder, i));
                const auto byte = Require(builder.BitExtract(Type::U8,
                                                             {Operand::ValueOf(data),
                                                              Operand::Immediate(i * 8, Type::U8),
                                                              Operand::Immediate(8, Type::U8)}));
                StoreMemory<OpCode::StoreMemory>(builder, address, byte, 8);
            });
        }
        enter();
        return true;
    }
    if (!MmxRegister(destination)) Unsupported();
    unsigned bits{}, group{}, sign{};
    bool high{}, subtract{}, arithmetic{}, shift_left{}, unary{};
    switch (instruction.opcode) {
        case Op::PADDB:
            bits = 8;
            group = 1;
            break;
        case Op::PADDW:
            bits = 16;
            group = 1;
            break;
        case Op::PADDD:
            bits = 32;
            group = 1;
            break;
        case Op::PADDQ:
            bits = 64;
            group = 1;
            break;
        case Op::PSUBB:
            bits = 8;
            group = 1;
            subtract = true;
            break;
        case Op::PSUBW:
            bits = 16;
            group = 1;
            subtract = true;
            break;
        case Op::PSUBD:
            bits = 32;
            group = 1;
            subtract = true;
            break;
        case Op::PSUBQ:
            bits = 64;
            group = 1;
            subtract = true;
            break;
        case Op::PXOR:
            group = 2;
            break;
        case Op::PAND:
            group = 3;
            break;
        case Op::PANDN:
            group = 4;
            break;
        case Op::POR:
            group = 5;
            break;
        case Op::PCMPEQB:
            bits = 8;
            group = 6;
            break;
        case Op::PCMPEQW:
            bits = 16;
            group = 6;
            break;
        case Op::PCMPEQD:
            bits = 32;
            group = 6;
            break;
        case Op::PCMPGTB:
            bits = 8;
            group = 7;
            break;
        case Op::PCMPGTW:
            bits = 16;
            group = 7;
            break;
        case Op::PCMPGTD:
            bits = 32;
            group = 7;
            break;
        case Op::PADDSB:
            bits = 8;
            sign = 1;
            group = 8;
            break;
        case Op::PADDSW:
            bits = 16;
            sign = 1;
            group = 8;
            break;
        case Op::PADDUSB:
            bits = 8;
            group = 8;
            break;
        case Op::PADDUSW:
            bits = 16;
            group = 8;
            break;
        case Op::PSUBSB:
            bits = 8;
            sign = 1;
            group = 8;
            subtract = true;
            break;
        case Op::PSUBSW:
            bits = 16;
            sign = 1;
            group = 8;
            subtract = true;
            break;
        case Op::PSUBUSB:
            bits = 8;
            group = 8;
            subtract = true;
            break;
        case Op::PSUBUSW:
            bits = 16;
            group = 8;
            subtract = true;
            break;
        case Op::PAVGB:
            bits = 8;
            group = 9;
            break;
        case Op::PAVGW:
            bits = 16;
            group = 9;
            break;
        case Op::PMINUB:
            bits = 8;
            group = 10;
            break;
        case Op::PMAXUB:
            bits = 8;
            group = 10;
            high = true;
            break;
        case Op::PMINSW:
            bits = 16;
            sign = 1;
            group = 10;
            break;
        case Op::PMAXSW:
            bits = 16;
            sign = 1;
            group = 10;
            high = true;
            break;
        case Op::PMULLW:
            group = 11;
            break;
        case Op::PMULHW:
            sign = 1;
            [[fallthrough]];
        case Op::PMULHUW:
            group = 12;
            break;
        case Op::PMULUDQ:
            group = 13;
            break;
        case Op::PMADDWD:
            group = 14;
            break;
        case Op::PSADBW:
            group = 15;
            break;
        case Op::PACKSSWB:
            bits = 16;
            group = 16;
            break;
        case Op::PACKUSWB:
            bits = 16;
            sign = 1;
            group = 16;
            break;
        case Op::PACKSSDW:
            bits = 32;
            group = 16;
            break;
        case Op::PUNPCKLBW:
            bits = 8;
            group = 17;
            break;
        case Op::PUNPCKLWD:
            bits = 16;
            group = 17;
            break;
        case Op::PUNPCKLDQ:
            bits = 32;
            group = 17;
            break;
        case Op::PUNPCKHBW:
            bits = 8;
            group = 17;
            high = true;
            break;
        case Op::PUNPCKHWD:
            bits = 16;
            group = 17;
            high = true;
            break;
        case Op::PUNPCKHDQ:
            bits = 32;
            group = 17;
            high = true;
            break;
        case Op::PSHUFW:
            group = 18;
            unary = true;
            break;
        case Op::PSHUFB:
            group = 19;
            break;
        case Op::PSLLW:
            bits = 16;
            group = 20;
            shift_left = true;
            break;
        case Op::PSLLD:
            bits = 32;
            group = 20;
            shift_left = true;
            break;
        case Op::PSLLQ:
            bits = 64;
            group = 20;
            shift_left = true;
            break;
        case Op::PSRLW:
            bits = 16;
            group = 20;
            break;
        case Op::PSRLD:
            bits = 32;
            group = 20;
            break;
        case Op::PSRLQ:
            bits = 64;
            group = 20;
            break;
        case Op::PSRAW:
            bits = 16;
            group = 20;
            arithmetic = true;
            break;
        case Op::PSRAD:
            bits = 32;
            group = 20;
            arithmetic = true;
            break;
        case Op::PINSRW:
            group = 21;
            break;
        case Op::PALIGNR:
            group = 22;
            break;
        case Op::PABSB:
            bits = 8;
            group = 23;
            unary = true;
            break;
        case Op::PABSW:
            bits = 16;
            group = 23;
            unary = true;
            break;
        case Op::PABSD:
            bits = 32;
            group = 23;
            unary = true;
            break;
        case Op::PSIGNB:
            bits = 8;
            group = 24;
            break;
        case Op::PSIGNW:
            bits = 16;
            group = 24;
            break;
        case Op::PSIGND:
            bits = 32;
            group = 24;
            break;
        case Op::PHADDW:
            bits = 16;
            group = 25;
            break;
        case Op::PHADDD:
            bits = 32;
            group = 25;
            break;
        case Op::PHSUBW:
            bits = 16;
            group = 25;
            subtract = true;
            break;
        case Op::PHSUBD:
            bits = 32;
            group = 25;
            subtract = true;
            break;
        case Op::PHADDSW:
            bits = 16;
            sign = 1;
            group = 25;
            break;
        case Op::PHSUBSW:
            bits = 16;
            sign = 1;
            group = 25;
            subtract = true;
            break;
        case Op::PMULHRSW:
            group = 26;
            break;
        case Op::PMADDUBSW:
            group = 27;
            break;
        default:
            Unsupported();
    }
    const auto immediate = static_cast<unsigned>(static_cast<std::uint8_t>(instruction.immediate));
    const bool control = group == 18 || group == 21 || group == 22;
    if (control ? instruction.ops[2].type != GuestOperandKind::Immediate
                : instruction.ops[2].type != GuestOperandKind::None)
        Unsupported();
    const auto previous = unary ? Value{} : vector(destination);
    const auto lane = Operand::Immediate(bits, Type::U8),
               signed_lane = Operand::Immediate(sign, Type::U8);
    Value result;
    if (group == 20) {
        if (source.type == GuestOperandKind::Immediate) {
            result = shift_left   ? Require(builder.VecShiftLeftImm(
                                          Type::V128,
                                          {Operand::ValueOf(previous),
                                             Operand::Immediate(immediate, Type::U8),
                                             lane}))
                     : arithmetic ? Require(builder.VecShiftRightArithmeticImm(
                                            Type::V128,
                                            {Operand::ValueOf(previous),
                                             Operand::Immediate(immediate, Type::U8),
                                             lane}))
                                  : Require(builder.VecShiftRightImm(
                                            Type::V128,
                                            {Operand::ValueOf(previous),
                                             Operand::Immediate(immediate, Type::U8),
                                             lane}));
        } else {
            const auto count = raw(source);
            result =
                    shift_left
                            ? Require(builder.VecShiftLeft(
                                      Type::V128,
                                      {Operand::ValueOf(previous), Operand::ValueOf(count), lane}))
                    : arithmetic
                            ? Require(builder.VecShiftRightArithmetic(
                                      Type::V128,
                                      {Operand::ValueOf(previous), Operand::ValueOf(count), lane}))
                            : Require(builder.VecShiftRight(
                                      Type::V128,
                                      {Operand::ValueOf(previous), Operand::ValueOf(count), lane}));
        }
    } else if (group == 21) {
        auto input = source;
        if (Memory(input))
            input.size = 16;
        else if (input.type != GuestOperandKind::Register || input.size != 32 ||
                 MmxRegister(input) || VRegister(input))
            Unsupported();
        const auto value = ReadData(builder, layout, instruction, input, next_pc, target);
        result = Require(builder.VecInsert16(Type::V128,
                                             {Operand::ValueOf(previous),
                                              Operand::ValueOf(value, Type::U16),
                                              Operand::Immediate(immediate & 3, Type::U8)}));
    } else {
        const auto input = vector(source);
        switch (group) {
            case 1:
                result = subtract ? Require(builder.VecSub(Type::V128,
                                                           {Operand::ValueOf(previous),
                                                            Operand::ValueOf(input),
                                                            lane}))
                                  : Require(builder.VecAdd(Type::V128,
                                                           {Operand::ValueOf(previous),
                                                            Operand::ValueOf(input),
                                                            lane}));
                break;
            case 2:
                result = Require(builder.VecXor(
                        Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input)}));
                break;
            case 3:
                result = Require(builder.VecAnd(
                        Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input)}));
                break;
            case 4:
                result = Require(builder.VecAndNot(
                        Type::V128, {Operand::ValueOf(input), Operand::ValueOf(previous)}));
                break;
            case 5:
                result = Require(builder.VecOr(
                        Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input)}));
                break;
            case 6:
                result = Require(builder.VecCmpEq(
                        Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input), lane}));
                break;
            case 7:
                result = Require(builder.VecCmpGt(
                        Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input), lane}));
                break;
            case 8:
                result = subtract ? Require(builder.VecSatSub(Type::V128,
                                                              {Operand::ValueOf(previous),
                                                               Operand::ValueOf(input),
                                                               lane,
                                                               signed_lane}))
                                  : Require(builder.VecSatAdd(Type::V128,
                                                              {Operand::ValueOf(previous),
                                                               Operand::ValueOf(input),
                                                               lane,
                                                               signed_lane}));
                break;
            case 9:
                result = Require(builder.VecAvg(
                        Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input), lane}));
                break;
            case 10:
                result = high ? Require(builder.VecMax(Type::V128,
                                                       {Operand::ValueOf(previous),
                                                        Operand::ValueOf(input),
                                                        lane,
                                                        signed_lane}))
                              : Require(builder.VecMin(Type::V128,
                                                       {Operand::ValueOf(previous),
                                                        Operand::ValueOf(input),
                                                        lane,
                                                        signed_lane}));
                break;
            case 11:
                result = Require(builder.VecMul(Type::V128,
                                                {Operand::ValueOf(previous),
                                                 Operand::ValueOf(input),
                                                 Operand::Immediate(16, Type::U8)}));
                break;
            case 12:
                result = Require(builder.VecMulHigh16(
                        Type::V128,
                        {Operand::ValueOf(previous), Operand::ValueOf(input), signed_lane}));
                break;
            case 13:
                result = Require(builder.VecMulWiden(Type::V128,
                                                     {Operand::ValueOf(previous),
                                                      Operand::ValueOf(input),
                                                      Operand::Immediate(32, Type::U8),
                                                      Operand::Immediate(0, Type::U8)}));
                break;
            case 14:
                result = Require(builder.VecMadd16(
                        Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input)}));
                break;
            case 15:
                result = Require(builder.VecAbsDiffSum8(
                        Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input)}));
                break;
            case 16: {
                const auto packed = Require(builder.VecPack(
                        Type::V128,
                        {Operand::ValueOf(previous), Operand::ValueOf(input), lane, signed_lane}));
                result = Require(builder.VecShuffle32(
                        Type::V128, {Operand::ValueOf(packed), Operand::Immediate(8, Type::U8)}));
                break;
            }
            case 17: {
                result = Require(builder.VecZip(Type::V128,
                                                {Operand::ValueOf(previous),
                                                 Operand::ValueOf(input),
                                                 lane,
                                                 Operand::Immediate(0, Type::U8)}));
                if (high)
                    result = Require(
                            builder.VecDup64(Type::V128,
                                             {Operand::ValueOf(Require(builder.VecExtract64(
                                                     Type::U64,
                                                     {Operand::ValueOf(result),
                                                      Operand::Immediate(1, Type::U8)})))}));
                break;
            }
            case 18:
                result = Require(builder.VecShuffle16(Type::V128,
                                                      {Operand::ValueOf(input),
                                                       Operand::Immediate(immediate, Type::U8),
                                                       Operand::Immediate(0, Type::U8)}));
                break;
            case 19: {
                const auto indices = Require(builder.VecAnd(
                        Type::V128,
                        {Operand::ValueOf(input),
                         Operand::ValueOf(VConstant(builder, UINT64_C(0x8787878787878787), 0))}));
                result = Require(builder.VecTableLookup8(
                        Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(indices)}));
                break;
            }
            case 22: {
                const auto joined = Require(builder.VecZip(Type::V128,
                                                           {Operand::ValueOf(input),
                                                            Operand::ValueOf(previous),
                                                            Operand::Immediate(64, Type::U8),
                                                            Operand::Immediate(0, Type::U8)}));
                result = immediate == 0 ? joined : Require(builder.VecSharedZero(Type::V128, {}));
                if (immediate && immediate < 16)
                    result = Require(builder.VecByteShift(Type::V128,
                                                          {Operand::ValueOf(joined),
                                                           Operand::ValueOf(result),
                                                           Operand::Immediate(immediate, Type::U8),
                                                           Operand::Immediate(0, Type::U8)}));
                break;
            }
            case 23:
            case 24: {
                const auto zeros = Require(builder.VecSharedZero(Type::V128, {}));
                const auto negative =
                        Require(builder.VecSub(Type::V128,
                                               {Operand::ValueOf(zeros),
                                                Operand::ValueOf(group == 23 ? input : previous),
                                                lane}));
                if (group == 23)
                    result = Require(builder.VecMax(Type::V128,
                                                    {Operand::ValueOf(input),
                                                     Operand::ValueOf(negative),
                                                     lane,
                                                     Operand::Immediate(1, Type::U8)}));
                else {
                    const auto negmask = Require(builder.VecCmpGt(
                            Type::V128, {Operand::ValueOf(zeros), Operand::ValueOf(input), lane}));
                    const auto posmask = Require(builder.VecCmpGt(
                            Type::V128, {Operand::ValueOf(input), Operand::ValueOf(zeros), lane}));
                    const auto neg = Require(builder.VecAnd(
                            Type::V128, {Operand::ValueOf(negative), Operand::ValueOf(negmask)}));
                    const auto pos = Require(builder.VecAnd(
                            Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(posmask)}));
                    result = Require(builder.VecOr(Type::V128,
                                                   {Operand::ValueOf(neg), Operand::ValueOf(pos)}));
                }
                break;
            }
            case 25: {
                const auto a = Require(builder.VecUnzip(Type::V128,
                                                        {Operand::ValueOf(previous),
                                                         Operand::ValueOf(input),
                                                         lane,
                                                         Operand::Immediate(0, Type::U8)}));
                const auto b = Require(builder.VecUnzip(Type::V128,
                                                        {Operand::ValueOf(previous),
                                                         Operand::ValueOf(input),
                                                         lane,
                                                         Operand::Immediate(1, Type::U8)}));
                if (sign)
                    result = subtract ? Require(builder.VecSatSub(Type::V128,
                                                                  {Operand::ValueOf(a),
                                                                   Operand::ValueOf(b),
                                                                   lane,
                                                                   signed_lane}))
                                      : Require(builder.VecSatAdd(Type::V128,
                                                                  {Operand::ValueOf(a),
                                                                   Operand::ValueOf(b),
                                                                   lane,
                                                                   signed_lane}));
                else
                    result = subtract ? Require(builder.VecSub(
                                                Type::V128,
                                                {Operand::ValueOf(a), Operand::ValueOf(b), lane}))
                                      : Require(builder.VecAdd(
                                                Type::V128,
                                                {Operand::ValueOf(a), Operand::ValueOf(b), lane}));
                result = Require(builder.VecShuffle32(
                        Type::V128, {Operand::ValueOf(result), Operand::Immediate(8, Type::U8)}));
                break;
            }
            case 26: {
                const auto product =
                        Require(builder.VecSignedMulWidenLow(Type::V128,
                                                             {Operand::ValueOf(previous),
                                                              Operand::ValueOf(input),
                                                              Operand::Immediate(16, Type::U8)}));
                const auto rounded = Require(builder.VecAdd(
                        Type::V128,
                        {Operand::ValueOf(product),
                         Operand::ValueOf(VConstant(
                                 builder, UINT64_C(0x400000004000), UINT64_C(0x400000004000))),
                         Operand::Immediate(32, Type::U8)}));
                const auto shifted = Require(
                        builder.VecShiftRightArithmeticImm(Type::V128,
                                                           {Operand::ValueOf(rounded),
                                                            Operand::Immediate(15, Type::U8),
                                                            Operand::Immediate(32, Type::U8)}));
                result = Require(builder.VecUnzip(Type::V128,
                                                  {Operand::ValueOf(shifted),
                                                   Operand::ValueOf(shifted),
                                                   Operand::Immediate(16, Type::U8),
                                                   Operand::Immediate(0, Type::U8)}));
                break;
            }
            case 27: {
                const auto a = Require(builder.VecExtend(Type::V128,
                                                         {Operand::ValueOf(previous),
                                                          Operand::Immediate(8, Type::U8),
                                                          Operand::Immediate(16, Type::U8),
                                                          Operand::Immediate(0, Type::U8)}));
                const auto b = Require(builder.VecExtend(Type::V128,
                                                         {Operand::ValueOf(input),
                                                          Operand::Immediate(8, Type::U8),
                                                          Operand::Immediate(16, Type::U8),
                                                          Operand::Immediate(1, Type::U8)}));
                const auto product = Require(builder.VecMul(Type::V128,
                                                            {Operand::ValueOf(a),
                                                             Operand::ValueOf(b),
                                                             Operand::Immediate(16, Type::U8)}));
                const auto even = Require(builder.VecUnzip(Type::V128,
                                                           {Operand::ValueOf(product),
                                                            Operand::ValueOf(product),
                                                            Operand::Immediate(16, Type::U8),
                                                            Operand::Immediate(0, Type::U8)}));
                const auto odd = Require(builder.VecUnzip(Type::V128,
                                                          {Operand::ValueOf(product),
                                                           Operand::ValueOf(product),
                                                           Operand::Immediate(16, Type::U8),
                                                           Operand::Immediate(1, Type::U8)}));
                result = Require(builder.VecSatAdd(Type::V128,
                                                   {Operand::ValueOf(even),
                                                    Operand::ValueOf(odd),
                                                    Operand::Immediate(16, Type::U8),
                                                    Operand::Immediate(1, Type::U8)}));
                break;
            }
        }
    }
    commit(low(result));
    return true;
}
