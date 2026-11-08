// Members of the single Semantics<Sink> template. No instruction stream or owner.

static bool VRegister(const DecodedOperand& operand) {
    return operand.type == GuestOperandKind::Register && operand.index >= Register::XMM0 &&
           operand.index <= Register::XMM15;
}

static u32 VOffset(StateLayout layout, const DecodedOperand& operand) {
    if (!VRegister(operand) || layout.fprs_offset == UINT32_MAX ||
        layout.fprs_offset > UINT32_MAX - 255)
        Unsupported();
    return layout.fprs_offset + (operand.index - Register::XMM0) * 16;
}

static bool AlignedLiteralAddress(const InstructionData& instruction,
                                  const DecodedOperand& operand,
                                  u64 next_pc) {
    const auto segment = instruction.segment;
    if (segment == Register::FS || segment == Register::GS) return false;
    if (operand.type == GuestOperandKind::Displacement) return !(instruction.disp & 15);
    return operand.type == GuestOperandKind::Memory && operand.index == Register::RIP &&
           !((next_pc + instruction.disp) & 15);
}

static Value VRead(Emitter& builder,
                   StateLayout layout,
                   const InstructionData& instruction,
                   const DecodedOperand& operand,
                   u64 pc,
                   u64 next_pc,
                   TargetInfo target,
                   bool aligned = false) {
    if (VRegister(operand)) {
        const auto offset = VOffset(layout, operand);
        return builder.ReadXmm(offset, operand.index - Register::XMM0);
    }
    if (!Memory(operand) || operand.size != 128) Unsupported();
    PrepareFaultPC(builder, layout, pc, target);
    u64 displacement{};
    const auto address = Address(builder,
                                 layout,
                                 instruction,
                                 operand,
                                 next_pc,
                                 target,
                                 {},
                                 aligned ? nullptr : &displacement);
    if (aligned && !AlignedLiteralAddress(instruction, operand, next_pc))
        Require(builder.CheckMemoryAlignment(
                Type::Void, {Operand::ValueOf(address), Operand::Immediate(15, Type::U8)}));
    return LoadMemory(builder, Type::V128, address, displacement);
}

template <OpCode op = OpCode::StoreMemoryTSO> static void VWrite(Emitter& builder,
                                                                 StateLayout layout,
                                                                 const InstructionData& instruction,
                                                                 const DecodedOperand& operand,
                                                                 Value value,
                                                                 u64 pc,
                                                                 u64 next_pc,
                                                                 TargetInfo target,
                                                                 bool aligned = false) {
    if (VRegister(operand)) {
        const auto offset = VOffset(layout, operand);
        builder.WriteXmm(offset, operand.index - Register::XMM0, value);
        return;
    }
    if (!Memory(operand) || operand.size != 128) Unsupported();
    PrepareFaultPC(builder, layout, pc, target);
    u64 displacement{};
    const auto address = Address(builder,
                                 layout,
                                 instruction,
                                 operand,
                                 next_pc,
                                 target,
                                 {},
                                 aligned ? nullptr : &displacement);
    if (aligned && !AlignedLiteralAddress(instruction, operand, next_pc))
        Require(builder.CheckMemoryAlignment(
                Type::Void, {Operand::ValueOf(address), Operand::Immediate(15, Type::U8)}));
    StoreMemory<op>(builder, address, value, 128, displacement);
}

static Value VScalarRead(Emitter& builder,
                         StateLayout layout,
                         const InstructionData& instruction,
                         const DecodedOperand& operand,
                         unsigned bits,
                         u64 pc,
                         u64 next_pc,
                         TargetInfo target) {
    if (VRegister(operand)) {
        if (bits != 32 && bits != 64) Unsupported();
        const auto source = VRead(builder, layout, instruction, operand, pc, next_pc, target);
        const auto low = Require(builder.VecExtract64(
                Type::U64, {Operand::ValueOf(source), Operand::Immediate(0, Type::U8)}));
        if (bits == 32)
            return Require(builder.ZeroExtend32(Type::U32, {Operand::ValueOf(low, Type::U32)}));
        return low;
    }
    if (operand.size != bits) Unsupported();
    if (operand.type == GuestOperandKind::Register)
        return ReadRegister(builder, layout, operand.index, bits);
    if (!Memory(operand)) Unsupported();
    PrepareFaultPC(builder, layout, pc, target);
    return ReadData(builder, layout, instruction, operand, next_pc, target);
}

static Value VLowScalar(Emitter& builder, Value scalar, unsigned bits) {
    if (bits == 32)
        scalar = Require(builder.ZeroExtend64(Type::U64, {Operand::ValueOf(scalar, Type::U32)}));
    const auto duplicate = Require(builder.VecDup64(Type::V128, {Operand::ValueOf(scalar)}));
    const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
    // Explicitly clear high64, without changing raw BitCast semantics.
    return Require(builder.VecByteShift(Type::V128,
                                        {Operand::ValueOf(duplicate),
                                         Operand::ValueOf(zero),
                                         Operand::Immediate(8, Type::U8),
                                         Operand::Immediate(0, Type::U8)}));
}

