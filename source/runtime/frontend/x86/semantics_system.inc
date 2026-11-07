// Architectural system/state operations. Memory images use the explicitly
// installed runtime service; scalar/base/counter operations stay in neutral IR.
static void WideCompareExchange(Emitter& builder,
                                StateLayout layout,
                                const InstructionData& instruction,
                                PendingFlags& pending,
                                u64 pc,
                                u64 next_pc,
                                TargetInfo target) {
    const bool wide = instruction.opcode == Op::CMPXCHG16B;
    const auto& memory = instruction.ops[0];
    OneOperand(instruction);
    if (!Memory(memory) || memory.size != (wide ? 128 : 64)) Unsupported();
    pending.Flush(builder);
    PrepareFaultPC(builder, layout, pc, target);
    const auto address = Address(builder, layout, instruction, memory, next_pc, target);
    if (wide)
        Require(builder.CheckMemoryAlignment(
                Type::Void, {Operand::ValueOf(address), Operand::Immediate(15, Type::U64)}));
    const auto old_a = ReadRegister(builder, layout, Register::RAX, 64),
               old_d = ReadRegister(builder, layout, Register::RDX, 64);
    const auto bits = wide ? 64u : 32u;
    const auto b = ReadRegister(builder, layout, Gpr(3, bits).index, bits),
               c = ReadRegister(builder, layout, Gpr(1, bits).index, bits);
    Value low, high, different;
    if (wide) {
        const auto observed = Require(builder.CompareAndSwap128(Type::V128,
                                                                {Operand::ValueOf(address),
                                                                 Operand::ValueOf(old_a),
                                                                 Operand::ValueOf(old_d),
                                                                 Operand::ValueOf(b),
                                                                 Operand::ValueOf(c)}));
        low = Require(builder.VecExtract64(
                Type::U64, {Operand::ValueOf(observed), Operand::Immediate(0, Type::U8)}));
        high = Require(builder.VecExtract64(
                Type::U64, {Operand::ValueOf(observed), Operand::Immediate(1, Type::U8)}));
        const auto lo_diff =
                Require(builder.Xor(Type::U64, {Operand::ValueOf(low), Operand::ValueOf(old_a)}));
        const auto hi_diff =
                Require(builder.Xor(Type::U64, {Operand::ValueOf(high), Operand::ValueOf(old_d)}));
        different = Require(
                builder.Or(Type::U64, {Operand::ValueOf(lo_diff), Operand::ValueOf(hi_diff)}));
    } else {
        const auto a32 =
                Require(builder.ZeroExtend64(Type::U64, {Operand::ValueOf(old_a, Type::U32)}));
        const auto d32 =
                Require(builder.ZeroExtend64(Type::U64, {Operand::ValueOf(old_d, Type::U32)}));
        const auto d_high = Require(builder.LslImm(
                Type::U64, {Operand::ValueOf(d32), Operand::Immediate(32, Type::U8)}));
        const auto expected =
                Require(builder.Or(Type::U64, {Operand::ValueOf(a32), Operand::ValueOf(d_high)}));
        const auto b32 = Require(builder.ZeroExtend64(Type::U64, {Operand::ValueOf(b, Type::U32)}));
        const auto c32 = Require(builder.ZeroExtend64(Type::U64, {Operand::ValueOf(c, Type::U32)}));
        const auto c_high = Require(builder.LslImm(
                Type::U64, {Operand::ValueOf(c32), Operand::Immediate(32, Type::U8)}));
        const auto desired =
                Require(builder.Or(Type::U64, {Operand::ValueOf(b32), Operand::ValueOf(c_high)}));
        const auto observed = Require(builder.CompareAndSwap(Type::U64,
                                                             {Operand::ValueOf(address),
                                                              Operand::ValueOf(expected),
                                                              Operand::ValueOf(desired)}));
        different = Require(
                builder.Xor(Type::U64, {Operand::ValueOf(observed), Operand::ValueOf(expected)}));
        low = Require(builder.ZeroExtend64(Type::U64, {Operand::ValueOf(observed, Type::U32)}));
        high = Require(builder.LsrImm(
                Type::U64, {Operand::ValueOf(observed), Operand::Immediate(32, Type::U8)}));
    }
    // Success preserves complete RAX/RDX, including the high halves of the
    // 8B form. Failure writes EAX/EDX (zero-high) or complete RAX/RDX.
    const auto a = Require(builder.Select(
            Type::U64,
            {Operand::ValueOf(different), Operand::ValueOf(low), Operand::ValueOf(old_a)}));
    const auto d = Require(builder.Select(
            Type::U64,
            {Operand::ValueOf(different), Operand::ValueOf(high), Operand::ValueOf(old_d)}));
    WriteRegister(builder, layout, Gpr(0, 64), a);
    WriteRegister(builder, layout, Gpr(2, 64), d);
    const auto flags = Require(builder.Sub(
            Type::U64, {Operand::ValueOf(different), Operand::Immediate(0, Type::U64)}));
    pending = {builder.ProducerOf(flags),
               FlagProfile::ArithmeticCanonicalCarry,
               FlagBits(Flag::Zero),
               {}};
}

