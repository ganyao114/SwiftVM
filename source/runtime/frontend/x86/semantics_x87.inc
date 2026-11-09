// Members of the single Semantics<Sink> template. No instruction stream or owner.

static unsigned X87RegisterWindow(Emitter& builder,
                                  const StateLayout& layout,
                                  const InstructionData& instruction,
                                  u64 pc,
                                  TargetInfo target,
                                  InstructionRange following,
                                  std::span<const std::uint8_t> available) {
    const bool exchange = instruction.opcode == Op::FXCH;
    unsigned index{};
    if (exchange) {
        const auto& first = instruction.ops[0];
        if (first.type != GuestOperandKind::Register || first.index < Register::ST0 ||
            first.index > Register::ST7 || instruction.ops[1].type != GuestOperandKind::None)
            Unsupported();
        index = first.index - Register::ST0;
    } else {
        for (const auto& operand : instruction.ops)
            if (operand.type != GuestOperandKind::None) Unsupported();
    }
    // Only the two established register windows are fused. DirectSink keeps
    // one instruction; no memory or independent later failure is hidden here.
    const auto [command, consumed] =
            builder.X87Window(exchange ? X87Command::Exchange : X87Command::PushOne,
                              index,
                              instruction,
                              following,
                              available);
    PrepareFaultPC(builder, layout, pc, target);
    const auto raw = builder.X87Zero();
    const auto state = builder.X87Address(layout.x87_offset);
    const auto committed = X87CheckedCall(builder,
                                          layout,
                                          instruction,
                                          available,
                                          state,
                                          X87Encode(command, index),
                                          raw,
                                          {},
                                          consumed == 1);
    if (!committed) {
        const auto last = consumed > 1 ? following[consumed - 1].Record() : instruction;
        X87Metadata(builder, layout, last.addr, X87Opcode(last, available.subspan(last.addr - pc)));
    }
    return consumed;
}

static Value X87Query(Emitter& builder, const StateLayout& layout, unsigned field, Type type) {
    return Require(builder.template EmitUniform<OpCode::LoadUniform>(
            type, layout.x87_offset + field, type));
}

static void X87Field(Emitter& builder, u32 offset, Type type, Value value) {
    Require(builder.template EmitUniformValue<OpCode::StoreUniform>(
            Type::Void, offset, type, value, type));
}

static void X87Call(Emitter& builder, Value state, u64 command, Value raw) {
    Require(builder.X87Op(Type::U64,
                          {Operand::ValueOf(state),
                           Operand::Immediate(command, Type::U64),
                           Operand::ValueOf(raw)}));
}

static unsigned X87Opcode(const InstructionData& instruction, std::span<const std::uint8_t> bytes) {
    for (unsigned i = 0; i + 1 < instruction.size && i + 1 < bytes.size(); ++i)
        if (bytes[i] >= 0xd8 && bytes[i] <= 0xdf) return ((bytes[i] & 7) << 8) | bytes[i + 1];
    return 0;
}

static u64 X87MetadataEncoding(const StateLayout& layout,
                               unsigned opcode,
                               bool has_dp,
                               bool commit_success = false) {
    const auto relative = [&](u32 field) {
        const auto offset = static_cast<std::int64_t>(field) - layout.x87_offset;
        if (offset < INT16_MIN || offset > INT16_MAX) Unsupported();
        return static_cast<std::int16_t>(offset);
    };
    return runtime::x86::X87EncodeMetadata(relative(layout.x87_ip_offset),
                                           relative(layout.x87_dp_offset),
                                           relative(layout.x87_opcode_offset),
                                           opcode,
                                           has_dp,
                                           commit_success);
}

