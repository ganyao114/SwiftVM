// Members of the single Semantics<Sink> template. No instruction stream or owner.
static Value Constant(Emitter& builder, u64 bits, Type type = Type::U64) {
    return Require(builder.template EmitImm<OpCode::LoadImm>(Type::Void, bits, type));
}

static Value Add(Emitter& builder, Value left, Operand right) {
    return Require(builder.Add(Type::Void, {Operand::ValueOf(left), right}));
}

static Type ScalarType(unsigned bits) {
    switch (bits) {
        case 8:
            return Type::U8;
        case 16:
            return Type::U16;
        case 32:
            return Type::U32;
        case 64:
            return Type::U64;
        default:
            Unsupported();
    }
}

static u32 RegisterOffset(StateLayout layout, unsigned reg, unsigned bits, unsigned& slot) {
    unsigned byte{};
    if (bits == 8) {
        if (reg >= Register::AL && reg <= Register::BL)
            slot = reg - Register::AL;
        else if (reg >= Register::AH && reg <= Register::BH) {
            slot = reg - Register::AH;
            byte = 1;
        } else if (reg >= Register::R8B && reg <= Register::R15B)
            slot = 8 + reg - Register::R8B;
        else if (reg >= Register::SPL && reg <= Register::DIL)
            slot = 4 + reg - Register::SPL;
        else
            Unsupported();
    } else {
        const unsigned base = bits == 64   ? Register::RAX
                              : bits == 32 ? Register::EAX
                                           : Register::AX;
        if ((bits != 16 && bits != 32 && bits != 64) || reg < base || reg - base >= 16)
            Unsupported();
        slot = reg - base;
    }
    return layout.gprs_offset + slot * layout.stride + byte;
}

static Value ReadRegister(Emitter& builder, StateLayout layout, unsigned reg, unsigned bits) {
    unsigned slot{};
    const auto offset = RegisterOffset(layout, reg, bits, slot);
    return builder.ReadGpr(offset, slot, bits);
}

static void WriteRegister(Emitter& builder,
                          StateLayout layout,
                          const DecodedOperand& reg,
                          Value value) {
    unsigned slot{};
    const auto offset = RegisterOffset(layout, reg.index, reg.size, slot);
    builder.WriteGpr(offset, slot, reg.size, value);
}

static bool Memory(const DecodedOperand& operand) {
    return operand.type == GuestOperandKind::Memory ||
           operand.type == GuestOperandKind::IndexedMemory ||
           operand.type == GuestOperandKind::Displacement;
}

