// Members of the single Semantics<Sink> template. No instruction stream or owner.

static void StorePC(Emitter& builder, StateLayout layout, Value pc) {
    builder.PublishRegisters();
    Require(builder.StoreUniform(Type::Void,
            {Operand::Uniform(layout.pc_offset, Type::U64), Operand::ValueOf(pc)}));
}
static void PrepareFaultPC(Emitter& builder, StateLayout layout, u64 pc, TargetInfo target, bool external_helper = false) {
    builder.PrepareFault(layout, pc, target, external_helper);
}
static void EndDispatch(Emitter& builder, u64 reason = 0) { builder.FinishDispatch(reason); }
static unsigned StackBits(const InstructionData& instruction) {
    return instruction.operand_bits == 16 ? 16 : 64;
}
static void WriteStack(Emitter& builder, StateLayout layout, Value stack) {
    WriteRegister(builder, layout, DecodedOperand{GuestOperandKind::Register, Register::RSP, 64}, stack);
}
static void Push(Emitter& builder, StateLayout layout, const InstructionData& instruction, u64 next_pc, TargetInfo target) {
    OneOperand(instruction);
    const auto& source = instruction.ops[0];
    const auto bits = StackBits(instruction);
    if ((source.type != GuestOperandKind::Immediate && source.type != GuestOperandKind::Register && !Memory(source)) ||
        (source.type != GuestOperandKind::Immediate && source.size != bits)) Unsupported();
    // PUSH RSP and [RSP+...] read the original stack before decrementing it.
    const auto value = source.type == GuestOperandKind::Immediate ? Constant(builder, ImmediateBits(instruction, bits), ScalarType(bits))
            : ReadData(builder, layout, instruction, source, next_pc, target, true);
    const auto stack = ReadRegister(builder, layout, Register::RSP, 64);
    const auto changed = Add(builder, stack, Operand::Immediate(u64{} - bits / 8, Type::U64));
    StoreMemory(builder, changed, value, bits);
    WriteStack(builder, layout, changed);
}
static void Pop(Emitter& builder, StateLayout layout, const InstructionData& instruction, u64 next_pc, TargetInfo target) {
    OneOperand(instruction);
    const auto& destination = instruction.ops[0];
    const auto bits = StackBits(instruction);
    if ((destination.type != GuestOperandKind::Register && !Memory(destination)) || destination.size != bits) Unsupported();
    unsigned destination_slot{};
    const auto destination_offset = destination.type == GuestOperandKind::Register
            ? RegisterOffset(layout, destination.index, bits, destination_slot) : 0;
    const auto stack = ReadRegister(builder, layout, Register::RSP, 64);
    const auto value = Require(builder.LoadMemoryTSO(ScalarType(bits), {Operand::ValueOf(stack)}));
    const auto changed = Add(builder, stack, Operand::Immediate(bits / 8, Type::U64));
    if (Memory(destination)) {
        // POP [RSP+...] addresses through the incremented stack, but publishes
        // neither the stack nor its data result before the store succeeds.
        const auto address = Address(builder, layout, instruction, destination, next_pc, target, changed);
        StoreMemory(builder, address, value, bits);
    }
    WriteStack(builder, layout, changed);
    if (destination.type == GuestOperandKind::Register)
        builder.WriteGpr(destination_offset, destination_slot, bits, value);
}
static void Leave(Emitter& builder, StateLayout layout, const InstructionData& instruction) {
    if (instruction.ops[0].type != GuestOperandKind::None) Unsupported();
    const auto bits = StackBits(instruction);
    const auto base = ReadRegister(builder, layout, Register::RBP, 64);
    const auto value = Require(builder.LoadMemoryTSO(ScalarType(bits), {Operand::ValueOf(base)}));
    const auto changed = Add(builder, base, Operand::Immediate(bits / 8, Type::U64));
    WriteStack(builder, layout, changed);
    WriteRegister(builder, layout, DecodedOperand{GuestOperandKind::Register, static_cast<std::uint8_t>(bits == 64 ? Register::RBP : Register::BP),
                                        static_cast<std::uint16_t>(bits)}, value);
}

static Value TransferTarget(Emitter& builder, StateLayout layout, const InstructionData& instruction,
                     u64 next_pc, TargetInfo target) {
    OneOperand(instruction);
    const auto& destination = instruction.ops[0];
    if (destination.type == GuestOperandKind::Relative && (destination.size == 8 || destination.size == 32))
        return Constant(builder, next_pc + instruction.immediate);
    if ((destination.type != GuestOperandKind::Register && !Memory(destination)) || destination.size != 64) Unsupported();
    return ReadData(builder, layout, instruction, destination, next_pc, target, true);
}