static bool X87CheckedCall(Emitter& builder,
                           const StateLayout& layout,
                           const InstructionData& instruction,
                           std::span<const std::uint8_t> bytes,
                           Value state,
                           u64 command,
                           Value raw,
                           Value address = {},
                           bool commit_success = false) {
    // WAIT can only succeed or deliver an older pending exception; it never
    // raises a new exception that would replace the architectural pointers.
    if (command == X87Encode(X87Command::Wait)) {
        X87Call(builder, state, command, raw);
        return false;
    }
    if (layout.x87_ip_offset == UINT32_MAX || layout.x87_dp_offset == UINT32_MAX ||
        layout.x87_opcode_offset == UINT32_MAX) {
        X87Call(builder, state, command, raw);
        return false;
    }
    const auto metadata = X87MetadataEncoding(
            layout, X87Opcode(instruction, bytes), static_cast<bool>(address), commit_success);
    Require(builder.template EmitX87<OpCode::X87Execute>(
            Type::U64, state, command, raw, address ? address : builder.X87Zero(), metadata));
    return commit_success;
}

static void X87Metadata(
        Emitter& builder, const StateLayout& layout, u64 pc, unsigned opcode, Value address = {}) {
    if (layout.x87_ip_offset != UINT32_MAX && layout.x87_dp_offset != UINT32_MAX &&
        layout.x87_opcode_offset != UINT32_MAX) {
        const auto metadata = X87MetadataEncoding(layout, opcode, static_cast<bool>(address));
        Require(builder.X87CommitMetadata(Type::Void,
                                          {Operand::ValueOf(builder.X87Address(layout.x87_offset)),
                                           Operand::Immediate(pc, Type::U64),
                                           Operand::ValueOf(address ? address : builder.X87Zero()),
                                           Operand::Immediate(metadata, Type::U64)}));
        return;
    }
    if (layout.x87_ip_offset != UINT32_MAX)
        X87Field(builder, layout.x87_ip_offset, Type::U64, Constant(builder, pc));
    if (address && layout.x87_dp_offset != UINT32_MAX)
        X87Field(builder, layout.x87_dp_offset, Type::U64, address);
    if (layout.x87_opcode_offset != UINT32_MAX) {
        X87Field(
                builder, layout.x87_opcode_offset, Type::U16, Constant(builder, opcode, Type::U16));
    }
}

