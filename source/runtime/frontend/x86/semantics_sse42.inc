static bool Sse42Instruction(Emitter& builder, StateLayout layout, const InstructionData& instruction,
                             PendingFlags& pending, u64 pc, u64 next_pc, TargetInfo target) {
    const bool explicit_lengths = instruction.opcode == Op::PCMPESTRI || instruction.opcode == Op::PCMPESTRM ||
            instruction.opcode == Op::VPCMPESTRI || instruction.opcode == Op::VPCMPESTRM;
    const bool mask = instruction.opcode == Op::PCMPESTRM || instruction.opcode == Op::PCMPISTRM ||
            instruction.opcode == Op::VPCMPESTRM || instruction.opcode == Op::VPCMPISTRM;
    if (!explicit_lengths && instruction.opcode != Op::PCMPISTRI && instruction.opcode != Op::PCMPISTRM &&
            instruction.opcode != Op::VPCMPISTRI && instruction.opcode != Op::VPCMPISTRM) return false;
    const auto& first = instruction.ops[0]; const auto& second = instruction.ops[1];
    if (!VRegister(first) || first.size != 128 || (!VRegister(second) && !Memory(second)) || second.size != 128 ||
        instruction.ops[2].type != GuestOperandKind::Immediate || instruction.ops[2].size != 8 ||
        instruction.ops[3].type != GuestOperandKind::None) Unsupported();
    VOffset(layout, first);
    if (instruction.vector_bytes && (instruction.vector_bytes != 16 || (mask && layout.ymm_high_offset > UINT32_MAX - 15))) Unsupported();
    unsigned slot{};
    if (mask) VOffset(layout, DecodedOperand{GuestOperandKind::Register, Register::XMM0, 128});
    else (void)RegisterOffset(layout, Register::ECX, 32, slot);
    if (explicit_lengths) {
        (void)RegisterOffset(layout, Register::RAX, 64, slot);
        (void)RegisterOffset(layout, Register::RDX, 64, slot);
    }
    pending.Flush(builder);
    const auto a = VRead(builder, layout, instruction, first, pc, next_pc, target);
    // Intel's string compares explicitly allow unaligned m128, and read the
    // whole operand even when its logical string terminates earlier.
    const auto b = VRead(builder, layout, instruction, second, pc, next_pc, target);
    const auto control = instruction.immediate & 255;
    Value packed;
    if (explicit_lengths) {
        const auto low = Require(builder.VecDup64(Type::V128,
                {Operand::ValueOf(ReadRegister(builder, layout, Register::RAX, 64))}));
        const auto high = Require(builder.VecDup64(Type::V128,
                {Operand::ValueOf(ReadRegister(builder, layout, Register::RDX, 64))}));
        const auto lengths = Require(builder.VecZip(Type::V128,
                {Operand::ValueOf(low), Operand::ValueOf(high), Operand::Immediate(64, Type::U8), Operand::Immediate(0, Type::U8)}));
        packed = Require(builder.Sse42StrExplicit(Type::U64,
                {Operand::ValueOf(a), Operand::ValueOf(b), Operand::ValueOf(lengths),
                 Operand::Immediate(control | (instruction.operand_bits == 64 ? 256 : 0), Type::U16)}));
    } else packed = Require(builder.Sse42Str(Type::U64,
            {Operand::ValueOf(a), Operand::ValueOf(b), Operand::Immediate(control, Type::U8)}));
    Require(builder.PublishSse42StrFlags(Type::Void, {Operand::ValueOf(packed), Operand::Flags(Flag::All)}));
    if (!mask) {
        const auto index = Extract(builder, packed, 16, 8, Type::U32);
        WriteRegister(builder, layout, Gpr(1, 32), index);
        return true;
    }
    const auto bits = Extract(builder, packed, 0, 16);
    Value result;
    if (!(control & 64)) result = VLowScalar(builder, bits, 64);
    else {
        const bool words = control & 1;
        const auto indices = words ? VConstant(builder, UINT64_C(0x8000800080008000), UINT64_C(0x8000800080008000))
                                   : VConstant(builder, 0, UINT64_C(0x0101010101010101));
        const auto bit_masks = words ? VConstant(builder, UINT64_C(0x0008000400020001), UINT64_C(0x0080004000200010))
                                     : VConstant(builder, UINT64_C(0x8040201008040201), UINT64_C(0x8040201008040201));
        const auto duplicate = Require(builder.VecDup64(Type::V128, {Operand::ValueOf(bits)}));
        const auto spread = Require(builder.VecTableLookup8(Type::V128, {Operand::ValueOf(duplicate), Operand::ValueOf(indices)}));
        const auto selected = Require(builder.VecAnd(Type::V128, {Operand::ValueOf(spread), Operand::ValueOf(bit_masks)}));
        result = Require(builder.VecCmpEq(Type::V128,
                {Operand::ValueOf(selected), Operand::ValueOf(bit_masks), Operand::Immediate(words ? 16 : 8, Type::U8)}));
    }
    builder.WriteXmm(layout.fprs_offset, 0, result);
    if (instruction.vector_bytes) {
        const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
        Require(builder.StoreUniform(Type::Void,
                {Operand::Uniform(layout.ymm_high_offset, Type::V128), Operand::ValueOf(zero)}));
    }
    return true;
}