static bool SystemInstruction(Emitter& builder,
                              StateLayout layout,
                              const InstructionData& instruction,
                              PendingFlags& pending,
                              u64 pc,
                              u64 next_pc,
                              TargetInfo target,
                              bool& ended) {
    switch (instruction.opcode) {
        case Op::CMPXCHG8B:
        case Op::CMPXCHG16B:
            WideCompareExchange(builder, layout, instruction, pending, pc, next_pc, target);
            return true;
        case Op::RDTSC:
        case Op::RDTSCP: {
            OneOperand(instruction);
            if (instruction.ops[0].type != GuestOperandKind::None || !layout.system_query)
                Unsupported();
            const auto value = Require(builder.ReadSystem(
                    Type::U64,
                    {Operand::Immediate(instruction.opcode == Op::RDTSCP, Type::U64),
                     Operand::Immediate(layout.system_query, Type::U64)}));
            WriteRegister(builder, layout, Gpr(0, 32), value);
            const auto high = Require(builder.LsrImm(
                    Type::U64, {Operand::ValueOf(value), Operand::Immediate(32, Type::U8)}));
            WriteRegister(builder, layout, Gpr(2, 32), high);
            if (instruction.opcode == Op::RDTSCP)
                WriteRegister(builder, layout, Gpr(1, 32), Constant(builder, 0, Type::U32));
            return true;
        }
        case Op::RDFSBASE:
        case Op::RDGSBASE:
        case Op::WRFSBASE:
        case Op::WRGSBASE: {
            OneOperand(instruction);
            const auto reg = instruction.ops[0];
            if (reg.type != GuestOperandKind::Register || (reg.size != 32 && reg.size != 64))
                Unsupported();
            const bool gs =
                    instruction.opcode == Op::RDGSBASE || instruction.opcode == Op::WRGSBASE;
            const auto offset = gs ? layout.gs_base_offset : layout.fs_base_offset;
            if (offset > UINT32_MAX - 7) Unsupported();
            if (instruction.opcode == Op::RDFSBASE || instruction.opcode == Op::RDGSBASE)
                WriteRegister(builder,
                              layout,
                              reg,
                              Require(builder.LoadUniform(Type::U64,
                                                          {Operand::Uniform(offset, Type::U64)})));
            else {
                auto value = ReadRegister(builder, layout, reg.index, reg.size);
                if (reg.size == 32)
                    value = Require(
                            builder.ZeroExtend64(Type::U64, {Operand::ValueOf(value, Type::U32)}));
                Require(builder.StoreUniform(
                        Type::Void,
                        {Operand::Uniform(offset, Type::U64), Operand::ValueOf(value)}));
            }
            return true;
        }
        case Op::XGETBV: {
            OneOperand(instruction);
            if (instruction.ops[0].type != GuestOperandKind::None) Unsupported();
            pending.Flush(builder);
            PrepareFaultPC(builder, layout, pc, target);
            const auto index = ReadRegister(builder, layout, Register::ECX, 32);
            Require(builder.CheckMemoryAlignment(
                    Type::Void,
                    {Operand::ValueOf(index), Operand::Immediate(UINT32_MAX, Type::U64)}));
            WriteRegister(builder, layout, Gpr(0, 32), Constant(builder, 7, Type::U32));
            WriteRegister(builder, layout, Gpr(2, 32), Constant(builder, 0, Type::U32));
            return true;
        }
        case Op::LFENCE:
        case Op::MFENCE:
            OneOperand(instruction);
            if (instruction.ops[0].type != GuestOperandKind::None) Unsupported();
            if (instruction.opcode == Op::LFENCE)
                Require(builder.MemoryLoadFence(Type::Void, {}));
            else
                Require(builder.MemoryBarrierTSO(Type::Void, {}));
            return true;
        case Op::PAUSE:
            OneOperand(instruction);
            if (instruction.ops[0].type != GuestOperandKind::None) Unsupported();
            Require(builder.CpuYield(Type::Void, {}));
            return true;
        case Op::CLFLUSH: {
            OneOperand(instruction);
            if (!Memory(instruction.ops[0])) Unsupported();
            pending.Flush(builder);
            PrepareFaultPC(builder, layout, pc, target);
            const auto address =
                    Address(builder, layout, instruction, instruction.ops[0], next_pc, target);
            Require(builder.LoadMemoryTSO(Type::U8, {Operand::ValueOf(address)}));
            Require(builder.MemoryBarrierTSO(Type::Void, {}));
            return true;
        }
        case Op::FXSAVE:
        case Op::FXSAVE64:
        case Op::FXRSTOR:
        case Op::FXRSTOR64:
        case Op::XSAVE:
        case Op::XSAVE64:
        case Op::XSAVEOPT:
        case Op::XSAVEOPT64:
        case Op::XSAVEC:
        case Op::XSAVEC64:
        case Op::XRSTOR:
        case Op::XRSTOR64: {
            OneOperand(instruction);
            if (!layout.runtime_state_services || !Memory(instruction.ops[0])) Unsupported();
            pending.Flush(builder);
            PrepareFaultPC(builder, layout, pc, target);
            const auto address =
                    Address(builder, layout, instruction, instruction.ops[0], next_pc, target);
            unsigned kind{};
            switch (instruction.opcode) {
                case Op::FXRSTOR:
                case Op::FXRSTOR64:
                    kind = 1;
                    break;
                case Op::XSAVE:
                case Op::XSAVE64:
                    kind = 2;
                    break;
                case Op::XRSTOR:
                case Op::XRSTOR64:
                    kind = 3;
                    break;
                case Op::XSAVEOPT:
                case Op::XSAVEOPT64:
                    kind = 4;
                    break;
                case Op::XSAVEC:
                case Op::XSAVEC64:
                    kind = 5;
                    break;
                default:
                    break;
            }
            auto requested = Constant(builder, 3);
            if (kind >= 2) {
                const auto lo = ReadRegister(builder, layout, Register::EAX, 32),
                           hi = ReadRegister(builder, layout, Register::EDX, 32);
                const auto high = Require(builder.LslImm(
                        Type::U64,
                        {Operand::ValueOf(hi, Type::U32), Operand::Immediate(32, Type::U8)}));
                requested = Require(builder.Or(
                        Type::U64, {Operand::ValueOf(lo, Type::U32), Operand::ValueOf(high)}));
            }
            const bool wide =
                    instruction.opcode == Op::FXSAVE64 || instruction.opcode == Op::FXRSTOR64 ||
                    instruction.opcode == Op::XSAVE64 || instruction.opcode == Op::XRSTOR64 ||
                    instruction.opcode == Op::XSAVEOPT64 || instruction.opcode == Op::XSAVEC64;
            builder.StateMemory(kind | (wide ? 256 : 0), address, requested, pc, next_pc);
            ended = true;
            return true;
        }
        default:
            return false;
    }
}