static bool Conditional(Op opcode) {
    switch (opcode) {
        case Op::JO: case Op::JNO: case Op::JB: case Op::JAE: case Op::JZ: case Op::JNZ:
        case Op::JBE: case Op::JA: case Op::JS: case Op::JNS: case Op::JP: case Op::JNP:
        case Op::JL: case Op::JGE: case Op::JLE: case Op::JG: return true;
        default: return false;
    }
}
static bool Direct(const InstructionData& instruction) {
    return instruction.ops[0].type == GuestOperandKind::Relative && instruction.ops[1].type == GuestOperandKind::None &&
           instruction.ops[2].type == GuestOperandKind::None && instruction.ops[3].type == GuestOperandKind::None &&
           (instruction.ops[0].size == 8 || instruction.ops[0].size == 32);
}
static void RepeatStore(Emitter& builder, StateLayout layout, const InstructionData& instruction,
                        u64 pc, u64 next_pc, TargetInfo target) {
    StringInstruction(builder, layout, instruction, pc, next_pc, target);
}

static bool DirectCondition(Op opcode, Condition& condition) {
    switch (opcode) {
        case Op::JB: case Op::SETB: case Op::CMOVB: condition = Condition::CS; break;
        case Op::JAE: case Op::SETAE: case Op::CMOVAE: condition = Condition::CC; break;
        case Op::JO: case Op::SETO: case Op::CMOVO: condition = Condition::VS; break;
        case Op::JNO: case Op::SETNO: case Op::CMOVNO: condition = Condition::VC; break;
        case Op::JZ: case Op::SETZ: case Op::CMOVZ: condition = Condition::EQ; break;
        case Op::JNZ: case Op::SETNZ: case Op::CMOVNZ: condition = Condition::NE; break;
        case Op::JS: case Op::SETS: case Op::CMOVS: condition = Condition::MI; break;
        case Op::JNS: case Op::SETNS: case Op::CMOVNS: condition = Condition::PL; break;
        case Op::JL: case Op::SETL: case Op::CMOVL: condition = Condition::LT; break;
        case Op::JGE: case Op::SETGE: case Op::CMOVGE: condition = Condition::GE; break;
        case Op::JLE: case Op::SETLE: case Op::CMOVLE: condition = Condition::LE; break;
        case Op::JG: case Op::SETG: case Op::CMOVG: condition = Condition::GT; break;
        default: return false;
    }
    return true;
}

static Value BranchCondition(Emitter& builder, Op opcode) {
    Condition condition;
    if (DirectCondition(opcode, condition))
        return Require(builder.CondSet(Type::Void, {Operand::Condition(condition)}));
    const auto test = [&](Flag flags, bool set) {
        return set ? Require(builder.TestFlags(Type::Void, {Operand::Flags(flags)}))
                   : Require(builder.TestNotFlags(Type::Void, {Operand::Flags(flags)}));
    };
    switch (opcode) {
        case Op::JBE: case Op::SETBE: case Op::CMOVBE: return test(Flag::Carry | Flag::Zero, true);
        case Op::JA: case Op::SETA: case Op::CMOVA: return test(Flag::Carry | Flag::Zero, false);
        case Op::JP: case Op::SETP: case Op::CMOVP: return test(Flag::Parity, true);
        case Op::JNP: case Op::SETNP: case Op::CMOVNP: return test(Flag::Parity, false);
        default: Unsupported();
    }
}

static void ConditionalSet(Emitter& builder, StateLayout layout, const InstructionData& instruction,
                     u64 pc, u64 next_pc, TargetInfo target) {
    OneOperand(instruction);
    const auto& destination = instruction.ops[0];
    if ((destination.type != GuestOperandKind::Register && !Memory(destination)) || destination.size != 8) Unsupported();
    if (Memory(destination)) PrepareFaultPC(builder, layout, pc, target);
    const auto condition = BranchCondition(builder, instruction.opcode);
    if (destination.type == GuestOperandKind::Register) WriteRegister(builder, layout, destination, condition);
    else StoreMemory(builder, Address(builder, layout, instruction, destination, next_pc, target), condition, 8);
}
static void ConditionalMove(Emitter& builder, StateLayout layout, const InstructionData& instruction,
                      u64 pc, u64 next_pc, TargetInfo target) {
    const auto& destination = instruction.ops[0];
    const auto& source = instruction.ops[1];
    if (destination.type != GuestOperandKind::Register || (destination.size != 16 && destination.size != 32 && destination.size != 64) ||
        (source.type != GuestOperandKind::Register && !Memory(source)) || source.size != destination.size ||
        instruction.ops[2].type != GuestOperandKind::None || instruction.ops[3].type != GuestOperandKind::None) Unsupported();
    if (Memory(source)) PrepareFaultPC(builder, layout, pc, target);
    // CMOV's memory operand is read even when the condition is false.
    const auto value = ReadData(builder, layout, instruction, source, next_pc, target);
    const auto old = ReadRegister(builder, layout, destination.index, destination.size);
    Condition condition;
    Value selected;
    if (DirectCondition(instruction.opcode, condition))
        selected = Require(builder.CondSelect(ScalarType(destination.size),
                {Operand::Condition(condition), Operand::ValueOf(value), Operand::ValueOf(old)}));
    else {
        const auto check = BranchCondition(builder, instruction.opcode);
        selected = Require(builder.Select(ScalarType(destination.size),
                {Operand::ValueOf(check), Operand::ValueOf(value), Operand::ValueOf(old)}));
    }
    // A 32-bit CMOV zeroes the upper half on either path; 16-bit CMOV preserves it.
    WriteRegister(builder, layout, destination, selected);
}

