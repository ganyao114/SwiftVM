// Complete x86 instruction semantics; optional host crypto belongs only to
// the numerical/backend consumers of these neutral vector operations.
static bool CryptoInstruction(Emitter& builder,
                              StateLayout layout,
                              const InstructionData& instruction,
                              PendingFlags& pending,
                              u64 pc,
                              u64 next_pc,
                              TargetInfo target) {
    unsigned kind{};
    switch (instruction.opcode) {
        case Op::AESENC:
            kind = 1;
            break;
        case Op::AESENCLAST:
            kind = 2;
            break;
        case Op::AESDEC:
            kind = 3;
            break;
        case Op::AESDECLAST:
            kind = 4;
            break;
        case Op::AESIMC:
            kind = 5;
            break;
        case Op::AESKEYGENASSIST:
            kind = 6;
            break;
        case Op::PCLMULQDQ:
            kind = 7;
            break;
        case Op::SHA256MSG1:
            kind = 8;
            break;
        case Op::SHA256MSG2:
            kind = 9;
            break;
        case Op::SHA256RNDS2:
            kind = 10;
            break;
        default:
            return false;
    }
    const auto destination = instruction.ops[0], source = instruction.ops[1];
    const bool controlled = kind == 6 || kind == 7;
    if (!VRegister(destination) || destination.size != 128 ||
        (!VRegister(source) && !Memory(source)) || source.size != 128 ||
        instruction.ops[3].type != GuestOperandKind::None ||
        (controlled ? instruction.ops[2].type != GuestOperandKind::Immediate ||
                              instruction.ops[2].size != 8
                    : instruction.ops[2].type != GuestOperandKind::None))
        Unsupported();
    VOffset(layout, destination);
    // Validate all forms before an actual access. Legacy m128 operands retain
    // the same alignment and precise publication contract as other SSE ops.
    if (Memory(source)) pending.Flush(builder);
    const auto input = VRead(builder, layout, instruction, source, pc, next_pc, target, true);
    Value previous;
    if (kind != 5 && kind != 6)
        previous = VRead(builder, layout, instruction, destination, pc, next_pc, target);
    Value result;
    const auto a = Operand::ValueOf(previous), b = Operand::ValueOf(input);
    const auto control = Operand::Immediate(instruction.immediate & 255, Type::U8);
    switch (kind) {
        case 1:
            result = Require(builder.VecAesEnc(Type::V128, {a, b}));
            break;
        case 2:
            result = Require(builder.VecAesEncLast(Type::V128, {a, b}));
            break;
        case 3:
            result = Require(builder.VecAesDec(Type::V128, {a, b}));
            break;
        case 4:
            result = Require(builder.VecAesDecLast(Type::V128, {a, b}));
            break;
        case 5:
            result = Require(builder.VecAesImc(Type::V128, {b}));
            break;
        case 6:
            result = Require(builder.VecAesKeygenAssist(Type::V128, {b, control}));
            break;
        case 7:
            result = Require(builder.VecPclMul(Type::V128, {a, b, control}));
            break;
        case 8:
            result = Require(builder.VecSha256Msg1(Type::V128, {a, b}));
            break;
        case 9:
            result = Require(builder.VecSha256Msg2(Type::V128, {a, b}));
            break;
        case 10: {
            if (layout.fprs_offset > UINT32_MAX - 255) Unsupported();
            const auto key = builder.ReadXmm(layout.fprs_offset, 0);
            result = Require(builder.VecSha256Rnds2(Type::V128, {a, b, Operand::ValueOf(key)}));
            break;
        }
    }
    VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
    return true;
}
