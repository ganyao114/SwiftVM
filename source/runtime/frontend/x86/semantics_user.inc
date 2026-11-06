// Remaining user-mode scalar/hint instructions. Services supply nondeterministic
// values; no host instruction choice or mutable numerical owner lives here.
static bool UserInstruction(Emitter& builder,
                            StateLayout layout,
                            const InstructionData& instruction,
                            PendingFlags& pending,
                            u64 pc,
                            u64 next_pc,
                            TargetInfo target) {
    switch (instruction.opcode) {
        case Op::MOVNTI: {
            const auto& destination = instruction.ops[0];
            const auto& source = instruction.ops[1];
            if (!Memory(destination) || source.type != GuestOperandKind::Register ||
                source.size != destination.size ||
                (destination.size != 32 && destination.size != 64) ||
                instruction.ops[2].type != GuestOperandKind::None ||
                instruction.ops[3].type != GuestOperandKind::None)
                Unsupported();
            pending.Flush(builder);
            // TSO is stronger than the non-temporal ordering requirement.
            // The cache hint does not change its visible value or fault order.
            Move(builder, layout, instruction, pc, next_pc, target);
            return true;
        }
        case Op::XLAT: {
            OneOperand(instruction);
            const unsigned bits = instruction.address_bits;
            if (bits != 32 && bits != 64) Unsupported();
            const auto& implicit = instruction.ops[0];
            if (implicit.type != GuestOperandKind::None &&
                !(implicit.type == GuestOperandKind::IndexedMemory && implicit.size == 8 &&
                  implicit.index == Register::AL && instruction.base == Gpr(3, bits).index &&
                  instruction.disp == 0 && instruction.scale <= 1))
                Unsupported();
            pending.Flush(builder);
            PrepareFaultPC(builder, layout, pc, target);
            const auto index = ReadRegister(builder, layout, Register::AL, 8);
            const DecodedOperand base{GuestOperandKind::Memory, Gpr(3, bits).index, 8};
            // Address applies the addr32 wrap before FS/GS, including AL.
            const auto address = Address(builder,
                                         layout,
                                         instruction,
                                         base,
                                         next_pc,
                                         target,
                                         {},
                                         nullptr,
                                         Type::U64,
                                         index);
            const auto value =
                    Require(builder.LoadMemoryTSO(Type::U8, {Operand::ValueOf(address)}));
            WriteRegister(builder, layout, Gpr(0, 8), value);
            return true;
        }
        case Op::RDRAND:
        case Op::RDSEED: {
            OneOperand(instruction);
            const auto& destination = instruction.ops[0];
            if (destination.type != GuestOperandKind::Register ||
                (destination.size != 16 && destination.size != 32 && destination.size != 64) ||
                !layout.system_query)
                Unsupported();
            unsigned slot{};
            (void)RegisterOffset(layout, destination.index, destination.size, slot);
            pending.Flush(builder);
            const auto pair = Require(builder.ReadSystemPair(
                    Type::V128,
                    {Operand::Immediate(instruction.opcode == Op::RDRAND ? 2 : 3, Type::U64),
                     Operand::Immediate(layout.system_query, Type::U64)}));
            const auto data = Require(builder.VecExtract64(
                    Type::U64, {Operand::ValueOf(pair), Operand::Immediate(0, Type::U8)}));
            const auto status = Require(builder.VecExtract64(
                    Type::U64, {Operand::ValueOf(pair), Operand::Immediate(1, Type::U8)}));
            const auto success =
                    Require(builder.TestNotZero(Type::Bool, {Operand::ValueOf(status)}));
            const auto result = Require(builder.Select(Type::U64,
                                                       {Operand::ValueOf(success),
                                                        Operand::ValueOf(data),
                                                        Operand::ValueOf(Constant(builder, 0))}));
            WriteRegister(builder, layout, destination, result);
            Require(builder.ClearFlags(Type::Void, {Operand::Flags(Flag::All)}));
            Require(builder.SetCarry(Type::Void, {Operand::ValueOf(success)}));
            return true;
        }
        default:
            return false;
    }
}