// Byte decoding can stop after an already decoded prefix. Its error belongs at
// that frontier, after those actual instructions have gone through the same
// emitter; it must not hide an earlier unsupported opcode or manufacture a
// fallthrough/flags publication at the failed frontier.
static void Call(Emitter& builder, StateLayout layout, const InstructionData& instruction, PendingFlags& pending, u64 pc, u64 next_pc, TargetInfo target, bool& ended) {
    pending.Flush(builder);
    PrepareFaultPC(builder, layout, pc, target);
    // Resolve CALL RSP/[RSP] before pushing the return PC.
    const auto destination = TransferTarget(builder, layout, instruction, next_pc, target);
    const auto stack = ReadRegister(builder, layout, Register::RSP, 64);
    const auto changed = Add(builder, stack, Operand::Immediate(u64{} - 8, Type::U64));
    StoreMemory(builder, changed, Constant(builder, next_pc), 64);
    WriteStack(builder, layout, changed);
    StorePC(builder, layout, destination);
    EndDispatch(builder);
    ended = true;

}
static void Return(Emitter& builder, StateLayout layout, const InstructionData& instruction, PendingFlags& pending, u64 pc, TargetInfo target, bool& ended, std::span<const std::uint8_t> bytes) {
    OneOperand(instruction);
    u64 adjust{};
    auto opcode_bytes = bytes;
    // FS/GS do not change the implicit stack read.
    while (!opcode_bytes.empty() && (opcode_bytes.front() == 0x64 || opcode_bytes.front() == 0x65))
        opcode_bytes = opcode_bytes.subspan(1);
    if (opcode_bytes.size() == 1 && opcode_bytes[0] == 0xc3 && instruction.ops[0].type == GuestOperandKind::None) {}
    else if (opcode_bytes.size() == 3 && opcode_bytes[0] == 0xc2 && instruction.ops[0].type == GuestOperandKind::Immediate &&
             instruction.ops[0].size == 16) adjust = static_cast<std::uint16_t>(instruction.immediate);
    else Unsupported();
    pending.Flush(builder);
    PrepareFaultPC(builder, layout, pc, target);
    const auto stack = ReadRegister(builder, layout, Register::RSP, 64);
    const auto destination = Require(builder.LoadMemoryTSO(Type::U64,
            {Operand::ValueOf(stack)}));
    const auto advanced = Add(builder, stack, Operand::Immediate(8 + adjust, Type::U64));
    const DecodedOperand rsp{GuestOperandKind::Register, Register::RSP, 64};
    WriteRegister(builder, layout, rsp, advanced);
    StorePC(builder, layout, destination);
    EndDispatch(builder);
    ended = true;

}
static void Mxcsr(Emitter& builder, StateLayout layout, const InstructionData& instruction, PendingFlags& pending, u64 pc, u64 next_pc, TargetInfo target, bool& ended) {
    const auto& memory = instruction.ops[0];
    if (layout.mxcsr_offset > UINT32_MAX - 3 || !Memory(memory) || memory.size != 32 ||
        instruction.ops[1].type != GuestOperandKind::None || instruction.ops[2].type != GuestOperandKind::None ||
        instruction.ops[3].type != GuestOperandKind::None) Unsupported();
    pending.Flush(builder);
    PrepareFaultPC(builder, layout, pc, target);
    if (instruction.opcode == Op::LDMXCSR) {
        const auto value = ReadData(builder, layout, instruction, memory, next_pc, target);
        Require(builder.StoreUniform(Type::Void,
                {Operand::Uniform(layout.mxcsr_offset, Type::U32), Operand::ValueOf(value, Type::U32)}));
        StorePC(builder, layout, Constant(builder, next_pc));
        EndDispatch(builder, 3); // Root reloads the guest FP environment before re-entry.
        ended = true;
    } else {
        const auto value = Require(builder.LoadUniform(Type::U32,
                {Operand::Uniform(layout.mxcsr_offset, Type::U32)}));
        u64 displacement{};
        const auto address = Address(builder, layout, instruction, memory, next_pc, target, {}, &displacement);
        StoreMemory(builder, address, value, 32, displacement);
    }

}
static void CpuQuery(Emitter& builder, StateLayout layout, const InstructionData& instruction, PendingFlags& pending, u64 next_pc, bool& ended) {
    if (!layout.cpu_query || instruction.ops[0].type != GuestOperandKind::None || instruction.ops[1].type != GuestOperandKind::None ||
        instruction.ops[2].type != GuestOperandKind::None || instruction.ops[3].type != GuestOperandKind::None) Unsupported();
    const auto leaf = ReadRegister(builder, layout, Register::EAX, 32);
    const auto subleaf = ReadRegister(builder, layout, Register::ECX, 32);
    pending.Flush(builder);
    builder.PublishRegisters();
    const auto lower = Require(builder.Cpuid(Type::U64,
            {Operand::ValueOf(leaf, Type::U32), Operand::ValueOf(subleaf, Type::U32),
             Operand::Immediate(layout.cpu_query, Type::U64)}));
    const auto upper = Require(builder.CpuidUpper(Type::U64, {Operand::ValueOf(lower)}));
    const auto ebx = Require(builder.LsrImm(Type::U64,
            {Operand::ValueOf(lower), Operand::Immediate(32, Type::U8)}));
    const auto edx = Require(builder.LsrImm(Type::U64,
            {Operand::ValueOf(upper), Operand::Immediate(32, Type::U8)}));
    WriteRegister(builder, layout, Gpr(0, 32), lower);
    WriteRegister(builder, layout, Gpr(3, 32), ebx);
    WriteRegister(builder, layout, Gpr(1, 32), upper);
    WriteRegister(builder, layout, Gpr(2, 32), edx);
    StorePC(builder, layout, Constant(builder, next_pc));
    EndDispatch(builder); // Never turn serialization into an internal Function edge.
    ended = true;

}
static void Lea(Emitter& builder, StateLayout layout, const InstructionData& instruction, u64 next_pc, TargetInfo target) {
    if (instruction.ops[0].type != GuestOperandKind::Register ||
        (instruction.ops[0].size != 16 && instruction.ops[0].size != 32 && instruction.ops[0].size != 64) ||
        !Memory(instruction.ops[1]) || instruction.ops[2].type != GuestOperandKind::None ||
        instruction.ops[3].type != GuestOperandKind::None) Unsupported();
    const bool address32 = instruction.address_bits == 32;
    auto address = Address(builder, layout, instruction, instruction.ops[1], next_pc, target, {}, nullptr,
            address32 || instruction.ops[0].size == 32 ? Type::U32 : Type::U64);
    // Addr32 wraps before a 64-bit destination receives its
    // zero-extended result; 16-bit writes retain the parent.
    if (address32 && instruction.ops[0].size == 64)
        address = Require(builder.ZeroExtend64(Type::U64,
                {Operand::ValueOf(address, Type::U32)}));
    WriteRegister(builder, layout, instruction.ops[0], address);

}
static void NonTemporalVectorStore(Emitter& builder, StateLayout layout, const InstructionData& instruction, PendingFlags& pending, u64 pc, u64 next_pc, TargetInfo target) {
    const auto& destination = instruction.ops[0];
    const auto& source = instruction.ops[1];
    if (!Memory(destination) || destination.size != 128 || !VRegister(source) || source.size != 128 ||
        instruction.ops[2].type != GuestOperandKind::None || instruction.ops[3].type != GuestOperandKind::None ||
        layout.fprs_offset == UINT32_MAX) Unsupported();
    pending.Flush(builder);
    const auto value = VRead(builder, layout, instruction, source, pc, next_pc, target);
    VWrite<OpCode::StoreMemory>(builder, layout, instruction, destination, value, pc, next_pc, target, true);

}
static void SystemCall(Emitter& builder, StateLayout layout, const InstructionData& instruction, PendingFlags& pending, u64 next_pc, bool& ended) {
    if (instruction.ops[0].type != GuestOperandKind::None) Unsupported();
    pending.Flush(builder);
    StorePC(builder, layout, Constant(builder, next_pc));
    EndDispatch(builder, 1);
    ended = true;

}

template<class Flow>
static void Jump(Emitter& builder, StateLayout layout, const InstructionData& instruction, PendingFlags& pending,
                 u64 pc, u64 next_pc, TargetInfo target, Flow& flow, bool& ended) {
    if (Direct(instruction)) flow.finish(next_pc + instruction.immediate);
    else {
        pending.Flush(builder);
        if (Memory(instruction.ops[0])) PrepareFaultPC(builder, layout, pc, target);
        StorePC(builder, layout, TransferTarget(builder, layout, instruction, next_pc, target));
        EndDispatch(builder);
    }
    ended = true;

}