// A one-shot memory consumer can borrow the final displacement instead of
// birthing an Add. Absolute/RIP addresses and ordinary Value users stay whole.
// Only LEA32 requests U32: its arithmetic and register reads need low32, while
// real memory, TLS and stack addresses continue to use the default U64 path.
static Value Address(Emitter& builder,
                     StateLayout layout,
                     const InstructionData& instruction,
                     const DecodedOperand& operand,
                     u64 next_pc,
                     TargetInfo target,
                     Value stack_override = {},
                     u64* final_disp = nullptr,
                     Type result_type = Type::U64,
                     Value extra_offset = {}) {
    if (final_disp) *final_disp = 0;
    const bool address32 = instruction.address_bits == 32;
    const bool narrow = result_type == Type::U32 || address32;
    const auto address_type = narrow ? Type::U32 : Type::U64;
    const auto use = [&](Value value) {
        return Operand::ValueOf(value, narrow ? Type::U32 : Type::Void);
    };
    const auto literal = [&](u64 bits) {
        return Constant(builder, narrow ? static_cast<u32>(bits) : bits, address_type);
    };
    const auto add = [&](Value left, Operand right) {
        return narrow ? Require(builder.Add(Type::U32, {use(left), right}))
                      : Add(builder, left, right);
    };
    const auto read = [&](unsigned reg) {
        if ((reg == Register::RSP || reg == Register::ESP) && stack_override) return stack_override;
        if (!narrow) return ReadRegister(builder, layout, reg, 64);
        unsigned slot{};
        (void)RegisterOffset(layout, reg, address32 ? 32 : 64, slot);
        // A captured U32 parent already supplies low32, even when it came
        // from a mere read rather than a whole-parent architectural write.
        // Borrow either cached width with an explicit numeric U32 use;
        // do not widen it or create a separate truncation before an ALU.
        if (const auto parent = builder.ReadAddressGpr(slot)) return parent;
        return ReadRegister(builder, layout, Register::EAX + slot, 32);
    };
    const auto segment = instruction.opcode == Op::LEA ? Register::None : instruction.segment;
    const auto with_segment = [&](Value base) {
        if (segment != Register::FS && segment != Register::GS) return base;
        const auto offset = segment == Register::FS ? layout.fs_base_offset : layout.gs_base_offset;
        if (offset > UINT32_MAX - 7) Unsupported();
        const auto segment_base = Require(
                builder.template EmitUniform<OpCode::LoadUniform>(Type::U64, offset, Type::U64));
        if (!base) return segment_base;
        if (narrow)
            base = Require(builder.ZeroExtend64(Type::U64, {Operand::ValueOf(base, Type::U32)}));
        return Add(builder, base, Operand::ValueOf(segment_base));
    };
    const auto finish = [&](Value base) {
        const auto displacement = narrow ? static_cast<u32>(instruction.disp) : instruction.disp;
        if (extra_offset) base = base ? add(base, use(extra_offset)) : extra_offset;
        // Addr32 wraps the complete EA before a segment base is added. A
        // borrowed U64 displacement would skip that architectural wrap.
        if (address32) {
            if (!base)
                base = literal(displacement);
            else if (displacement)
                base = add(base, Operand::Immediate(displacement, Type::U32));
            else if (builder.NaturalType(base) != Type::U32)
                base = Require(builder.ZeroExtend32(Type::U32, {use(base)}));
            return with_segment(base);
        }
        base = with_segment(base);
        if (!base) return literal(displacement);
        if (final_disp) {
            *final_disp = instruction.disp;
            return base;
        }
        if (displacement) return add(base, Operand::Immediate(displacement, address_type));
        if (narrow && builder.NaturalType(base) != Type::U32)
            return Require(builder.ZeroExtend32(Type::U32, {use(base)}));
        return base;
    };
    if (operand.type == GuestOperandKind::Displacement) return finish({});
    if (operand.type == GuestOperandKind::Memory) {
        if (operand.index == Register::RIP)
            return with_segment(literal(next_pc + instruction.disp));
        return finish(read(operand.index));
    }
    if (operand.type != GuestOperandKind::IndexedMemory) Unsupported();
    Value address;
    if (instruction.base != Register::None) address = read(instruction.base);
    if (operand.index != Register::None) {
        auto index = read(operand.index);
        const auto scale = instruction.scale;
        if (scale > 1) {
            if (scale != 2 && scale != 4 && scale != 8) Unsupported();
            const auto shift = static_cast<std::uint8_t>(scale == 2 ? 1 : scale == 4 ? 2 : 3);
            if (address) {
                if (const auto combined =
                            builder.ShiftedAddress(target, address_type, address, index, shift))
                    return finish(combined);
            } else if (instruction.disp != 0 && !address32 && !extra_offset &&
                       segment != Register::FS && segment != Register::GS) {
                if (const auto base = builder.AddressLiteral(
                            target,
                            address_type,
                            narrow ? static_cast<u32>(instruction.disp) : instruction.disp)) {
                    if (const auto combined =
                                builder.ShiftedAddress(target, address_type, base, index, shift))
                        return combined;
                }
            }
            index = Require(builder.LslImm(narrow ? Type::U32 : Type::Void,
                                           {use(index), Operand::Immediate(shift, Type::U8)}));
        }
        address = address ? add(address, use(index)) : index;
    }
    return finish(address);
}

static u64 ImmediateBits(const InstructionData& instruction, unsigned bits) {
    const u64 value = instruction.immediate;
    return bits == 64 ? value : value & ((u64{1} << bits) - 1);
}

