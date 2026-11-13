// Members of the single Semantics<Sink> template. No instruction stream or owner.

template <class Flow>
[[gnu::always_inline]] static inline unsigned Execute(Emitter& builder,
                                                      StateLayout layout,
                                                      const InstructionData& instruction,
                                                      std::span<const std::uint8_t> bytes,
                                                      PendingFlags& pending,
                                                      u64 pc,
                                                      u64 next_pc,
                                                      TargetInfo target,
                                                      Flow& flow,
                                                      bool& ended,
                                                      InstructionRange following,
                                                      std::span<const std::uint8_t> available,
                                                      bool budget_safepoint = false) {
    if (target.Has(TargetFeature::GuestEvents)) {
        using runtime::frontend::CrossesGuestEventWindow;
        using runtime::frontend::GuestEventTransfer;
        bool observe = budget_safepoint || CrossesGuestEventWindow(pc, next_pc);
        bool dispatch_observe{};
        if (instruction.flow == FlowKind::Return)
            dispatch_observe = true;
        else if (instruction.flow == FlowKind::Jump || instruction.flow == FlowKind::Call ||
                 instruction.flow == FlowKind::Conditional) {
            if (Direct(instruction))
                observe |= GuestEventTransfer(pc, next_pc + instruction.immediate);
            else
                dispatch_observe = true;
        }
        // Each self-dispatch reaches this boundary before the next REP element.
        if (instruction.opcode == Op::MOVS || instruction.opcode == Op::LODS ||
            instruction.opcode == Op::CMPS || instruction.opcode == Op::SCAS ||
            instruction.opcode == Op::STOS)
            observe |= (instruction.flags &
                        (InstructionFlag::Repeat | InstructionFlag::RepeatNotEqual)) != 0;
        if (observe || dispatch_observe) {
            pending.Flush(builder);
            builder.PublishRegisters();
            if (observe)
                Require(builder.template EmitImm<OpCode::GuestSafepoint>(Type::Void, pc, Type::U64));
            else
                Require(builder.template EmitImm<OpCode::GuestDispatchSafepoint>(
                        Type::Void, pc, Type::U64));
        }
    }
    unsigned consumed = 1;
    if (Conditional(instruction.opcode)) {
        if (!Direct(instruction)) Unsupported();
        pending.Flush(builder);
        builder.PublishRegisters();
        const auto condition = BranchCondition(builder, instruction.opcode);
        const u64 taken = next_pc + instruction.immediate;
        flow.branch(condition, taken, next_pc);
        ended = true;
    } else
        switch (instruction.opcode) {
            case Op::CLC:
            case Op::STC:
            case Op::CMC:
            case Op::CLD:
            case Op::STD:
            case Op::LAHF:
            case Op::SAHF:
            case Op::PUSHF:
            case Op::POPF:
                FlagInstruction(builder, layout, instruction, pending, pc, target);
                return 1;
            case Op::BSWAP:
            case Op::MOVBE:
                if (Memory(instruction.ops[0]) || Memory(instruction.ops[1]))
                    pending.Flush(builder);
                SwapBytes(builder, layout, instruction, pc, next_pc, target);
                return 1;
            case Op::LZCNT:
                LeadingCount(builder, layout, instruction, pending, pc, next_pc, target);
                return 1;
            case Op::SHLD:
            case Op::SHRD:
                DoubleShift(builder, layout, instruction, pending, pc, next_pc, target);
                return 1;
            case Op::RCL:
            case Op::RCR:
                CarryRotate(builder, layout, instruction, pending, pc, next_pc, target);
                return 1;
            case Op::ENTER:
                EnterFrame(builder, layout, instruction, pending, pc, target);
                return 1;
            case Op::LOOP:
            case Op::LOOPZ:
            case Op::LOOPNZ:
            case Op::JCXZ:
            case Op::JECXZ:
            case Op::JRCXZ:
                CounterBranch(builder, layout, instruction, pending, next_pc, flow, ended);
                return 1;
            case Op::MOVS:
            case Op::LODS:
            case Op::CMPS:
            case Op::SCAS:
                pending.Flush(builder);
                StringInstruction(builder, layout, instruction, pc, next_pc, target);
                ended = true;
                return 1;
            case Op::FCMOVB:
            case Op::FCMOVNB:
            case Op::FCMOVE:
            case Op::FCMOVNE:
            case Op::FCMOVBE:
            case Op::FCMOVNBE:
            case Op::FCMOVU:
            case Op::FCMOVNU:
            case Op::FBLD:
            case Op::FBSTP:
            case Op::FSAVE:
            case Op::FINIT:
            case Op::FCLEX:
            case Op::FSTCW:
            case Op::FSTSW:
            case Op::FSTENV:
            case Op::FNINIT:
            case Op::FLD1:
            case Op::FLD:
            case Op::FXCH:
            case Op::FADD:
            case Op::FMUL:
            case Op::FDIV:
            case Op::FSQRT:
            case Op::FSTP:
            case Op::FLDENV:
            case Op::FRSTOR:
            case Op::FNSAVE:
            case Op::FNSTENV:
            case Op::FNCLEX:
            case Op::WAIT:
            case Op::FNOP:
            case Op::FLDCW:
            case Op::FNSTCW:
            case Op::FNSTSW:
            case Op::FILD:
            case Op::FST:
            case Op::FIST:
            case Op::FISTP:
            case Op::FISTTP:
            case Op::FLDL2T:
            case Op::FLDL2E:
            case Op::FLDPI:
            case Op::FLDLG2:
            case Op::FLDLN2:
            case Op::FLDZ:
            case Op::FCHS:
            case Op::FABS:
            case Op::FTST:
            case Op::FXAM:
            case Op::FRNDINT:
            case Op::FPREM:
            case Op::FPREM1:
            case Op::FSCALE:
            case Op::FXTRACT:
            case Op::FSIN:
            case Op::FCOS:
            case Op::FSINCOS:
            case Op::FPTAN:
            case Op::FPATAN:
            case Op::FYL2X:
            case Op::FYL2XP1:
            case Op::F2XM1:
            case Op::FFREE:
            case Op::FFREEP:
            case Op::FINCSTP:
            case Op::FDECSTP:
            case Op::FADDP:
            case Op::FIADD:
            case Op::FMULP:
            case Op::FIMUL:
            case Op::FSUB:
            case Op::FSUBP:
            case Op::FSUBR:
            case Op::FSUBRP:
            case Op::FISUB:
            case Op::FISUBR:
            case Op::FDIVP:
            case Op::FDIVR:
            case Op::FDIVRP:
            case Op::FIDIV:
            case Op::FIDIVR:
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
                pending.Flush(builder);
                consumed = X87(
                        builder, layout, instruction, pc, next_pc, target, following, available);
                break;
            case Op::STOS:
                pending.Flush(builder);
                RepeatStore(builder, layout, instruction, pc, next_pc, target);
                ended = true;
                break;
            case Op::ADD:
                Alu<OpCode::Add>(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::SUB:
                Alu<OpCode::Sub>(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::ADC:
                Alu<OpCode::Adc>(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::SBB:
                Alu<OpCode::Sbb>(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::INC:
                Alu<OpCode::Add, true>(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::DEC:
                Alu<OpCode::Sub, true>(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::CMP:
                Alu<OpCode::Sub>(builder, layout, instruction, pending, pc, next_pc, target, false);
                break;
            case Op::AND:
                Alu<OpCode::And>(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::OR:
                Alu<OpCode::Or>(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::XOR:
                Alu<OpCode::Xor>(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::TEST:
                Alu<OpCode::And>(builder, layout, instruction, pending, pc, next_pc, target, false);
                break;
            case Op::NEG:
                Alu<OpCode::Neg>(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::CBW:
            case Op::CWDE:
            case Op::CDQE:
            case Op::CWD:
            case Op::CDQ:
            case Op::CQO:
                ExtendAccumulator(builder, layout, instruction);
                break;
            case Op::MUL:
            case Op::IMUL:
                if (Memory(instruction.ops[0]) || Memory(instruction.ops[1]) ||
                    (pending.recipe.result && pending.recipe.opcode != Op::MUL &&
                     pending.recipe.opcode != Op::IMUL))
                    pending.Flush(builder);
                // MUL replaces only CF/OF. Keep unobserved ALU NZPA
                // at its real producer; an older MUL recipe is simply
                // replaced, while any already published bits remain.
                pending.outputs &= ~FlagBits(Flag::Carry | Flag::Overflow);
                if (!pending.outputs) pending.producer = {};
                Multiply(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::DIV:
            case Op::IDIV:
                pending.Flush(builder);
                Divide(builder, layout, instruction, pc, next_pc, target);
                break;
            case Op::BT:
            case Op::BTS:
            case Op::BTR:
            case Op::BTC:
                BitString(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::BSF:
            case Op::BSR:
            case Op::TZCNT:
                BitScan(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::POPCNT:
                PopulationCount(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::CRC32:
                Crc32(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::MOVSS:
            case Op::CVTSI2SS:
            case Op::CVTTSS2SI:
            case Op::CVTSS2SI:
            case Op::CVTSD2SI:
            case Op::UCOMISS:
            case Op::COMISS:
            case Op::PTEST:
            case Op::MASKMOVDQU:
            case Op::MOVSD:
            case Op::CVTSI2SD:
            case Op::ADDSD:
            case Op::SUBSD:
            case Op::MULSD:
            case Op::DIVSD:
            case Op::UCOMISD:
            case Op::COMISD:
            case Op::CVTTSD2SI:
                ScalarDouble(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::SHL:
            case Op::SAL:
            case Op::SHR:
            case Op::SAR:
            case Op::ROL:
            case Op::ROR: {
                const auto bits = instruction.ops[0].size;
                if (instruction.ops[0].type == GuestOperandKind::Register &&
                    instruction.ops[1].type == GuestOperandKind::Immediate &&
                    instruction.ops[2].type == GuestOperandKind::None &&
                    instruction.ops[3].type == GuestOperandKind::None &&
                    (bits == 8 || bits == 16 || bits == 32 || bits == 64)) {
                    const unsigned count =
                            static_cast<unsigned>(static_cast<std::uint8_t>(instruction.immediate)) &
                            (bits == 64 ? 63u : 31u);
                    if (count != 0) {
                        const bool rotate =
                                instruction.opcode == Op::ROL || instruction.opcode == Op::ROR;
                        const u64 written =
                                (rotate ? FlagBits(Flag::Carry)
                                        : FlagBits(Flag::Carry | Flag::Parity | Flag::Zero |
                                                   Flag::Negate)) |
                                (count == 1 ? FlagBits(Flag::Overflow) : 0);
                        if (pending.recipe.result) {
                            const bool prev_mul = pending.recipe.opcode == Op::MUL ||
                                                  pending.recipe.opcode == Op::IMUL;
                            if (prev_mul && (written & FlagBits(Flag::Carry | Flag::Overflow)) ==
                                                    FlagBits(Flag::Carry | Flag::Overflow))
                                pending.recipe = {};
                            else
                                pending.Observe(builder, FlagBits(Flag::All) & ~written);
                        }
                        pending.outputs &= ~written;
                        if (!pending.outputs) pending.producer = {};
                    }
                } else {
                    pending.Flush(builder);
                }
                Shift(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            }
            case Op::NOT:
                if (Memory(instruction.ops[0])) pending.Flush(builder);
                Complement(builder, layout, instruction, pc, next_pc, target);
                break;
            case Op::XCHG:
                if (Memory(instruction.ops[0]) || Memory(instruction.ops[1]))
                    pending.Flush(builder);
                Exchange(builder, layout, instruction, pc, next_pc, target);
                break;
            case Op::CMPXCHG:
                CompareExchange(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::XADD:
                ExchangeAdd(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::MOV:
                if (Memory(instruction.ops[0]) || Memory(instruction.ops[1]))
                    pending.Flush(builder);
                Move(builder, layout, instruction, pc, next_pc, target);
                break;
            case Op::MOVZX:
            case Op::MOVSX:
            case Op::MOVSXD:
                if (Memory(instruction.ops[1])) pending.Flush(builder);
                ExtendedMove(builder, layout, instruction, pc, next_pc, target);
                break;
            case Op::SETO:
            case Op::SETNO:
            case Op::SETB:
            case Op::SETAE:
            case Op::SETZ:
            case Op::SETNZ:
            case Op::SETBE:
            case Op::SETA:
            case Op::SETS:
            case Op::SETNS:
            case Op::SETP:
            case Op::SETNP:
            case Op::SETL:
            case Op::SETGE:
            case Op::SETLE:
            case Op::SETG:
                if (Memory(instruction.ops[0]))
                    pending.Flush(builder);
                else
                    pending.Observe(builder, instruction);
                ConditionalSet(builder, layout, instruction, pc, next_pc, target);
                break;
            case Op::CMOVO:
            case Op::CMOVNO:
            case Op::CMOVB:
            case Op::CMOVAE:
            case Op::CMOVZ:
            case Op::CMOVNZ:
            case Op::CMOVBE:
            case Op::CMOVA:
            case Op::CMOVS:
            case Op::CMOVNS:
            case Op::CMOVP:
            case Op::CMOVNP:
            case Op::CMOVL:
            case Op::CMOVGE:
            case Op::CMOVLE:
            case Op::CMOVG:
                if (Memory(instruction.ops[1]))
                    pending.Flush(builder);
                else
                    pending.Observe(builder, instruction);
                ConditionalMove(builder, layout, instruction, pc, next_pc, target);
                break;
            case Op::LEA:
                Lea(builder, layout, instruction, next_pc, target);
                break;
            case Op::NOP:
                Require(builder.Nop(Type::Void, {}));
                break;
            case Op::PREFETCH:
            case Op::PREFETCHW:
            case Op::PREFETCHT0:
            case Op::PREFETCHT1:
            case Op::PREFETCHT2:
            case Op::PREFETCHNTA:
                OneOperand(instruction);
                if (!Memory(instruction.ops[0])) Unsupported();
                // A non-faulting cache hint observes neither the memory
                // nor its address registers and needs no IR operation.
                break;
            case Op::MOVNTPS:
            case Op::MOVNTPD:
            case Op::MOVNTDQ:
                NonTemporalVectorStore(builder, layout, instruction, pending, pc, next_pc, target);
                break;
            case Op::SFENCE:
                OneOperand(instruction);
                if (instruction.ops[0].type != GuestOperandKind::None) Unsupported();
                Require(builder.MemoryStoreFence(Type::Void, {}));
                break;
            case Op::PUSH:
            case Op::POP:
            case Op::LEAVE:
                pending.Flush(builder);
                PrepareFaultPC(builder, layout, pc, target);
                if (instruction.opcode == Op::PUSH)
                    Push(builder, layout, instruction, next_pc, target);
                else if (instruction.opcode == Op::POP)
                    Pop(builder, layout, instruction, next_pc, target);
                else
                    Leave(builder, layout, instruction);
                break;
            case Op::CALL:
                Call(builder, layout, instruction, pending, pc, next_pc, target, ended);
                break;
            case Op::JMP:
                Jump(builder, layout, instruction, pending, pc, next_pc, target, flow, ended);
                break;
            case Op::RET:
                Return(builder, layout, instruction, pending, pc, target, ended, bytes);
                break;
            case Op::SYSCALL:
                SystemCall(builder, layout, instruction, pending, next_pc, ended);
                break;
            case Op::UD2:
            case Op::INT_3:
                SoftwareException(builder, layout, instruction, pending, pc, next_pc, ended);
                break;
            case Op::LDMXCSR:
            case Op::STMXCSR:
                Mxcsr(builder, layout, instruction, pending, pc, next_pc, target, ended);
                break;
            case Op::CPUID:
                CpuQuery(builder, layout, instruction, pending, next_pc, ended);
                break;
            case Op::VPCMPESTRI:
            case Op::VPCMPESTRM:
            case Op::VPCMPISTRI:
            case Op::VPCMPISTRM:
                if (!Sse42Instruction(builder, layout, instruction, pending, pc, next_pc, target))
                    Unsupported();
                break;
            default:
                // Register-only vectors neither read nor write EFLAGS.
                // Preserve pending flags until a real observer; memory
                // forms must still expose the old flags on a fault.
                if (Memory(instruction.ops[0]) || Memory(instruction.ops[1]))
                    pending.Flush(builder);
                if (instruction.vector_bytes) {
                    if (Avx(builder, layout, instruction, pending, pc, next_pc, target)) break;
                    Unsupported();
                }
                if (Mmx(builder, layout, instruction, pending, pc, next_pc, target)) break;
                if (Vector(builder, layout, instruction, pc, next_pc, target)) break;
                if (Sse42Instruction(builder, layout, instruction, pending, pc, next_pc, target))
                    break;
                if (CryptoInstruction(builder, layout, instruction, pending, pc, next_pc, target))
                    break;
                if (BmiInstruction(builder, layout, instruction, pending, pc, next_pc, target))
                    break;
                if (UserInstruction(builder, layout, instruction, pending, pc, next_pc, target))
                    break;
                if (SystemInstruction(
                            builder, layout, instruction, pending, pc, next_pc, target, ended))
                    break;
                Unsupported();
        }
    return consumed;
}