[[gnu::noinline]] static unsigned X87Environment(Emitter& builder,
                                                 const StateLayout& layout,
                                                 const InstructionData& instruction,
                                                 u64 pc,
                                                 u64 next_pc,
                                                 TargetInfo target) {
    using State = runtime::x86::X87State;
    const auto& operand = instruction.ops[0];
    if (!Memory(operand)) Unsupported();
    OneOperand(instruction);
    if (layout.x87_ip_offset == UINT32_MAX || layout.x87_dp_offset == UINT32_MAX ||
        layout.x87_opcode_offset == UINT32_MAX)
        Unsupported();
    const bool small = instruction.operand_bits == 16;
    const unsigned environment_bytes = small ? 14 : 28;
    const bool load = instruction.opcode == Op::FLDENV || instruction.opcode == Op::FRSTOR;
    const bool registers = instruction.opcode == Op::FRSTOR || instruction.opcode == Op::FNSAVE ||
                           instruction.opcode == Op::FSAVE;
    PrepareFaultPC(builder, layout, pc, target);
    const auto address = Address(builder, layout, instruction, operand, next_pc, target);
    const auto state = builder.X87Address(layout.x87_offset);
    const auto zero = builder.X87Zero();
    const auto call = [&](X87Command command, unsigned index = 0, Value raw = {}) {
        X87Call(builder, state, X87Encode(command, index), raw ? raw : zero);
    };
    if (load || instruction.opcode == Op::FSTENV || instruction.opcode == Op::FSAVE)
        call(X87Command::Wait);
    if (load) {
        // Load the complete operand before committing control, tags or TOP.
        const auto control = LoadMemory(builder, Type::U16, address);
        const auto status = LoadMemory(builder, Type::U16, address, small ? 2 : 4);
        const auto tags = LoadMemory(builder, Type::U16, address, small ? 4 : 8);
        const auto ip = LoadMemory(builder, small ? Type::U16 : Type::U32, address, small ? 6 : 12);
        const auto opcode = small ? Constant(builder, 0, Type::U16)
                                  : LoadMemory(builder, Type::U16, address, 18);
        const auto dp =
                LoadMemory(builder, small ? Type::U16 : Type::U32, address, small ? 10 : 20);
        (void)LoadMemory(builder, Type::U16, address, small ? 8 : 16);
        (void)LoadMemory(builder, Type::U16, address, small ? 12 : 24);
        if (!small) (void)LoadMemory(builder, Type::U16, address, 26);
        std::array<Value, 8> significands{}, exponents{};
        if (registers)
            for (unsigned i = 0; i < 8; ++i) {
                significands[i] =
                        LoadMemory(builder, Type::U64, address, environment_bytes + i * 10);
                exponents[i] =
                        LoadMemory(builder, Type::U16, address, environment_bytes + i * 10 + 8);
            }
        X87Field(builder, layout.x87_offset + offsetof(State, control), Type::U16, control);
        X87Field(builder, layout.x87_offset + offsetof(State, status), Type::U16, status);
        X87Field(builder, layout.x87_offset + offsetof(State, tags), Type::U16, tags);
        X87Field(builder, layout.x87_ip_offset, Type::U64, ip);
        X87Field(builder, layout.x87_dp_offset, Type::U64, dp);
        X87Field(builder, layout.x87_opcode_offset, Type::U16, opcode);
        if (registers)
            for (unsigned i = 0; i < 8; ++i) {
                X87Field(builder,
                         layout.x87_offset + offsetof(State, result),
                         Type::U64,
                         significands[i]);
                call(X87Command::RestoreRegExtHigh, i, exponents[i]);
            }
        call(X87Command::RefreshStatus);
    } else {
        call(X87Command::Status);
        const auto control = X87Query(builder, layout, offsetof(State, control), Type::U16);
        const auto status = X87Query(builder, layout, offsetof(State, status), Type::U16);
        const auto tags = X87Query(builder, layout, offsetof(State, tags), Type::U16);
        const auto ip = Require(builder.template EmitUniform<OpCode::LoadUniform>(
                Type::U64, layout.x87_ip_offset, Type::U64));
        const auto dp = Require(builder.template EmitUniform<OpCode::LoadUniform>(
                Type::U64, layout.x87_dp_offset, Type::U64));
        const auto opcode = Require(builder.template EmitUniform<OpCode::LoadUniform>(
                Type::U16, layout.x87_opcode_offset, Type::U16));
        if (small) {
            StoreMemory(builder, address, control, 16);
            StoreMemory(builder, address, status, 16, 2);
            StoreMemory(builder, address, tags, 16, 4);
            StoreMemory(builder, address, ip, 16, 6);
            StoreMemory(builder, address, zero, 16, 8);
            StoreMemory(builder, address, dp, 16, 10);
            StoreMemory(builder, address, zero, 16, 12);
        } else {
            StoreMemory(builder, address, control, 32);
            StoreMemory(builder, address, status, 32, 4);
            StoreMemory(builder, address, tags, 32, 8);
            StoreMemory(builder, address, ip, 32, 12);
            StoreMemory(builder, address, zero, 16, 16);
            StoreMemory(builder, address, opcode, 16, 18);
            StoreMemory(builder, address, dp, 32, 20);
            StoreMemory(builder, address, zero, 32, 24);
        }
        if (registers)
            for (unsigned i = 0; i < 8; ++i) {
                call(X87Command::QueryRegExt, i);
                const auto low = X87Query(builder, layout, offsetof(State, result), Type::U64);
                const auto high = X87Query(builder, layout, offsetof(State, reserved), Type::U16);
                StoreMemory(builder, address, low, 64, environment_bytes + i * 10);
                StoreMemory(builder, address, high, 16, environment_bytes + i * 10 + 8);
            }
        call(registers ? X87Command::Init : X87Command::MaskExceptions);
        if (registers) X87Metadata(builder, layout, 0, 0, zero);
    }
    return 1;
}