static void ScalarDouble(Emitter& builder,
                         StateLayout layout,
                         const InstructionData& instruction,
                         PendingFlags& pending,
                         u64 pc,
                         u64 next_pc,
                         TargetInfo target) {
    if (instruction.ops[2].type != GuestOperandKind::None ||
        instruction.ops[3].type != GuestOperandKind::None)
        Unsupported();
    const auto destination = instruction.ops[0], source = instruction.ops[1];
    if (instruction.opcode == Op::PTEST) {
        if (!VRegister(destination) || (!VRegister(source) && !Memory(source))) Unsupported();
        if (Memory(source)) pending.Flush(builder);
        const auto a = VRead(builder, layout, instruction, destination, pc, next_pc, target);
        const auto b = VRead(builder, layout, instruction, source, pc, next_pc, target, true);
        const auto zero = [&](Value value) {
            const auto low = VLane(builder, value, 64, 0), high = VLane(builder, value, 64, 1);
            const auto combined =
                    Require(builder.Or(Type::U64, {Operand::ValueOf(low), Operand::ValueOf(high)}));
            return Require(builder.TestZero(Type::Bool, {Operand::ValueOf(combined)}));
        };
        const auto both =
                Require(builder.VecAnd(Type::V128, {Operand::ValueOf(a), Operand::ValueOf(b)}));
        const auto different =
                Require(builder.VecAndNot(Type::V128, {Operand::ValueOf(b), Operand::ValueOf(a)}));
        const auto carry = zero(different), z = zero(both);
        const auto packed = Require(builder.BitInsert(Type::U32,
                                                      {Operand::ValueOf(carry, Type::U32),
                                                       Operand::ValueOf(z, Type::U32),
                                                       Operand::Immediate(2, Type::U8),
                                                       Operand::Immediate(1, Type::U8)}));
        Require(builder.PublishFCmpFlags(Type::Void, {Operand::ValueOf(packed)}));
        pending = {};
        return;
    }
    if (instruction.opcode == Op::MASKMOVDQU) {
        if (!VRegister(destination) || !VRegister(source)) Unsupported();
        pending.Flush(builder);
        // Publish before the first conditional body. A skipped byte must
        // never make the emitter forget an earlier architectural write.
        builder.PublishRegisters();
        const auto data = VRead(builder, layout, instruction, destination, pc, next_pc, target);
        const auto mask = VRead(builder, layout, instruction, source, pc, next_pc, target);
        auto memory_instruction = instruction;
        memory_instruction.disp = 0;
        memory_instruction.base = Register::None;
        memory_instruction.scale = 1;
        const DecodedOperand memory{
                GuestOperandKind::Memory,
                static_cast<std::uint8_t>(instruction.address_bits == 32 ? Register::EDI
                                                                         : Register::RDI),
                8};
        const auto address = Address(builder, layout, memory_instruction, memory, next_pc, target);
        for (unsigned i = 0; i < 16; ++i) {
            const auto mask_byte = VLane(builder, mask, 8, i);
            const auto selected = Require(builder.TestBit(
                    Type::Bool,
                    {Operand::ValueOf(mask_byte, Type::U8), Operand::Immediate(7, Type::U8)}));
            builder.IfNonZero(selected, [&] {
                PrepareFaultPC(builder, layout, pc, target);
                StoreMemory<OpCode::StoreMemory>(
                        builder, address, VLane(builder, data, 8, i), 8, i);
            });
        }
        return;
    }
    const bool single = instruction.opcode == Op::MOVSS || instruction.opcode == Op::CVTSI2SS ||
                        instruction.opcode == Op::CVTTSS2SI || instruction.opcode == Op::CVTSS2SI ||
                        instruction.opcode == Op::UCOMISS || instruction.opcode == Op::COMISS;
    const unsigned float_bits = single ? 32 : 64;
    if (instruction.opcode == Op::MOVSD || instruction.opcode == Op::MOVSS) {
        if (VRegister(destination)) {
            if (!VRegister(source) && (!Memory(source) || source.size != float_bits)) Unsupported();
            if (VRegister(source) && source.index == destination.index) return;
            if (Memory(source)) pending.Flush(builder);
            const auto low = VScalarRead(
                    builder, layout, instruction, source, float_bits, pc, next_pc, target);
            const auto result = VRegister(source) ? VReplaceLane(builder,
                                                                 VRead(builder,
                                                                       layout,
                                                                       instruction,
                                                                       destination,
                                                                       pc,
                                                                       next_pc,
                                                                       target),
                                                                 low,
                                                                 float_bits,
                                                                 0)
                                                  : VLowScalar(builder, low, float_bits);
            VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        } else {
            if (!Memory(destination) || destination.size != float_bits || !VRegister(source))
                Unsupported();
            pending.Flush(builder);
            PrepareFaultPC(builder, layout, pc, target);
            const auto low = VScalarRead(
                    builder, layout, instruction, source, float_bits, pc, next_pc, target);
            u64 displacement{};
            const auto address = Address(
                    builder, layout, instruction, destination, next_pc, target, {}, &displacement);
            StoreMemory(builder, address, low, float_bits, displacement);
        }
        return;
    }
    const bool to_float = instruction.opcode == Op::CVTSI2SD || instruction.opcode == Op::CVTSI2SS;
    const bool from_float =
            instruction.opcode == Op::CVTTSD2SI || instruction.opcode == Op::CVTSD2SI ||
            instruction.opcode == Op::CVTTSS2SI || instruction.opcode == Op::CVTSS2SI;
    if (to_float || from_float) {
        const unsigned bits = to_float ? source.size : destination.size;
        if (bits != 16 && bits != 32 && bits != 64) Unsupported();
        if (Memory(source)) {
            pending.Flush(builder);
            PrepareFaultPC(builder, layout, pc, target);
        }
        if (to_float) {
            if (!VRegister(destination) ||
                (source.type != GuestOperandKind::Register && !Memory(source)) || VRegister(source))
                Unsupported();
            const auto input = ReadData(builder, layout, instruction, source, next_pc, target);
            const auto previous =
                    VRead(builder, layout, instruction, destination, pc, next_pc, target);
            const auto converted =
                    Require(builder.VecFCvtIntToFloat(ScalarType(float_bits),
                                                      {Operand::ValueOf(input, ScalarType(bits)),
                                                       Operand::Immediate(bits, Type::U8),
                                                       Operand::Immediate(float_bits, Type::U8)}));
            VWrite(builder,
                   layout,
                   instruction,
                   destination,
                   VReplaceLane(builder, previous, converted, float_bits, 0),
                   pc,
                   next_pc,
                   target);
        } else {
            if (destination.type != GuestOperandKind::Register || VRegister(destination) ||
                (!VRegister(source) && (!Memory(source) || source.size != float_bits)))
                Unsupported();
            const auto input = VScalarRead(
                    builder, layout, instruction, source, float_bits, pc, next_pc, target);
            const bool truncate =
                    instruction.opcode == Op::CVTTSD2SI || instruction.opcode == Op::CVTTSS2SI;
            const auto converted = Require(
                    builder.VecFCvtFloatToInt(ScalarType(bits),
                                              {Operand::ValueOf(input, ScalarType(float_bits)),
                                               Operand::Immediate(float_bits, Type::U8),
                                               Operand::Immediate(bits, Type::U8),
                                               Operand::Immediate(truncate, Type::U8)}));
            WriteRegister(builder, layout, destination, converted);
        }
        return;
    }
    if (!VRegister(destination) ||
        (!VRegister(source) && (!Memory(source) || source.size != float_bits)))
        Unsupported();
    if (Memory(source)) pending.Flush(builder);
    const auto left = VRead(builder, layout, instruction, destination, pc, next_pc, target);
    const auto right = VRegister(source)
                               ? VRead(builder, layout, instruction, source, pc, next_pc, target)
                               : Require(builder.VecDup64(Type::V128,
                                                          {Operand::ValueOf(VScalarRead(builder,
                                                                                        layout,
                                                                                        instruction,
                                                                                        source,
                                                                                        float_bits,
                                                                                        pc,
                                                                                        next_pc,
                                                                                        target))}));
    Value result;
    switch (instruction.opcode) {
        case Op::ADDSD:
            result = Require(builder.VecFAddScalar64(
                    Type::V128, {Operand::ValueOf(left), Operand::ValueOf(right)}));
            break;
        case Op::SUBSD:
            result = Require(builder.VecFSubScalar64(
                    Type::V128, {Operand::ValueOf(left), Operand::ValueOf(right)}));
            break;
        case Op::MULSD:
            result = Require(builder.VecFMulScalar64(
                    Type::V128, {Operand::ValueOf(left), Operand::ValueOf(right)}));
            break;
        case Op::DIVSD:
            result = Require(builder.VecFDivScalar64(
                    Type::V128, {Operand::ValueOf(left), Operand::ValueOf(right)}));
            break;
        case Op::COMISD:
        case Op::UCOMISD:
        case Op::COMISS:
        case Op::UCOMISS: {
            const auto compared = Require(builder.VecFCmp(
                    Type::U32,
                    {Operand::ValueOf(left),
                     Operand::ValueOf(right),
                     Operand::Immediate(float_bits | ((instruction.opcode == Op::COMISS ||
                                                       instruction.opcode == Op::COMISD)
                                                              ? 128
                                                              : 0),
                                        Type::U8)}));
            Require(builder.PublishFCmpFlags(Type::Void, {Operand::ValueOf(compared)}));
            pending = {};
            return;
        }
        default:
            Unsupported();
    }
    VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
}