static Value LoadMemory(Emitter& builder, Type type, Value address, u64 displacement = 0) {
    if (displacement) {
        const bool negative_narrow = (displacement >> 63) && u64{} - displacement <= UINT32_MAX;
        return Require(builder.LoadMemoryTSO(
                type,
                {Operand::Expression(negative_narrow ? OperandOp::Sub : OperandOp::Add, 2),
                 Operand::ValueOf(address),
                 Operand::Immediate(negative_narrow ? u64{} - displacement : displacement,
                                    Type::U64)}));
    }
    return Require(builder.LoadMemoryTSO(type, {Operand::ValueOf(address)}));
}

static Value ReadData(Emitter& builder,
                      StateLayout layout,
                      const InstructionData& instruction,
                      const DecodedOperand& source,
                      u64 next_pc,
                      TargetInfo target,
                      bool materialize_address = false) {
    const auto type = ScalarType(source.size);
    if (source.type == GuestOperandKind::Register)
        return ReadRegister(builder, layout, source.index, source.size);
    if (!Memory(source)) Unsupported();
    u64 displacement{};
    const auto address = Address(builder,
                                 layout,
                                 instruction,
                                 source,
                                 next_pc,
                                 target,
                                 {},
                                 materialize_address ? nullptr : &displacement);
    return LoadMemory(builder, type, address, displacement);
}

template <OpCode op = OpCode::StoreMemoryTSO> static void StoreMemory(
        Emitter& builder, Value address, Value value, unsigned bits, u64 displacement = 0) {
    const auto type = bits == 128 ? Type::V128 : ScalarType(bits);
    if (displacement) {
        const bool negative_narrow = (displacement >> 63) && u64{} - displacement <= UINT32_MAX;
        Require(builder.template Emit<op>(
                Type::Void,
                {Operand::Expression(negative_narrow ? OperandOp::Sub : OperandOp::Add, 2),
                 Operand::ValueOf(address),
                 Operand::Immediate(negative_narrow ? u64{} - displacement : displacement,
                                    Type::U64),
                 Operand::ValueOf(value, type)}));
    } else
        Require(builder.template Emit<op>(
                Type::Void, {Operand::ValueOf(address), Operand::ValueOf(value, type)}));
}

static void OneOperand(const InstructionData& instruction) {
    if (instruction.ops[1].type != GuestOperandKind::None ||
        instruction.ops[2].type != GuestOperandKind::None ||
        instruction.ops[3].type != GuestOperandKind::None)
        Unsupported();
}

static bool LockedMemoryForm(const InstructionData& instruction) {
    if (instruction.opcode == Op::XCHG)
        return Memory(instruction.ops[0]) || Memory(instruction.ops[1]);
    if (!Memory(instruction.ops[0])) return false;
    // Prefix permission only. Each emitter checks its actual operand shape
    // and width once, along the same path used without LOCK.
    switch (instruction.opcode) {
        case Op::CMPXCHG8B:
        case Op::CMPXCHG16B:
        case Op::CMPXCHG:
        case Op::XADD:
        case Op::INC:
        case Op::DEC:
        case Op::NEG:
        case Op::NOT:
        case Op::BTS:
        case Op::BTR:
        case Op::BTC:
        case Op::ADD:
        case Op::SUB:
        case Op::ADC:
        case Op::SBB:
        case Op::AND:
        case Op::OR:
        case Op::XOR:
            return true;
        default:
            return false;
    }
}