static unsigned X87(Emitter& builder,
                    const StateLayout& layout,
                    const InstructionData& instruction,
                    u64 pc,
                    u64 next_pc,
                    TargetInfo target,
                    InstructionRange following,
                    std::span<const std::uint8_t> available) {
    using namespace runtime::x86;
    using State = X87State;
    if (layout.x87_offset == UINT32_MAX) Unsupported();
    if (instruction.opcode == Op::FLDENV || instruction.opcode == Op::FNSTENV ||
        instruction.opcode == Op::FSTENV || instruction.opcode == Op::FRSTOR ||
        instruction.opcode == Op::FNSAVE || instruction.opcode == Op::FSAVE)
        return X87Environment(builder, layout, instruction, pc, next_pc, target);
    // Retain the already-established exact helper windows and their first
    // failure boundary; metadata after success describes the final real op.
    if (instruction.opcode == Op::FXCH || instruction.opcode == Op::FLD1)
        return X87RegisterWindow(builder, layout, instruction, pc, target, following, available);
    const auto& first = instruction.ops[0];
    const auto& second = instruction.ops[1];
    const auto stack_index = [](const DecodedOperand& operand) {
        if (operand.type != GuestOperandKind::Register || operand.index < Register::ST0 ||
            operand.index > Register::ST7)
            Unsupported();
        return static_cast<unsigned>(operand.index - Register::ST0);
    };
    const auto no_operands = [&] {
        for (const auto& operand : instruction.ops)
            if (operand.type != GuestOperandKind::None) Unsupported();
    };
    const auto memory_format = [](unsigned bits, bool integer) {
        if (integer) {
            if (bits == 16) return X87Format::Int16;
            if (bits == 32) return X87Format::Int32;
            if (bits == 64) return X87Format::Int64;
        } else {
            if (bits == 32) return X87Format::Float32;
            if (bits == 64) return X87Format::Float64;
        }
        Unsupported();
    };
    PrepareFaultPC(builder, layout, pc, target);
    const auto state = builder.X87Address(layout.x87_offset);
    const auto zero = builder.X87Zero();
    Value address{};
    bool metadata_committed{};
    const auto call = [&](X87Command command,
                          unsigned index = 0,
                          Value raw = {},
                          bool commit_success = false) {
        metadata_committed |= X87CheckedCall(builder,
                                             layout,
                                             instruction,
                                             available,
                                             state,
                                             X87Encode(command, index),
                                             raw ? raw : zero,
                                             address,
                                             commit_success);
    };
    // Waiting x87 instructions check an older exception before touching the
    // guest operand. Non-waiting status/control stores intentionally do not.
    if (Memory(first) && instruction.opcode != Op::FNSTCW && instruction.opcode != Op::FNSTSW)
        call(X87Command::Wait);
    const auto memory_address = [&] {
        if (!address) address = Address(builder, layout, instruction, first, next_pc, target);
        return address;
    };
    const auto metadata = [&] {
        if (!metadata_committed)
            X87Metadata(
                    builder, layout, instruction.addr, X87Opcode(instruction, available), address);
    };
    switch (instruction.opcode) {
        case Op::FNINIT:
        case Op::FINIT:
            no_operands();
            if (instruction.opcode == Op::FINIT) call(X87Command::Wait);
            call(X87Command::Init);
            X87Metadata(builder, layout, 0, 0, zero);
            return 1;
        case Op::FNCLEX:
        case Op::FCLEX:
            no_operands();
            if (instruction.opcode == Op::FCLEX) call(X87Command::Wait);
            call(X87Command::ClearExceptions);
            return 1;
        case Op::WAIT:
            no_operands();
            call(X87Command::Wait);
            return 1;
        case Op::FNOP:
            no_operands();
            call(X87Command::Wait);
            metadata();
            return 1;
        case Op::FLDCW:
            if (!Memory(first) || first.size != 16) Unsupported();
            OneOperand(instruction);
            call(X87Command::LoadControlWord, 0, LoadMemory(builder, Type::U16, memory_address()));
            return 1;
        case Op::FNSTCW:
        case Op::FNSTSW:
        case Op::FSTCW:
        case Op::FSTSW: {
            OneOperand(instruction);
            const bool status = instruction.opcode == Op::FNSTSW || instruction.opcode == Op::FSTSW;
            if ((!Memory(first) && (!status || first.type != GuestOperandKind::Register ||
                                    first.index != Register::AX)) ||
                first.size != 16)
                Unsupported();
            if (Memory(first)) (void)memory_address();
            if (!Memory(first) &&
                (instruction.opcode == Op::FSTCW || instruction.opcode == Op::FSTSW))
                call(X87Command::Wait);
            call(status ? X87Command::Status : X87Command::ControlWord);
            const auto result = X87Query(builder, layout, offsetof(State, result), Type::U16);
            if (Memory(first))
                StoreMemory(builder, address, result, 16);
            else
                WriteRegister(builder, layout, first, result);
            return 1;
        }
        case Op::FLD:
        case Op::FILD: {
            OneOperand(instruction);
            if (first.type == GuestOperandKind::Register) {
                if (instruction.opcode != Op::FLD) Unsupported();
                call(X87Command::PushReg, stack_index(first), {}, true);
            } else {
                if (!Memory(first)) Unsupported();
                const auto ea = memory_address();
                if (instruction.opcode == Op::FLD && first.size == 80) {
                    const auto low = LoadMemory(builder, Type::U64, ea);
                    const auto high = LoadMemory(builder, Type::U16, ea, 8);
                    call(X87Command::StageExtLow, 0, low);
                    call(X87Command::PushExtHigh, 0, high, true);
                } else {
                    const auto format = memory_format(first.size, instruction.opcode == Op::FILD);
                    const auto command = format == X87Format::Float32   ? X87Command::PushF32
                                         : format == X87Format::Float64 ? X87Command::PushF64
                                         : format == X87Format::Int16   ? X87Command::PushI16
                                         : format == X87Format::Int32   ? X87Command::PushI32
                                                                        : X87Command::PushI64;
                    call(command, 0, LoadMemory(builder, ScalarType(first.size), ea), true);
                }
            }
            metadata();
            return 1;
        }
        case Op::FBLD: {
            OneOperand(instruction);
            if (!Memory(first) || first.size != 80) Unsupported();
            const auto ea = memory_address();
            const auto low = LoadMemory(builder, Type::U64, ea);
            const auto high = LoadMemory(builder, Type::U16, ea, 8);
            call(X87Command::StageExtLow, 0, low);
            call(X87Command::PushBcdHigh, 0, high, true);
            metadata();
            return 1;
        }
        case Op::FBSTP: {
            OneOperand(instruction);
            if (!Memory(first) || first.size != 80) Unsupported();
            const auto ea = memory_address();
            call(X87Command::PrepareBcd);
            const auto low = X87Query(builder, layout, offsetof(State, result), Type::U64);
            const auto scratch = X87Query(builder, layout, offsetof(State, reserved), Type::U16);
            const auto invalid = Require(builder.TestBit(
                    Type::Bool, {Operand::ValueOf(scratch), Operand::Immediate(8, Type::U8)}));
            const auto payload = Require(builder.BitClear(Type::U16,
                                                          {Operand::ValueOf(scratch),
                                                           Operand::Immediate(8, Type::U8),
                                                           Operand::Immediate(7, Type::U8)}));
            const auto high =
                    Require(builder.Select(Type::U16,
                                           {Operand::ValueOf(invalid),
                                            Operand::ValueOf(Constant(builder, 0xffff, Type::U16)),
                                            Operand::ValueOf(payload)}));
            StoreMemory(builder, ea, low, 64);
            StoreMemory(builder, ea, high, 16, 8);
            call(X87Command::CommitBcd, 0, {}, true);
            metadata();
            return 1;
        }
        case Op::FCMOVB:
        case Op::FCMOVNB:
        case Op::FCMOVE:
        case Op::FCMOVNE:
        case Op::FCMOVBE:
        case Op::FCMOVNBE:
        case Op::FCMOVU:
        case Op::FCMOVNU: {
            if (instruction.ops[2].type != GuestOperandKind::None ||
                instruction.ops[3].type != GuestOperandKind::None || stack_index(first) != 0)
                Unsupported();
            const auto index = stack_index(second);
            const auto flags =
                    instruction.opcode == Op::FCMOVB || instruction.opcode == Op::FCMOVNB
                            ? Flag::Carry
                    : instruction.opcode == Op::FCMOVE || instruction.opcode == Op::FCMOVNE
                            ? Flag::Zero
                    : instruction.opcode == Op::FCMOVBE || instruction.opcode == Op::FCMOVNBE
                            ? Flag::Carry | Flag::Zero
                            : Flag::Parity;
            const bool invert =
                    instruction.opcode == Op::FCMOVNB || instruction.opcode == Op::FCMOVNE ||
                    instruction.opcode == Op::FCMOVNBE || instruction.opcode == Op::FCMOVNU;
            const auto condition =
                    invert ? Require(builder.TestNotFlags(Type::Bool, {Operand::Flags(flags)}))
                           : Require(builder.TestFlags(Type::Bool, {Operand::Flags(flags)}));
            call(X87Command::ConditionalMove, index, condition, true);
            metadata();
            return 1;
        }
        case Op::FST:
        case Op::FSTP:
        case Op::FIST:
        case Op::FISTP:
        case Op::FISTTP: {
            OneOperand(instruction);
            const bool integer = instruction.opcode == Op::FIST ||
                                 instruction.opcode == Op::FISTP ||
                                 instruction.opcode == Op::FISTTP;
            const bool pop = instruction.opcode == Op::FSTP || instruction.opcode == Op::FISTP ||
                             instruction.opcode == Op::FISTTP;
            if (first.type == GuestOperandKind::Register) {
                if (integer) Unsupported();
                call(pop ? X87Command::StoreRegPop : X87Command::StoreReg,
                     stack_index(first),
                     {},
                     true);
            } else {
                if (!Memory(first)) Unsupported();
                const auto ea = memory_address();
                if (!integer && first.size == 80) {
                    if (!pop) Unsupported();
                    call(X87Command::PrepareExt80);
                    const auto low = X87Query(builder, layout, offsetof(State, result), Type::U64);
                    const auto high =
                            X87Query(builder, layout, offsetof(State, reserved), Type::U16);
                    StoreMemory(builder, ea, low, 64);
                    StoreMemory(builder, ea, high, 16, 8);
                    call(X87Command::CommitExt80, 0, Constant(builder, 1), true);
                } else {
                    const auto format = memory_format(first.size, integer);
                    if (integer && !pop && first.size == 64) Unsupported();
                    const auto command = format == X87Format::Float32   ? X87Command::PrepareF32
                                         : format == X87Format::Float64 ? X87Command::PrepareF64
                                         : format == X87Format::Int16   ? X87Command::PrepareI16
                                         : format == X87Format::Int32   ? X87Command::PrepareI32
                                                                        : X87Command::PrepareI64;
                    call(command, 0, Constant(builder, instruction.opcode == Op::FISTTP));
                    const auto result = X87Query(
                            builder, layout, offsetof(State, result), ScalarType(first.size));
                    StoreMemory(builder, ea, result, first.size);
                    call(X87Command::CommitPrepared, 0, Constant(builder, pop), true);
                }
            }
            metadata();
            return 1;
        }
        case Op::FLDL2T:
        case Op::FLDL2E:
        case Op::FLDPI:
        case Op::FLDLG2:
        case Op::FLDLN2:
        case Op::FLDZ: {
            no_operands();
            const auto constant = instruction.opcode == Op::FLDL2T   ? X87Constant::Log2Ten
                                  : instruction.opcode == Op::FLDL2E ? X87Constant::Log2E
                                  : instruction.opcode == Op::FLDPI  ? X87Constant::Pi
                                  : instruction.opcode == Op::FLDLG2 ? X87Constant::Log10Two
                                  : instruction.opcode == Op::FLDLN2 ? X87Constant::LnTwo
                                                                     : X87Constant::Zero;
            call(X87Command::PushConstant, 0, Constant(builder, static_cast<u64>(constant)), true);
            metadata();
            return 1;
        }
        case Op::FCHS:
        case Op::FABS:
        case Op::FTST:
        case Op::FXAM:
        case Op::FRNDINT: {
            no_operands();
            const auto unary = instruction.opcode == Op::FCHS   ? X87Unary::ChangeSign
                               : instruction.opcode == Op::FABS ? X87Unary::Abs
                               : instruction.opcode == Op::FTST ? X87Unary::Test
                               : instruction.opcode == Op::FXAM ? X87Unary::Examine
                                                                : X87Unary::Round;
            call(X87Command::Unary, 0, Constant(builder, static_cast<u64>(unary)), true);
            metadata();
            return 1;
        }
        case Op::FSQRT:
        case Op::FPREM:
        case Op::FPREM1:
        case Op::FSCALE:
        case Op::FXTRACT:
            no_operands();
            call(instruction.opcode == Op::FSQRT     ? X87Command::Sqrt
                 : instruction.opcode == Op::FSCALE  ? X87Command::Scale
                 : instruction.opcode == Op::FXTRACT ? X87Command::Extract
                                                     : X87Command::Remainder,
                 0,
                 Constant(builder, instruction.opcode == Op::FPREM1),
                 true);
            metadata();
            return 1;
        case Op::FSIN:
        case Op::FCOS:
        case Op::FSINCOS:
        case Op::FPTAN:
        case Op::FPATAN:
        case Op::FYL2X:
        case Op::FYL2XP1:
        case Op::F2XM1: {
            no_operands();
            const auto operation = instruction.opcode == Op::FSIN      ? X87Transcendental::Sin
                                   : instruction.opcode == Op::FCOS    ? X87Transcendental::Cos
                                   : instruction.opcode == Op::FSINCOS ? X87Transcendental::SinCos
                                   : instruction.opcode == Op::FPTAN   ? X87Transcendental::Tan
                                   : instruction.opcode == Op::FPATAN  ? X87Transcendental::Atan
                                   : instruction.opcode == Op::FYL2X   ? X87Transcendental::YLog2X
                                   : instruction.opcode == Op::FYL2XP1
                                           ? X87Transcendental::YLog2XPlusOne
                                           : X87Transcendental::TwoToXMinusOne;
            call(X87Command::Transcendental,
                 0,
                 Constant(builder, static_cast<u64>(operation)),
                 true);
            metadata();
            return 1;
        }
        case Op::FFREE:
        case Op::FFREEP:
            OneOperand(instruction);
            call(X87Command::Free,
                 stack_index(first),
                 Constant(builder, instruction.opcode == Op::FFREEP),
                 true);
            metadata();
            return 1;
        case Op::FINCSTP:
        case Op::FDECSTP:
            no_operands();
            call(X87Command::AdjustTop,
                 0,
                 Constant(builder, instruction.opcode == Op::FINCSTP),
                 true);
            metadata();
            return 1;
        default:
            break;
    }

    bool compare{}, integer{}, pop{}, reverse{}, quiet{}, to_flags{}, pop_twice{};
    X87Binary operation = X87Binary::Add;
    switch (instruction.opcode) {
        case Op::FADD:
        case Op::FADDP:
        case Op::FIADD:
            operation = X87Binary::Add;
            break;
        case Op::FMUL:
        case Op::FMULP:
        case Op::FIMUL:
            operation = X87Binary::Mul;
            break;
        case Op::FSUB:
        case Op::FSUBP:
        case Op::FSUBR:
        case Op::FSUBRP:
        case Op::FISUB:
        case Op::FISUBR:
            operation = X87Binary::Sub;
            break;
        case Op::FDIV:
        case Op::FDIVP:
        case Op::FDIVR:
        case Op::FDIVRP:
        case Op::FIDIV:
        case Op::FIDIVR:
            operation = X87Binary::Div;
            break;
        case Op::FCOM:
        case Op::FCOMP:
        case Op::FCOMPP:
        case Op::FICOM:
        case Op::FICOMP:
        case Op::FUCOM:
        case Op::FUCOMP:
        case Op::FUCOMPP:
        case Op::FCOMI:
        case Op::FCOMIP:
        case Op::FUCOMI:
        case Op::FUCOMIP:
            compare = true;
            break;
        default:
            Unsupported();
    }
    integer = instruction.opcode == Op::FIADD || instruction.opcode == Op::FIMUL ||
              instruction.opcode == Op::FISUB || instruction.opcode == Op::FISUBR ||
              instruction.opcode == Op::FIDIV || instruction.opcode == Op::FIDIVR ||
              instruction.opcode == Op::FICOM || instruction.opcode == Op::FICOMP;
    pop = instruction.opcode == Op::FADDP || instruction.opcode == Op::FMULP ||
          instruction.opcode == Op::FSUBP || instruction.opcode == Op::FSUBRP ||
          instruction.opcode == Op::FDIVP || instruction.opcode == Op::FDIVRP ||
          instruction.opcode == Op::FCOMP || instruction.opcode == Op::FICOMP ||
          instruction.opcode == Op::FUCOMP || instruction.opcode == Op::FCOMIP ||
          instruction.opcode == Op::FUCOMIP;
    reverse = instruction.opcode == Op::FSUBR || instruction.opcode == Op::FSUBRP ||
              instruction.opcode == Op::FISUBR || instruction.opcode == Op::FDIVR ||
              instruction.opcode == Op::FDIVRP || instruction.opcode == Op::FIDIVR;
    quiet = instruction.opcode == Op::FUCOM || instruction.opcode == Op::FUCOMP ||
            instruction.opcode == Op::FUCOMPP || instruction.opcode == Op::FUCOMI ||
            instruction.opcode == Op::FUCOMIP;
    to_flags = instruction.opcode == Op::FCOMI || instruction.opcode == Op::FCOMIP ||
               instruction.opcode == Op::FUCOMI || instruction.opcode == Op::FUCOMIP;
    pop_twice = instruction.opcode == Op::FCOMPP || instruction.opcode == Op::FUCOMPP;
    if (instruction.ops[2].type != GuestOperandKind::None ||
        instruction.ops[3].type != GuestOperandKind::None)
        Unsupported();
    X87Format format = X87Format::Register;
    Value raw = zero;
    unsigned index{};
    u64 options{};
    if (Memory(first)) {
        if (second.type != GuestOperandKind::None || (integer && first.size == 64)) Unsupported();
        format = memory_format(first.size, integer);
        raw = LoadMemory(builder, ScalarType(first.size), memory_address());
    } else {
        if (integer) Unsupported();
        if (pop_twice) {
            if (first.type != GuestOperandKind::None || second.type != GuestOperandKind::None)
                Unsupported();
            index = 1;
        } else if (compare) {
            if (second.type == GuestOperandKind::None)
                index = stack_index(first);
            else {
                if (stack_index(first) != 0) Unsupported();
                index = stack_index(second);
            }
        } else if (first.type == GuestOperandKind::None && second.type == GuestOperandKind::None &&
                   pop) {
            index = 1;
            options |= X87DestIndex;
        } else {
            const auto destination = stack_index(first), source = stack_index(second);
            if (destination == 0 && !pop)
                index = source;
            else {
                if (source != 0) Unsupported();
                index = destination;
                options |= X87DestIndex;
            }
        }
    }
    if (pop) options |= X87Pop;
    if (reverse) options |= X87Reverse;
    if (quiet) options |= X87QuietCompare;
    if (to_flags) options |= X87ToFlags;
    if (pop_twice) options |= X87PopTwice;
    // Keep the original successful hot command ABI for these existing forms.
    if (!compare && !options && format == X87Format::Register && operation != X87Binary::Sub)
        call(operation == X87Binary::Add   ? X87Command::Add
             : operation == X87Binary::Mul ? X87Command::Mul
                                           : X87Command::Div,
             index,
             {},
             true);
    else
        metadata_committed |= X87CheckedCall(
                builder,
                layout,
                instruction,
                available,
                state,
                X87EncodeOperation(compare ? X87Command::Compare : X87Command::Binary,
                                   index,
                                   format,
                                   operation,
                                   options),
                raw,
                address,
                true);
    if (to_flags) {
        const auto flags = X87Query(builder, layout, offsetof(State, result), Type::U32);
        Require(builder.PublishFCmpFlags(Type::Void, {Operand::ValueOf(flags)}));
    }
    metadata();
    return 1;
}
