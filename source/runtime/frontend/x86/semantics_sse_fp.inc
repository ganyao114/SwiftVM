// Legacy XMM numerical FP, sharing the active FP environment and vector owner.
static bool SseFloating(Emitter& builder,
                        StateLayout layout,
                        const InstructionData& instruction,
                        u64 pc,
                        u64 next_pc,
                        TargetInfo target) {
    unsigned bits = 32, group{}, kind{}, relation{};
    bool scalar{}, subtract{};
    switch (instruction.opcode) {
        case Op::ADDSS:
            group = 1;
            break;
        case Op::SUBSS:
            group = 1;
            kind = 1;
            break;
        case Op::MULSS:
            group = 1;
            kind = 2;
            break;
        case Op::DIVSS:
            group = 1;
            kind = 3;
            break;
        case Op::MINSD:
            bits = 64;
            [[fallthrough]];
        case Op::MINSS:
            scalar = true;
            group = 2;
            break;
        case Op::MINPD:
            bits = 64;
            [[fallthrough]];
        case Op::MINPS:
            group = 2;
            break;
        case Op::MAXSD:
            bits = 64;
            [[fallthrough]];
        case Op::MAXSS:
            scalar = true;
            group = 2;
            kind = 1;
            break;
        case Op::MAXPD:
            bits = 64;
            [[fallthrough]];
        case Op::MAXPS:
            group = 2;
            kind = 1;
            break;
        case Op::SQRTSD:
            bits = 64;
            [[fallthrough]];
        case Op::SQRTSS:
            scalar = true;
            group = 3;
            break;
        case Op::SQRTPD:
            bits = 64;
            [[fallthrough]];
        case Op::SQRTPS:
            group = 3;
            break;
        case Op::RCPSS:
            scalar = true;
            [[fallthrough]];
        case Op::RCPPS:
            group = 3;
            kind = 1;
            break;
        case Op::RSQRTSS:
            scalar = true;
            [[fallthrough]];
        case Op::RSQRTPS:
            group = 3;
            kind = 2;
            break;
        case Op::CVTDQ2PS:
            group = 4;
            kind = 0;
            break;
        case Op::CVTPS2DQ:
            group = 4;
            kind = 2;
            break;
        case Op::CVTTPS2DQ:
            group = 4;
            kind = 3;
            break;
        case Op::CVTPD2DQ:
            group = 4;
            kind = 4;
            break;
        case Op::CVTPS2PD:
            group = 4;
            kind = 6;
            break;
        case Op::CVTPD2PS:
            group = 4;
            kind = 7;
            break;
        case Op::CVTSS2SD:
            bits = 32;
            group = 5;
            kind = 0;
            break;
        case Op::CVTSD2SS:
            bits = 64;
            group = 5;
            kind = 1;
            break;
        case Op::CMPEQSD:
            bits = 64;
            [[fallthrough]];
        case Op::CMPEQSS:
            group = 6;
            relation = 2;
            break;
        case Op::CMPLTSD:
            bits = 64;
            [[fallthrough]];
        case Op::CMPLTSS:
            group = 6;
            relation = 1;
            break;
        case Op::CMPLESD:
            bits = 64;
            [[fallthrough]];
        case Op::CMPLESS:
            group = 6;
            relation = 3;
            break;
        case Op::CMPUNORDSD:
            bits = 64;
            [[fallthrough]];
        case Op::CMPUNORDSS:
            group = 6;
            relation = 8;
            break;
        case Op::CMPNEQSD:
            bits = 64;
            [[fallthrough]];
        case Op::CMPNEQSS:
            group = 6;
            relation = 13;
            break;
        case Op::CMPNLTSD:
            bits = 64;
            [[fallthrough]];
        case Op::CMPNLTSS:
            group = 6;
            relation = 14;
            break;
        case Op::CMPNLESD:
            bits = 64;
            [[fallthrough]];
        case Op::CMPNLESS:
            group = 6;
            relation = 12;
            break;
        case Op::CMPORDSD:
            bits = 64;
            [[fallthrough]];
        case Op::CMPORDSS:
            group = 6;
            relation = 7;
            break;
        case Op::HADDPD:
            bits = 64;
            [[fallthrough]];
        case Op::HADDPS:
            group = 7;
            break;
        case Op::HSUBPD:
            bits = 64;
            [[fallthrough]];
        case Op::HSUBPS:
            group = 7;
            subtract = true;
            break;
        case Op::ADDSUBPD:
            bits = 64;
            [[fallthrough]];
        case Op::ADDSUBPS:
            group = 8;
            break;
        case Op::DPPD:
            bits = 64;
            [[fallthrough]];
        case Op::DPPS:
            group = 9;
            break;
        case Op::ROUNDSD:
            bits = 64;
            [[fallthrough]];
        case Op::ROUNDSS:
            scalar = true;
            group = 10;
            break;
        case Op::ROUNDPD:
            bits = 64;
            [[fallthrough]];
        case Op::ROUNDPS:
            group = 10;
            break;
        default:
            return false;
    }
    if (instruction.ops[3].type != GuestOperandKind::None) Unsupported();
    const auto destination = instruction.ops[0], source = instruction.ops[1];
    const bool controlled = group == 9 || group == 10;
    if (!VRegister(destination) ||
        (controlled ? instruction.ops[2].type != GuestOperandKind::Immediate ||
                              instruction.ops[2].size != 8
                    : instruction.ops[2].type != GuestOperandKind::None))
        Unsupported();
    if (group == 1 || group == 5 || group == 6) scalar = true;
    const auto previous = VRead(builder, layout, instruction, destination, pc, next_pc, target);
    Value input;
    if (scalar && !VRegister(source)) {
        if (!Memory(source) || source.size != bits) Unsupported();
        input = Require(builder.VecDup64(
                Type::V128,
                {Operand::ValueOf(VScalarRead(
                        builder, layout, instruction, source, bits, pc, next_pc, target))}));
    } else if (group == 4 && kind == 6 && Memory(source)) {
        if (source.size != 64) Unsupported();
        input = Require(builder.VecDup64(
                Type::V128,
                {Operand::ValueOf(VScalarRead(
                        builder, layout, instruction, source, 64, pc, next_pc, target))}));
    } else
        input = VRead(builder, layout, instruction, source, pc, next_pc, target, true);
    Value result;
    const auto lane = Operand::Immediate(bits, Type::U8);
    switch (group) {
        case 1:
            if (kind == 0)
                result = Require(builder.VecFAddScalar32(
                        Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input)}));
            else if (kind == 1)
                result = Require(builder.VecFSubScalar32(
                        Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input)}));
            else if (kind == 2)
                result = Require(builder.VecFMulScalar32(
                        Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input)}));
            else
                result = Require(builder.VecFDivScalar32(
                        Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input)}));
            break;
        case 2:
            result = Require(builder.VecFMinMax(Type::V128,
                                                {Operand::ValueOf(previous),
                                                 Operand::ValueOf(input),
                                                 lane,
                                                 Operand::Immediate(kind, Type::U8),
                                                 Operand::Immediate(scalar, Type::U8)}));
            break;
        case 3:
            result = Require(builder.VecFUnary(Type::V128,
                                               {Operand::ValueOf(previous),
                                                Operand::ValueOf(input),
                                                lane,
                                                Operand::Immediate(kind, Type::U8),
                                                Operand::Immediate(scalar, Type::U8)}));
            break;
        case 4:
            result = Require(builder.VecFCvtPacked(
                    Type::V128, {Operand::ValueOf(input), Operand::Immediate(kind, Type::U8)}));
            break;
        case 5: {
            const auto raw = VLane(builder, input, bits, 0);
            const auto out_bits = bits == 32 ? 64 : 32;
            const auto converted = Require(builder.VecFCvtScalar(
                    ScalarType(out_bits),
                    {Operand::ValueOf(raw, ScalarType(bits)), Operand::Immediate(bits, Type::U8)}));
            result = VReplaceLane(builder, previous, converted, out_bits, 0);
            break;
        }
        case 6:
            result = Require(builder.VecFCmpMask(Type::V128,
                                                 {Operand::ValueOf(previous),
                                                  Operand::ValueOf(input),
                                                  lane,
                                                  Operand::Immediate(relation, Type::U8),
                                                  Operand::Immediate(1, Type::U8)}));
            break;
        case 7: {
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
            result =
                    subtract
                            ? Require(builder.VecFSub(
                                      Type::V128, {Operand::ValueOf(a), Operand::ValueOf(b), lane}))
                            : Require(builder.VecFAdd(
                                      Type::V128,
                                      {Operand::ValueOf(a), Operand::ValueOf(b), lane}));
            break;
        }
        case 8: {
            const auto added = Require(builder.VecFAdd(
                    Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input), lane}));
            const auto subtracted = Require(builder.VecFSub(
                    Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input), lane}));
            result = VBlend(builder, added, subtracted, VLaneMask(builder, bits, 5));
            break;
        }
        case 9: {
            const auto immediate =
                    static_cast<unsigned>(static_cast<std::uint8_t>(instruction.immediate));
            const auto selection = VLaneMask(builder, bits, immediate >> 4);
            const auto a = Require(builder.VecAnd(
                    Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(selection)}));
            const auto b = Require(builder.VecAnd(
                    Type::V128, {Operand::ValueOf(input), Operand::ValueOf(selection)}));
            auto sum = Require(
                    builder.VecFMul(Type::V128, {Operand::ValueOf(a), Operand::ValueOf(b), lane}));
            for (unsigned step = 0; step < (bits == 32 ? 2u : 1u); ++step) {
                const auto even = Require(builder.VecUnzip(Type::V128,
                                                           {Operand::ValueOf(sum),
                                                            Operand::ValueOf(sum),
                                                            lane,
                                                            Operand::Immediate(0, Type::U8)}));
                const auto odd = Require(builder.VecUnzip(Type::V128,
                                                          {Operand::ValueOf(sum),
                                                           Operand::ValueOf(sum),
                                                           lane,
                                                           Operand::Immediate(1, Type::U8)}));
                sum = Require(builder.VecFAdd(
                        Type::V128, {Operand::ValueOf(even), Operand::ValueOf(odd), lane}));
            }
            const auto mask = VLaneMask(builder, bits, immediate);
            result = Require(
                    builder.VecAnd(Type::V128, {Operand::ValueOf(sum), Operand::ValueOf(mask)}));
            break;
        }
        case 10: {
            const auto immediate =
                    static_cast<unsigned>(static_cast<std::uint8_t>(instruction.immediate));
            const auto round = [&](unsigned mode) {
                return Require(builder.VecFRoundInt(Type::V128,
                                                    {Operand::ValueOf(previous),
                                                     Operand::ValueOf(input),
                                                     lane,
                                                     Operand::Immediate(mode, Type::U8),
                                                     Operand::Immediate(scalar, Type::U8)}));
            };
            if (!(immediate & 4))
                result = round(immediate & 3);
            else {
                if (layout.mxcsr_offset > UINT32_MAX - 3) Unsupported();
                const auto mxcsr = Require(builder.LoadUniform(
                        Type::U32, {Operand::Uniform(layout.mxcsr_offset, Type::U32)}));
                const auto mode = Require(builder.BitExtract(Type::U32,
                                                             {Operand::ValueOf(mxcsr),
                                                              Operand::Immediate(13, Type::U8),
                                                              Operand::Immediate(2, Type::U8)}));
                result = round(0);
                for (unsigned i = 1; i < 4; ++i) {
                    const auto mismatch = Require(builder.Xor(
                            Type::U32, {Operand::ValueOf(mode), Operand::Immediate(i, Type::U32)}));
                    const auto selected =
                            Require(builder.TestZero(Type::Bool, {Operand::ValueOf(mismatch)}));
                    const auto mask = Require(
                            builder.Neg(Type::U64, {Operand::ValueOf(selected, Type::U64)}));
                    const auto duplicated =
                            Require(builder.VecDup64(Type::V128, {Operand::ValueOf(mask)}));
                    result = VBlend(builder, result, round(i), duplicated);
                }
            }
            break;
        }
        default:
            Unsupported();
    }
    VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
    return true;
}