static bool Vector(Emitter& builder,
                   StateLayout layout,
                   const InstructionData& instruction,
                   u64 pc,
                   u64 next_pc,
                   TargetInfo target) {
    unsigned source_bits{}, destination_bits{}, shift_bits{}, relation{UINT32_MAX},
            relation_bits{64};
    bool sign{}, shift_left{}, shift_arithmetic{};
    switch (instruction.opcode) {
        case Op::MOVUPS:
        case Op::MOVAPS:
        case Op::MOVUPD:
        case Op::MOVAPD:
        case Op::MOVDQU:
        case Op::MOVDQA:
        case Op::MOVD:
        case Op::MOVQ:
        case Op::MOVLPD:
        case Op::MOVHPD:
        case Op::MOVLPS:
        case Op::MOVHPS:
        case Op::MOVHLPS:
        case Op::MOVLHPS:
        case Op::PXOR:
        case Op::XORPS:
        case Op::XORPD:
        case Op::PAND:
        case Op::ANDPD:
        case Op::ANDPS:
        case Op::PANDN:
        case Op::ANDNPD:
        case Op::ANDNPS:
        case Op::POR:
        case Op::ORPD:
        case Op::ORPS:
        case Op::CVTTPD2DQ:
        case Op::CVTDQ2PD:
        case Op::PADDB:
        case Op::PADDW:
        case Op::PADDD:
        case Op::PADDQ:
        case Op::PSUBB:
        case Op::PSUBW:
        case Op::PSUBD:
        case Op::PSUBQ:
        case Op::PSHUFD:
        case Op::PSHUFLW:
        case Op::SHUFPS:
        case Op::PALIGNR:
        case Op::PSLLDQ:
        case Op::PSRLDQ:
        case Op::PUNPCKLBW:
        case Op::PUNPCKLWD:
        case Op::PUNPCKLDQ:
        case Op::PUNPCKHBW:
        case Op::PUNPCKHWD:
        case Op::PUNPCKHDQ:
        case Op::PUNPCKLQDQ:
        case Op::PUNPCKHQDQ:
        case Op::PACKUSWB:
        case Op::ADDPS:
        case Op::ADDPD:
        case Op::SUBPS:
        case Op::SUBPD:
        case Op::MULPS:
        case Op::MULPD:
        case Op::DIVPS:
        case Op::DIVPD:
        case Op::PMULUDQ:
        case Op::PMULLW:
        case Op::PMULLD:
        case Op::PMADDWD:
        case Op::PMINUB:
        case Op::PCMPEQB:
        case Op::PCMPEQW:
        case Op::PCMPEQD:
        case Op::PCMPGTB:
        case Op::PCMPGTW:
        case Op::PCMPGTD:
        case Op::PMOVMSKB:
        case Op::MOVMSKPS:
        case Op::MOVMSKPD:
            break;
        // The legacy comparison encodings use these guest mnemonic aliases.
        // The neutral relation bits are less/equal/greater/unordered, not the
        // x86 comparison encoding or a host condition code.
        case Op::CMPEQPS:
            relation_bits = 32;
            [[fallthrough]];
        case Op::CMPEQPD:
            relation = 2;
            break;
        case Op::CMPLTPS:
            relation_bits = 32;
            [[fallthrough]];
        case Op::CMPLTPD:
            relation = 1;
            break;
        case Op::CMPLEPS:
            relation_bits = 32;
            [[fallthrough]];
        case Op::CMPLEPD:
            relation = 3;
            break;
        case Op::CMPUNORDPS:
            relation_bits = 32;
            [[fallthrough]];
        case Op::CMPUNORDPD:
            relation = 8;
            break;
        case Op::CMPNEQPS:
            relation_bits = 32;
            [[fallthrough]];
        case Op::CMPNEQPD:
            relation = 13;
            break;
        case Op::CMPNLTPS:
            relation_bits = 32;
            [[fallthrough]];
        case Op::CMPNLTPD:
            relation = 14;
            break;
        case Op::CMPNLEPS:
            relation_bits = 32;
            [[fallthrough]];
        case Op::CMPNLEPD:
            relation = 12;
            break;
        case Op::CMPORDPS:
            relation_bits = 32;
            [[fallthrough]];
        case Op::CMPORDPD:
            relation = 7;
            break;
        case Op::PSLLW:
            shift_left = true;
            [[fallthrough]];
        case Op::PSRLW:
            shift_bits = 16;
            break;
        case Op::PSLLD:
            shift_left = true;
            [[fallthrough]];
        case Op::PSRLD:
            shift_bits = 32;
            break;
        case Op::PSLLQ:
            shift_left = true;
            [[fallthrough]];
        case Op::PSRLQ:
            shift_bits = 64;
            break;
        case Op::PSRAD:
            shift_bits = 32;
            shift_arithmetic = true;
            break;
        case Op::PSRAW:
            shift_bits = 16;
            shift_arithmetic = true;
            break;
        case Op::PMOVSXBW:
            sign = true;
            [[fallthrough]];
        case Op::PMOVZXBW:
            source_bits = 8;
            destination_bits = 16;
            break;
        case Op::PMOVSXBD:
            sign = true;
            [[fallthrough]];
        case Op::PMOVZXBD:
            source_bits = 8;
            destination_bits = 32;
            break;
        case Op::PMOVSXBQ:
            sign = true;
            [[fallthrough]];
        case Op::PMOVZXBQ:
            source_bits = 8;
            destination_bits = 64;
            break;
        case Op::PMOVSXWD:
            sign = true;
            [[fallthrough]];
        case Op::PMOVZXWD:
            source_bits = 16;
            destination_bits = 32;
            break;
        case Op::PMOVSXWQ:
            sign = true;
            [[fallthrough]];
        case Op::PMOVZXWQ:
            source_bits = 16;
            destination_bits = 64;
            break;
        case Op::PMOVSXDQ:
            sign = true;
            [[fallthrough]];
        case Op::PMOVZXDQ:
            source_bits = 32;
            destination_bits = 64;
            break;
        default:
            return SseExtended(builder, layout, instruction, pc, next_pc, target) ||
                   SseFloating(builder, layout, instruction, pc, next_pc, target);
    }
    if (layout.fprs_offset == UINT32_MAX || instruction.ops[3].type != GuestOperandKind::None)
        Unsupported();
    const auto& destination = instruction.ops[0];
    const auto& source = instruction.ops[1];
    if (instruction.opcode == Op::MOVHLPS || instruction.opcode == Op::MOVLHPS) {
        if (!VRegister(destination) || !VRegister(source) ||
            instruction.ops[2].type != GuestOperandKind::None)
            Unsupported();
        const auto previous = VRead(builder, layout, instruction, destination, pc, next_pc, target);
        const auto input = VRead(builder, layout, instruction, source, pc, next_pc, target);
        const bool high_to_low = instruction.opcode == Op::MOVHLPS;
        // ZIP2(src,dst) gives {src.high,dst.high}; ZIP1(dst,src) gives
        // {dst.low,src.low}. Both inputs are complete snapshots before VWrite.
        const auto result =
                Require(builder.VecZip(Type::V128,
                                       {Operand::ValueOf(high_to_low ? input : previous),
                                        Operand::ValueOf(high_to_low ? previous : input),
                                        Operand::Immediate(64, Type::U8),
                                        Operand::Immediate(high_to_low, Type::U8)}));
        VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        return true;
    }
    if (instruction.opcode == Op::MOVLPD || instruction.opcode == Op::MOVHPD ||
        instruction.opcode == Op::MOVLPS || instruction.opcode == Op::MOVHPS) {
        if (instruction.ops[2].type != GuestOperandKind::None) Unsupported();
        const bool load = VRegister(destination);
        const auto& reg = load ? destination : source;
        const auto& memory = load ? source : destination;
        if (!VRegister(reg) || !Memory(memory) || memory.size != 64) Unsupported();
        const auto offset =
                VOffset(layout, reg) +
                (instruction.opcode == Op::MOVHPD || instruction.opcode == Op::MOVHPS ? 8 : 0);
        if (load) {
            PrepareFaultPC(builder, layout, pc, target);
            const auto value = ReadData(builder, layout, instruction, memory, next_pc, target);
            Require(builder.StoreUniform(
                    Type::Void, {Operand::Uniform(offset, Type::U64), Operand::ValueOf(value)}));
            builder.InvalidateXmm(reg.index - Register::XMM0);
        } else {
            PrepareFaultPC(builder, layout, pc, target);
            const auto value =
                    Require(builder.LoadUniform(Type::U64, {Operand::Uniform(offset, Type::U64)}));
            u64 displacement{};
            const auto address = Address(
                    builder, layout, instruction, memory, next_pc, target, {}, &displacement);
            StoreMemory(builder, address, value, 64, displacement);
        }
        return true;
    }
    if (source_bits) {
        if (!VRegister(destination) || destination.size != 128 ||
            instruction.ops[2].type != GuestOperandKind::None)
            Unsupported();
        Value value;
        if (VRegister(source))
            value = VRead(builder, layout, instruction, source, pc, next_pc, target);
        else {
            if (!Memory(source)) Unsupported();
            auto narrow = source;
            narrow.size = static_cast<std::uint16_t>(128 / destination_bits * source_bits);
            PrepareFaultPC(builder, layout, pc, target);
            const auto scalar = ReadData(builder, layout, instruction, narrow, next_pc, target);
            // VecExtend observes only the low 2/4/8 bytes, so the duplicated
            // upper half needs no additional clear or full-width memory read.
            value = Require(builder.VecDup64(Type::V128, {Operand::ValueOf(scalar)}));
        }
        const auto result =
                Require(builder.VecExtend(Type::V128,
                                          {Operand::ValueOf(value),
                                           Operand::Immediate(source_bits, Type::U8),
                                           Operand::Immediate(destination_bits, Type::U8),
                                           Operand::Immediate(sign, Type::U8)}));
        VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        return true;
    }
    if (instruction.opcode == Op::CVTTPD2DQ || instruction.opcode == Op::CVTDQ2PD) {
        if (!VRegister(destination) || destination.size != 128 ||
            instruction.ops[2].type != GuestOperandKind::None)
            Unsupported();
        Value input;
        if (VRegister(source)) {
            input = VRead(builder, layout, instruction, source, pc, next_pc, target);
        } else if (instruction.opcode == Op::CVTDQ2PD) {
            if (!Memory(source) || source.size != 64) Unsupported();
            // Only two signed dwords participate. Load their actual eight
            // bytes; the duplicate's other half is not observed by kind 1.
            const auto low =
                    VScalarRead(builder, layout, instruction, source, 64, pc, next_pc, target);
            input = Require(builder.VecDup64(Type::V128, {Operand::ValueOf(low)}));
        } else {
            if (!Memory(source) || source.size != 128) Unsupported();
            input = VRead(builder, layout, instruction, source, pc, next_pc, target, true);
        }
        const auto result = Require(builder.VecFCvtPacked(
                Type::V128,
                {Operand::ValueOf(input),
                 Operand::Immediate(instruction.opcode == Op::CVTTPD2DQ ? 5 : 1, Type::U8)}));
        VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        return true;
    }
    if (relation != UINT32_MAX) {
        if (!VRegister(destination) || destination.size != 128 || source.size != 128 ||
            instruction.ops[2].type != GuestOperandKind::None)
            Unsupported();
        const auto left = VRead(builder, layout, instruction, destination, pc, next_pc, target);
        const auto right = VRead(builder, layout, instruction, source, pc, next_pc, target, true);
        if (relation == 1 || relation == 3 || relation == 14 || relation == 12) relation |= 16;
        const auto result =
                Require(builder.VecFCmpMask(Type::V128,
                                            {Operand::ValueOf(left),
                                             Operand::ValueOf(right),
                                             Operand::Immediate(relation_bits, Type::U8),
                                             Operand::Immediate(relation, Type::U8),
                                             Operand::Immediate(0, Type::U8)}));
        VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        return true;
    }
    if (instruction.opcode == Op::PMOVMSKB || instruction.opcode == Op::MOVMSKPS ||
        instruction.opcode == Op::MOVMSKPD) {
        if (instruction.ops[2].type != GuestOperandKind::None ||
            destination.type != GuestOperandKind::Register || destination.size != 32 ||
            !VRegister(source) || source.size != 128)
            Unsupported();
        const auto value = VRead(builder, layout, instruction, source, pc, next_pc, target);
        const unsigned lane = instruction.opcode == Op::PMOVMSKB   ? 8
                              : instruction.opcode == Op::MOVMSKPS ? 32
                                                                   : 64;
        const auto mask = Require(builder.VecMovMask(
                Type::U32, {Operand::ValueOf(value), Operand::Immediate(lane, Type::U8)}));
        WriteRegister(builder, layout, destination, mask);
        return true;
    }
    if (instruction.opcode == Op::MOVD || instruction.opcode == Op::MOVQ) {
        if (instruction.ops[2].type != GuestOperandKind::None ||
            (!VRegister(destination) && !VRegister(source)))
            Unsupported();
        const unsigned bits =
                instruction.opcode == Op::MOVQ || destination.size == 64 || source.size == 64 ? 64
                                                                                              : 32;
        if (VRegister(destination)) {
            const auto scalar =
                    VScalarRead(builder, layout, instruction, source, bits, pc, next_pc, target);
            VWrite(builder,
                   layout,
                   instruction,
                   destination,
                   VLowScalar(builder, scalar, bits),
                   pc,
                   next_pc,
                   target);
        } else {
            const auto vector = VRead(builder, layout, instruction, source, pc, next_pc, target);
            const auto value = Require(builder.VecExtract64(
                    Type::U64, {Operand::ValueOf(vector), Operand::Immediate(0, Type::U8)}));
            if (destination.size != bits) Unsupported();
            if (destination.type == GuestOperandKind::Register)
                WriteRegister(builder, layout, destination, value);
            else {
                if (!Memory(destination)) Unsupported();
                PrepareFaultPC(builder, layout, pc, target);
                u64 displacement{};
                const auto address = Address(builder,
                                             layout,
                                             instruction,
                                             destination,
                                             next_pc,
                                             target,
                                             {},
                                             &displacement);
                StoreMemory(builder, address, value, bits, displacement);
            }
        }
        return true;
    }
    if (instruction.opcode == Op::MOVUPS || instruction.opcode == Op::MOVAPS ||
        instruction.opcode == Op::MOVUPD || instruction.opcode == Op::MOVAPD ||
        instruction.opcode == Op::MOVDQU || instruction.opcode == Op::MOVDQA) {
        if (instruction.ops[2].type != GuestOperandKind::None || destination.size != 128 ||
            source.size != 128 || (!VRegister(destination) && !VRegister(source)))
            Unsupported();
        const bool aligned = instruction.opcode == Op::MOVAPS || instruction.opcode == Op::MOVAPD ||
                             instruction.opcode == Op::MOVDQA;
        const auto value =
                VRead(builder, layout, instruction, source, pc, next_pc, target, aligned);
        VWrite(builder, layout, instruction, destination, value, pc, next_pc, target, aligned);
        return true;
    }
    if (!VRegister(destination) || destination.size != 128) Unsupported();
    if (instruction.opcode == Op::PALIGNR) {
        if (instruction.ops[2].type != GuestOperandKind::Immediate ||
            instruction.ops[2].size != 8 || source.size != 128 ||
            (!VRegister(source) && !Memory(source)))
            Unsupported();
        const auto count = static_cast<std::uint8_t>(instruction.immediate);
        Value low;
        // A memory source is still accessed when the count discards all its
        // bytes. Pure register sources need no such unused read.
        if (count < 16 || Memory(source))
            low = VRead(builder, layout, instruction, source, pc, next_pc, target, true);
        Value result;
        if (count >= 32)
            result = Require(builder.VecSharedZero(Type::V128, {}));
        else if (!count)
            result = low;
        else {
            const auto high = VRead(builder, layout, instruction, destination, pc, next_pc, target);
            if (count < 16)
                result = Require(builder.VecExtractBytes(Type::V128,
                                                         {Operand::ValueOf(low),
                                                          Operand::ValueOf(high),
                                                          Operand::Immediate(count, Type::U8)}));
            else if (count == 16)
                result = high;
            else {
                const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
                result = Require(builder.VecByteShift(Type::V128,
                                                      {Operand::ValueOf(high),
                                                       Operand::ValueOf(zero),
                                                       Operand::Immediate(count - 16, Type::U8),
                                                       Operand::Immediate(0, Type::U8)}));
            }
        }
        VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        return true;
    }
    if (instruction.opcode == Op::PSLLDQ || instruction.opcode == Op::PSRLDQ) {
        if (instruction.ops[2].type != GuestOperandKind::None ||
            source.type != GuestOperandKind::Immediate || source.size != 8)
            Unsupported();
        const auto count = static_cast<std::uint8_t>(instruction.immediate);
        if (!count) return true;
        const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
        auto result = zero;
        if (count < 16) {
            const auto value =
                    VRead(builder, layout, instruction, destination, pc, next_pc, target);
            result = Require(builder.VecByteShift(
                    Type::V128,
                    {Operand::ValueOf(value),
                     Operand::ValueOf(zero),
                     Operand::Immediate(count, Type::U8),
                     Operand::Immediate(instruction.opcode == Op::PSLLDQ, Type::U8)}));
        }
        VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        return true;
    }
    if (instruction.opcode == Op::SHUFPS) {
        if (instruction.ops[2].type != GuestOperandKind::Immediate ||
            instruction.ops[2].size != 8 || source.size != 128 ||
            (!VRegister(source) && !Memory(source)))
            Unsupported();
        const auto left = VRead(builder, layout, instruction, destination, pc, next_pc, target);
        const auto right = VRead(builder, layout, instruction, source, pc, next_pc, target, true);
        const auto result =
                static_cast<std::uint8_t>(instruction.immediate) == 0x88
                        ? Require(builder.VecUnzip(Type::V128,
                                                   {Operand::ValueOf(left),
                                                    Operand::ValueOf(right),
                                                    Operand::Immediate(32, Type::U8),
                                                    Operand::Immediate(0, Type::U8)}))
                        : Require(builder.VecShuffle32TwoSrc(
                                  Type::V128,
                                  {Operand::ValueOf(left),
                                   Operand::ValueOf(right),
                                   Operand::Immediate(
                                           static_cast<std::uint8_t>(instruction.immediate),
                                           Type::U8)}));
        VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        return true;
    }
    if (instruction.opcode == Op::PSHUFD || instruction.opcode == Op::PSHUFLW) {
        if (instruction.ops[2].type != GuestOperandKind::Immediate ||
            instruction.ops[2].size != 8 || source.size != 128)
            Unsupported();
        const auto value = VRead(builder, layout, instruction, source, pc, next_pc, target, true);
        const auto control =
                Operand::Immediate(static_cast<std::uint8_t>(instruction.immediate), Type::U8);
        const auto result =
                instruction.opcode == Op::PSHUFD
                        ? Require(builder.VecShuffle32(Type::V128,
                                                       {Operand::ValueOf(value), control}))
                        : Require(builder.VecShuffle16(Type::V128,
                                                       {Operand::ValueOf(value),
                                                        control,
                                                        Operand::Immediate(0, Type::U8)}));
        VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        return true;
    }
    if (instruction.ops[2].type != GuestOperandKind::None) Unsupported();
    Value result;
    if (shift_bits) {
        if (source.type != GuestOperandKind::Immediate) {
            if (!VRegister(source) && (!Memory(source) || source.size != 128)) Unsupported();
            const auto left = VRead(builder, layout, instruction, destination, pc, next_pc, target);
            const auto vector =
                    VRead(builder, layout, instruction, source, pc, next_pc, target, true);
            const auto count = Require(builder.VecExtract64(
                    Type::U64, {Operand::ValueOf(vector), Operand::Immediate(0, Type::U8)}));
            const auto bits = Operand::Immediate(shift_bits, Type::U8);
            result = shift_left ? Require(builder.VecShiftLeft(
                                          Type::V128,
                                          {Operand::ValueOf(left), Operand::ValueOf(count), bits}))
                     : shift_arithmetic
                             ? Require(builder.VecShiftRightArithmetic(
                                       Type::V128,
                                       {Operand::ValueOf(left), Operand::ValueOf(count), bits}))
                             : Require(builder.VecShiftRight(
                                       Type::V128,
                                       {Operand::ValueOf(left), Operand::ValueOf(count), bits}));
            VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
            return true;
        }
        if (source.size != 8) Unsupported();
        const auto count = static_cast<std::uint8_t>(instruction.immediate);
        if (!count) return true;
        if (!shift_arithmetic && count >= shift_bits)
            result = Require(builder.VecSharedZero(Type::V128, {}));
        else {
            const auto left = VRead(builder, layout, instruction, destination, pc, next_pc, target);
            const auto amount = Operand::Immediate(count, Type::U8),
                       bits = Operand::Immediate(shift_bits, Type::U8);
            result = shift_left ? Require(builder.VecShiftLeftImm(
                                          Type::V128, {Operand::ValueOf(left), amount, bits}))
                     : shift_arithmetic
                             ? Require(builder.VecShiftRightArithmeticImm(
                                       Type::V128, {Operand::ValueOf(left), amount, bits}))
                             : Require(builder.VecShiftRightImm(
                                       Type::V128, {Operand::ValueOf(left), amount, bits}));
        }
    } else {
        if (source.size != 128) Unsupported();
        const auto left = VRead(builder, layout, instruction, destination, pc, next_pc, target);
        // These legacy full-m128 computations require a 16-byte aligned EA.
        // VRead elides the check before birth for a proved literal address.
        const auto right = VRead(builder, layout, instruction, source, pc, next_pc, target, true);
        switch (instruction.opcode) {
            case Op::PXOR:
            case Op::XORPS:
            case Op::XORPD:
                result = Require(builder.VecXor(Type::V128,
                                                {Operand::ValueOf(left), Operand::ValueOf(right)}));
                break;
            case Op::PAND:
            case Op::ANDPD:
            case Op::ANDPS:
                result = Require(builder.VecAnd(Type::V128,
                                                {Operand::ValueOf(left), Operand::ValueOf(right)}));
                break;
            case Op::PANDN:
            case Op::ANDNPD:
            case Op::ANDNPS:
                // VecAndNot(a,b) = a & ~b; the legacy destination is inverted.
                result = Require(builder.VecAndNot(
                        Type::V128, {Operand::ValueOf(right), Operand::ValueOf(left)}));
                break;
            case Op::POR:
            case Op::ORPD:
            case Op::ORPS:
                result = Require(builder.VecOr(Type::V128,
                                               {Operand::ValueOf(left), Operand::ValueOf(right)}));
                break;
            case Op::PADDB:
            case Op::PADDW:
            case Op::PADDD:
            case Op::PADDQ:
                result = Require(
                        builder.VecAdd(Type::V128,
                                       {Operand::ValueOf(left),
                                        Operand::ValueOf(right),
                                        Operand::Immediate(instruction.opcode == Op::PADDB   ? 8
                                                           : instruction.opcode == Op::PADDW ? 16
                                                           : instruction.opcode == Op::PADDD ? 32
                                                                                             : 64,
                                                           Type::U8)}));
                break;
            case Op::PSUBB:
            case Op::PSUBW:
            case Op::PSUBD:
            case Op::PSUBQ:
                result = Require(
                        builder.VecSub(Type::V128,
                                       {Operand::ValueOf(left),
                                        Operand::ValueOf(right),
                                        Operand::Immediate(instruction.opcode == Op::PSUBB   ? 8
                                                           : instruction.opcode == Op::PSUBW ? 16
                                                           : instruction.opcode == Op::PSUBD ? 32
                                                                                             : 64,
                                                           Type::U8)}));
                break;
            case Op::PMULLW:
            case Op::PMULLD:
                result = Require(builder.VecMul(
                        Type::V128,
                        {Operand::ValueOf(left),
                         Operand::ValueOf(right),
                         Operand::Immediate(instruction.opcode == Op::PMULLW ? 16 : 32,
                                            Type::U8)}));
                break;
            case Op::PCMPEQB:
            case Op::PCMPEQW:
            case Op::PCMPEQD:
                result = Require(builder.VecCmpEq(
                        Type::V128,
                        {Operand::ValueOf(left),
                         Operand::ValueOf(right),
                         Operand::Immediate(instruction.opcode == Op::PCMPEQB   ? 8
                                            : instruction.opcode == Op::PCMPEQW ? 16
                                                                                : 32,
                                            Type::U8)}));
                break;
            case Op::PCMPGTB:
            case Op::PCMPGTW:
            case Op::PCMPGTD:
                result = Require(builder.VecCmpGt(
                        Type::V128,
                        {Operand::ValueOf(left),
                         Operand::ValueOf(right),
                         Operand::Immediate(instruction.opcode == Op::PCMPGTB   ? 8
                                            : instruction.opcode == Op::PCMPGTW ? 16
                                                                                : 32,
                                            Type::U8)}));
                break;
            case Op::PMULUDQ:
                result = Require(builder.VecMulWiden(Type::V128,
                                                     {Operand::ValueOf(left),
                                                      Operand::ValueOf(right),
                                                      Operand::Immediate(32, Type::U8),
                                                      Operand::Immediate(0, Type::U8)}));
                break;
            case Op::PMADDWD:
                result = Require(builder.VecMadd16(
                        Type::V128, {Operand::ValueOf(left), Operand::ValueOf(right)}));
                break;
            case Op::PMINUB:
                result = Require(builder.VecMin(Type::V128,
                                                {Operand::ValueOf(left),
                                                 Operand::ValueOf(right),
                                                 Operand::Immediate(8, Type::U8),
                                                 Operand::Immediate(0, Type::U8)}));
                break;
            case Op::PACKUSWB:
                result = Require(builder.VecPack(Type::V128,
                                                 {Operand::ValueOf(left),
                                                  Operand::ValueOf(right),
                                                  Operand::Immediate(16, Type::U8),
                                                  Operand::Immediate(1, Type::U8)}));
                break;
            case Op::PUNPCKLBW:
            case Op::PUNPCKLWD:
            case Op::PUNPCKLDQ:
            case Op::PUNPCKHBW:
            case Op::PUNPCKHWD:
            case Op::PUNPCKHDQ:
            case Op::PUNPCKLQDQ:
            case Op::PUNPCKHQDQ:
                result = Require(builder.VecZip(
                        Type::V128,
                        {Operand::ValueOf(left),
                         Operand::ValueOf(right),
                         Operand::Immediate(instruction.opcode == Op::PUNPCKLBW ||
                                                            instruction.opcode == Op::PUNPCKHBW
                                                    ? 8
                                            : instruction.opcode == Op::PUNPCKLWD ||
                                                            instruction.opcode == Op::PUNPCKHWD
                                                    ? 16
                                            : instruction.opcode == Op::PUNPCKLDQ ||
                                                            instruction.opcode == Op::PUNPCKHDQ
                                                    ? 32
                                                    : 64,
                                            Type::U8),
                         Operand::Immediate(instruction.opcode == Op::PUNPCKHBW ||
                                                            instruction.opcode == Op::PUNPCKHWD ||
                                                            instruction.opcode == Op::PUNPCKHDQ ||
                                                            instruction.opcode == Op::PUNPCKHQDQ
                                                    ? 1
                                                    : 0,
                                            Type::U8)}));
                break;
            case Op::ADDPS:
            case Op::ADDPD:
                result = Require(builder.VecFAdd(
                        Type::V128,
                        {Operand::ValueOf(left),
                         Operand::ValueOf(right),
                         Operand::Immediate(instruction.opcode == Op::ADDPS ? 32 : 64, Type::U8)}));
                break;
            case Op::SUBPS:
            case Op::SUBPD:
                result = Require(builder.VecFSub(
                        Type::V128,
                        {Operand::ValueOf(left),
                         Operand::ValueOf(right),
                         Operand::Immediate(instruction.opcode == Op::SUBPS ? 32 : 64, Type::U8)}));
                break;
            case Op::MULPS:
            case Op::MULPD:
                result = Require(builder.VecFMul(
                        Type::V128,
                        {Operand::ValueOf(left),
                         Operand::ValueOf(right),
                         Operand::Immediate(instruction.opcode == Op::MULPS ? 32 : 64, Type::U8)}));
                break;
            case Op::DIVPS:
            case Op::DIVPD:
                result = Require(builder.VecFDiv(
                        Type::V128,
                        {Operand::ValueOf(left),
                         Operand::ValueOf(right),
                         Operand::Immediate(instruction.opcode == Op::DIVPS ? 32 : 64, Type::U8)}));
                break;
            default:
                Unsupported();
        }
    }
    VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
    return true;
}