// Prefix permission is an opcode/form property, not a second semantic pass.
static unsigned MandatoryRepeatPrefixes(Op opcode) {
    switch (opcode) {
        case Op::STOS:
        case Op::MOVS:
        case Op::LODS:
        case Op::CMPS:
        case Op::SCAS:
            return 3;
        case Op::MOVQ2DQ:
        case Op::MOVDQU:
        case Op::MOVQ:
        case Op::POPCNT:
        case Op::TZCNT:
        case Op::PAUSE:
        case Op::RDFSBASE:
        case Op::RDGSBASE:
        case Op::WRFSBASE:
        case Op::WRGSBASE:
        case Op::LZCNT:
        case Op::ADOX:
        case Op::CVTDQ2PD:
        case Op::MOVSS:
        case Op::ADDSS:
        case Op::SUBSS:
        case Op::MULSS:
        case Op::DIVSS:
        case Op::MINSS:
        case Op::MAXSS:
        case Op::SQRTSS:
        case Op::RCPSS:
        case Op::RSQRTSS:
        case Op::CVTSI2SS:
        case Op::CVTSS2SI:
        case Op::CVTTSS2SI:
        case Op::MOVSLDUP:
        case Op::MOVSHDUP:
        case Op::CVTTPS2DQ:
        case Op::CVTSS2SD:
        case Op::PSHUFHW:
        case Op::CMPEQSS:
        case Op::CMPLTSS:
        case Op::CMPLESS:
        case Op::CMPUNORDSS:
        case Op::CMPNEQSS:
        case Op::CMPNLTSS:
        case Op::CMPNLESS:
        case Op::CMPORDSS:
            return 2;
        case Op::MOVDQ2Q:
        case Op::PSHUFLW:
        case Op::CRC32:
        case Op::MOVSD:
        case Op::CVTSI2SD:
        case Op::ADDSD:
        case Op::SUBSD:
        case Op::MULSD:
        case Op::DIVSD:
        case Op::CVTTSD2SI:
        case Op::CVTPD2DQ:
        case Op::CVTSD2SS:
        case Op::CVTSD2SI:
        case Op::MOVDDUP:
        case Op::ADDSUBPS:
        case Op::HADDPS:
        case Op::HSUBPS:
        case Op::LDDQU:
        case Op::SQRTSD:
        case Op::MINSD:
        case Op::MAXSD:
        case Op::CMPEQSD:
        case Op::CMPLTSD:
        case Op::CMPLESD:
        case Op::CMPUNORDSD:
        case Op::CMPNEQSD:
        case Op::CMPNLTSD:
        case Op::CMPNLESD:
        case Op::CMPORDSD:
            return 1;
        default:
            return 0;
    }
}

static bool AllowedPrefixes(const InstructionData& instruction,
                            std::span<const std::uint8_t> bytes) {
    if (instruction.opcode == Op::NOP && bytes[0] == 0xf3 &&
        instruction.size == DisabledCetHintSize(bytes))
        return true;  // Exact disabled-CET architectural hints only.
    const auto repeat_prefixes = MandatoryRepeatPrefixes(instruction.opcode);
    const bool mandatory_f3 = (repeat_prefixes & 2) != 0, mandatory_f2 = (repeat_prefixes & 1) != 0;
    // In long mode 67 selects a wrapping 32-bit EA/count. Implicit stack
    // accesses retain their 64-bit stack address independently of this prefix.
    if (((instruction.flags & InstructionFlag::Locked) && !LockedMemoryForm(instruction)) ||
        ((instruction.flags & InstructionFlag::Repeat) && !mandatory_f3) ||
        ((instruction.flags & InstructionFlag::RepeatNotEqual) && !mandatory_f2) ||
        (instruction.address_bits != 64 && instruction.address_bits != 32))
        return false;
    for (const auto byte : bytes.first(instruction.size)) {
        if ((byte == 0xf0 && !LockedMemoryForm(instruction)) || (byte == 0xf2 && !mandatory_f2) ||
            (byte == 0xf3 && !mandatory_f3) || (byte == 0x67 && instruction.address_bits != 32))
            return false;
        if (byte == 0x26 || byte == 0x2e || byte == 0x36 || byte == 0x3e || byte == 0x64 ||
            byte == 0x65 || byte == 0x66 || byte == 0x67 || byte == 0xf0 || byte == 0xf2 ||
            byte == 0xf3 || (byte >= 0x40 && byte <= 0x4f))
            continue;
        if (byte == 0xc4 || byte == 0xc5)
            return instruction.vector_bytes != 0 || BmiMnemonic(instruction.opcode);
        return byte != 0x62;
    }
    return false;
}

static DecodedOperand Gpr(unsigned slot, unsigned bits) {
    const auto base = bits == 64   ? Register::RAX
                      : bits == 32 ? Register::EAX
                      : bits == 16 ? Register::AX
                                   : Register::AL;
    const auto reg = bits == 8 && slot >= 4 && slot < 8 ? Register::SPL + slot - 4 : base + slot;
    return {GuestOperandKind::Register,
            static_cast<std::uint8_t>(reg),
            static_cast<std::uint16_t>(bits)};
}
