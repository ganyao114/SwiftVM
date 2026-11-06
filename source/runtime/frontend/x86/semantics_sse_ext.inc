// Legacy XMM integer/data movement extensions in the one Semantics owner.
static Value VConstant(Emitter& builder, u64 low, u64 high) {
    return Require(builder.VecLoadConst(Type::V128,
            {Operand::Immediate(low, Type::U64), Operand::Immediate(high, Type::U64)}));
}
static Value VLane(Emitter& builder, Value vector, unsigned bits, unsigned lane) {
    auto value = Require(builder.VecExtract64(Type::U64,
            {Operand::ValueOf(vector), Operand::Immediate(lane * bits / 64, Type::U8)}));
    if (bits < 64) value = Require(builder.BitExtract(ScalarType(bits),
            {Operand::ValueOf(value), Operand::Immediate(lane * bits % 64, Type::U8), Operand::Immediate(bits, Type::U8)}));
    return value;
}
static Value VReplaceLane(Emitter& builder, Value previous, Value scalar, unsigned bits, unsigned lane) {
    const auto qword = lane * bits / 64;
    auto value = scalar;
    if (bits < 64) {
        const auto old = Require(builder.VecExtract64(Type::U64,
                {Operand::ValueOf(previous), Operand::Immediate(qword, Type::U8)}));
        value = Require(builder.BitInsert(Type::U64,
                {Operand::ValueOf(old), Operand::ValueOf(scalar, ScalarType(bits)),
                 Operand::Immediate(lane * bits % 64, Type::U8), Operand::Immediate(bits, Type::U8)}));
    }
    const auto duplicate = Require(builder.VecDup64(Type::V128, {Operand::ValueOf(value)}));
    return Require(builder.VecZip(Type::V128,
            {Operand::ValueOf(qword ? previous : duplicate), Operand::ValueOf(qword ? duplicate : previous),
             Operand::Immediate(64, Type::U8), Operand::Immediate(qword ? 0 : 1, Type::U8)}));
}
static Value VBlend(Emitter& builder, Value left, Value right, Value mask) {
    const auto a = Require(builder.VecAndNot(Type::V128, {Operand::ValueOf(left), Operand::ValueOf(mask)}));
    const auto b = Require(builder.VecAnd(Type::V128, {Operand::ValueOf(right), Operand::ValueOf(mask)}));
    return Require(builder.VecOr(Type::V128, {Operand::ValueOf(a), Operand::ValueOf(b)}));
}
static Value VLaneMask(Emitter& builder, unsigned bits, unsigned control) {
    u64 low{}, high{};
    for (unsigned i = 0; i < 128 / bits; ++i) if ((control >> i) & 1u) {
        const auto mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        (i * bits < 64 ? low : high) |= mask << (i * bits % 64);
    }
    return VConstant(builder, low, high);
}
static Value VMultiplyAddBytes(Emitter& builder, Value a, Value b) {
    std::array<Value, 2> product{};
    const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
    for (unsigned half = 0; half < 2; ++half) {
        auto x = a, y = b;
        if (half) {
            x = Require(builder.VecByteShift(Type::V128, {Operand::ValueOf(x), Operand::ValueOf(zero), Operand::Immediate(8, Type::U8), Operand::Immediate(0, Type::U8)}));
            y = Require(builder.VecByteShift(Type::V128, {Operand::ValueOf(y), Operand::ValueOf(zero), Operand::Immediate(8, Type::U8), Operand::Immediate(0, Type::U8)}));
        }
        const auto unsigned_bytes = Require(builder.VecExtend(Type::V128,
                {Operand::ValueOf(x), Operand::Immediate(8, Type::U8), Operand::Immediate(16, Type::U8), Operand::Immediate(0, Type::U8)}));
        const auto signed_bytes = Require(builder.VecExtend(Type::V128,
                {Operand::ValueOf(y), Operand::Immediate(8, Type::U8), Operand::Immediate(16, Type::U8), Operand::Immediate(1, Type::U8)}));
        product[half] = Require(builder.VecMul(Type::V128,
                {Operand::ValueOf(unsigned_bytes), Operand::ValueOf(signed_bytes), Operand::Immediate(16, Type::U8)}));
    }
    const auto even = Require(builder.VecUnzip(Type::V128,
            {Operand::ValueOf(product[0]), Operand::ValueOf(product[1]), Operand::Immediate(16, Type::U8), Operand::Immediate(0, Type::U8)}));
    const auto odd = Require(builder.VecUnzip(Type::V128,
            {Operand::ValueOf(product[0]), Operand::ValueOf(product[1]), Operand::Immediate(16, Type::U8), Operand::Immediate(1, Type::U8)}));
    return Require(builder.VecSatAdd(Type::V128,
            {Operand::ValueOf(even), Operand::ValueOf(odd), Operand::Immediate(16, Type::U8), Operand::Immediate(1, Type::U8)}));
}
static Value VSadWindow(Emitter& builder, Value window_source, Value needle_source, unsigned immediate) {
    Value result;
    // Each table control directly gathers eight zero-extended
    // byte lanes; the four unsigned absolute differences fit U16.
    for (unsigned j = 0; j < 4; ++j) {
        u64 indices[2]{};
        for (unsigned i = 0; i < 8; ++i)
            indices[i / 4] |= u64{(((immediate >> 2) & 1) * 4 + i + j) | 0x8000u} << ((i % 4) * 16);
        const auto window_control = VConstant(builder, indices[0], indices[1]);
        const auto window = Require(builder.VecTableLookup8(Type::V128,
                {Operand::ValueOf(window_source), Operand::ValueOf(window_control)}));
        const auto needle_index = u64{((immediate & 3) * 4 + j) | 0x8000u} * UINT64_C(0x0001000100010001);
        const auto needle_control = VConstant(builder, needle_index, needle_index);
        const auto needle = Require(builder.VecTableLookup8(Type::V128,
                {Operand::ValueOf(needle_source), Operand::ValueOf(needle_control)}));
        const auto width = Operand::Immediate(16, Type::U8), unsigned_lanes = Operand::Immediate(0, Type::U8);
        const auto maximum = Require(builder.VecMax(Type::V128,
                {Operand::ValueOf(window), Operand::ValueOf(needle), width, unsigned_lanes}));
        const auto minimum = Require(builder.VecMin(Type::V128,
                {Operand::ValueOf(window), Operand::ValueOf(needle), width, unsigned_lanes}));
        const auto difference = Require(builder.VecSub(Type::V128,
                {Operand::ValueOf(maximum), Operand::ValueOf(minimum), width}));
        result = j == 0 ? difference : Require(builder.VecAdd(Type::V128,
                {Operand::ValueOf(result), Operand::ValueOf(difference), width}));
    }
    return result;
}
static bool SseExtended(Emitter& builder, StateLayout layout, const InstructionData& instruction,
                        u64 pc, u64 next_pc, TargetInfo target) {
    unsigned bits{}, sign{}, group{};
    bool subtract{}, high{};
    switch (instruction.opcode) {
        case Op::PADDSB: sign = 1; [[fallthrough]]; case Op::PADDUSB: bits = 8; group = 1; break;
        case Op::PADDSW: sign = 1; [[fallthrough]]; case Op::PADDUSW: bits = 16; group = 1; break;
        case Op::PSUBSB: sign = 1; [[fallthrough]]; case Op::PSUBUSB: bits = 8; group = 1; subtract = true; break;
        case Op::PSUBSW: sign = 1; [[fallthrough]]; case Op::PSUBUSW: bits = 16; group = 1; subtract = true; break;
        case Op::PAVGB: bits = 8; group = 2; break; case Op::PAVGW: bits = 16; group = 2; break;
        case Op::PMINSB: sign = 1; bits = 8; group = 3; break;
        case Op::PMINSW: sign = 1; bits = 16; group = 3; break;
        case Op::PMINSD: sign = 1; bits = 32; group = 3; break;
        case Op::PMINUW: bits = 16; group = 3; break; case Op::PMINUD: bits = 32; group = 3; break;
        case Op::PMAXSB: sign = 1; bits = 8; group = 3; high = true; break;
        case Op::PMAXSW: sign = 1; bits = 16; group = 3; high = true; break;
        case Op::PMAXSD: sign = 1; bits = 32; group = 3; high = true; break;
        case Op::PMAXUB: bits = 8; group = 3; high = true; break;
        case Op::PMAXUW: bits = 16; group = 3; high = true; break;
        case Op::PMAXUD: bits = 32; group = 3; high = true; break;
        case Op::PACKSSWB: bits = 16; group = 4; break;
        case Op::PACKSSDW: bits = 32; group = 4; break;
        case Op::PACKUSDW: bits = 32; sign = 1; group = 4; break;
        case Op::PMULHW: sign = 1; [[fallthrough]]; case Op::PMULHUW: group = 5; break;
        case Op::PMULDQ: group = 6; break;
        case Op::PSADBW: group = 7; break;
        case Op::PCMPEQQ: group = 8; break; case Op::PCMPGTQ: group = 8; high = true; break;
        case Op::PSHUFB: group = 9; break;
        case Op::PSHUFHW: group = 10; break;
        case Op::PEXTRW: bits = 16; group = 11; break;
        case Op::PEXTRB: bits = 8; group = 11; break;
        case Op::PEXTRD: case Op::EXTRACTPS: bits = 32; group = 11; break;
        case Op::PEXTRQ: bits = 64; group = 11; break;
        case Op::PINSRW: bits = 16; group = 12; break;
        case Op::PINSRB: bits = 8; group = 12; break;
        case Op::PINSRD: bits = 32; group = 12; break;
        case Op::PINSRQ: bits = 64; group = 12; break;
        case Op::UNPCKLPS: bits = 32; group = 13; break;
        case Op::UNPCKHPS: bits = 32; group = 13; high = true; break;
        case Op::UNPCKLPD: bits = 64; group = 13; break;
        case Op::UNPCKHPD: bits = 64; group = 13; high = true; break;
        case Op::SHUFPD: group = 14; break;
        case Op::MOVSLDUP: group = 15; break; case Op::MOVSHDUP: group = 15; high = true; break;
        case Op::MOVDDUP: group = 16; break;
        case Op::PABSB: bits = 8; group = 17; break;
        case Op::PABSW: bits = 16; group = 17; break;
        case Op::PABSD: bits = 32; group = 17; break;
        case Op::PSIGNB: bits = 8; group = 18; break;
        case Op::PSIGNW: bits = 16; group = 18; break;
        case Op::PSIGND: bits = 32; group = 18; break;
        case Op::PHADDW: bits = 16; group = 19; break;
        case Op::PHADDD: bits = 32; group = 19; break;
        case Op::PHSUBW: bits = 16; group = 19; subtract = true; break;
        case Op::PHSUBD: bits = 32; group = 19; subtract = true; break;
        case Op::PHADDSW: bits = 16; group = 19; sign = 1; break;
        case Op::PHSUBSW: bits = 16; group = 19; sign = 1; subtract = true; break;
        case Op::PMULHRSW: group = 20; break;
        case Op::PBLENDW: bits = 16; group = 21; break;
        case Op::BLENDPS: bits = 32; group = 21; break;
        case Op::BLENDPD: bits = 64; group = 21; break;
        case Op::PBLENDVB: bits = 8; group = 22; break;
        case Op::BLENDVPS: bits = 32; group = 22; break;
        case Op::BLENDVPD: bits = 64; group = 22; break;
        case Op::INSERTPS: group = 23; break;
        case Op::PHMINPOSUW: group = 24; break;
        case Op::LDDQU: group = 25; break;
        case Op::MOVNTDQA: group = 25; break;
        case Op::MPSADBW: group = 26; break;
        case Op::PMADDUBSW: group = 27; break;
        default: return false;
    }
    if (layout.fprs_offset == UINT32_MAX || instruction.ops[3].type != GuestOperandKind::None) Unsupported();
    const auto destination = instruction.ops[0], source = instruction.ops[1];
    const bool control = group == 10 || group == 11 || group == 12 || group == 14 || group == 21 || group == 23 || group == 26;
    if (control ? instruction.ops[2].type != GuestOperandKind::Immediate || instruction.ops[2].size != 8
                : instruction.ops[2].type != GuestOperandKind::None) Unsupported();
    const auto immediate = static_cast<unsigned>(static_cast<std::uint8_t>(instruction.immediate));
    if (group == 11) {
        if (!VRegister(source)) Unsupported();
        const auto input = VRead(builder, layout, instruction, source, pc, next_pc, target);
        const auto lane = immediate & (128 / bits - 1);
        const auto result = bits == 16 ? Require(builder.VecExtract16(Type::U32,
                {Operand::ValueOf(input), Operand::Immediate(lane, Type::U8)})) : VLane(builder, input, bits, lane);
        if (destination.type == GuestOperandKind::Register) {
            if (destination.size != (bits == 64 ? 64 : 32)) Unsupported();
            WriteRegister(builder, layout, destination, result);
        } else {
            if (!Memory(destination) || destination.size != bits) Unsupported();
            PrepareFaultPC(builder, layout, pc, target);
            u64 displacement{};
            const auto address = Address(builder, layout, instruction, destination, next_pc, target, {}, &displacement);
            StoreMemory(builder, address, result, bits, displacement);
        }
        return true;
    }
    if (!VRegister(destination)) Unsupported();
    const auto previous = VRead(builder, layout, instruction, destination, pc, next_pc, target);
    Value result;
    if (group == 12) {
        auto narrow = source;
        if (Memory(narrow)) narrow.size = static_cast<std::uint16_t>(bits);
        if ((!Memory(narrow) && narrow.type != GuestOperandKind::Register) || VRegister(narrow) ||
                (narrow.type == GuestOperandKind::Register && narrow.size != (bits == 64 ? 64 : 32))) Unsupported();
        if (Memory(narrow)) PrepareFaultPC(builder, layout, pc, target);
        const auto scalar = ReadData(builder, layout, instruction, narrow, next_pc, target);
        const auto lane = immediate & (128 / bits - 1);
        result = bits == 16 ? Require(builder.VecInsert16(Type::V128,
                {Operand::ValueOf(previous), Operand::ValueOf(scalar, Type::U16), Operand::Immediate(lane, Type::U8)}))
                : VReplaceLane(builder, previous, scalar, bits, lane);
    } else if (group == 16) {
        const auto scalar = VScalarRead(builder, layout, instruction, source, 64, pc, next_pc, target);
        result = Require(builder.VecDup64(Type::V128, {Operand::ValueOf(scalar)}));
    } else if (group == 23) {
        Value scalar;
        if (VRegister(source)) scalar = VLane(builder, VRead(builder, layout, instruction, source, pc, next_pc, target), 32, immediate >> 6);
        else {
            if (!Memory(source) || source.size != 32) Unsupported();
            scalar = VScalarRead(builder, layout, instruction, source, 32, pc, next_pc, target);
        }
        result = VReplaceLane(builder, previous, scalar, 32, (immediate >> 4) & 3);
        if (immediate & 15) {
            const auto mask = VLaneMask(builder, 32, ~immediate & 15);
            result = Require(builder.VecAnd(Type::V128, {Operand::ValueOf(result), Operand::ValueOf(mask)}));
        }
    } else {
        if (instruction.opcode == Op::MOVNTDQA && !Memory(source)) Unsupported();
        const auto input = VRead(builder, layout, instruction, source, pc, next_pc, target,
                                 group != 25 || instruction.opcode == Op::MOVNTDQA);
        const auto lane = Operand::Immediate(bits, Type::U8), signed_lane = Operand::Immediate(sign, Type::U8);
        switch (group) {
            case 1:
                result = subtract ? Require(builder.VecSatSub(Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input), lane, signed_lane}))
                                  : Require(builder.VecSatAdd(Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input), lane, signed_lane})); break;
            case 2: result = Require(builder.VecAvg(Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input), lane})); break;
            case 3:
                result = high ? Require(builder.VecMax(Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input), lane, signed_lane}))
                              : Require(builder.VecMin(Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input), lane, signed_lane})); break;
            case 4: result = Require(builder.VecPack(Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input), lane, signed_lane})); break;
            case 5: result = Require(builder.VecMulHigh16(Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input), signed_lane})); break;
            case 6: result = Require(builder.VecMulWiden(Type::V128,
                    {Operand::ValueOf(previous), Operand::ValueOf(input), Operand::Immediate(32, Type::U8), Operand::Immediate(1, Type::U8)})); break;
            case 7: result = Require(builder.VecAbsDiffSum8(Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input)})); break;
            case 8: result = high ? Require(builder.VecCmpGt(Type::V128,
                    {Operand::ValueOf(previous), Operand::ValueOf(input), Operand::Immediate(64, Type::U8)}))
                    : Require(builder.VecCmpEq(Type::V128,
                    {Operand::ValueOf(previous), Operand::ValueOf(input), Operand::Immediate(64, Type::U8)})); break;
            case 9: result = Require(builder.VecTableLookup8(Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input)})); break;
            case 10: result = Require(builder.VecShuffle16(Type::V128,
                    {Operand::ValueOf(input), Operand::Immediate(immediate, Type::U8), Operand::Immediate(1, Type::U8)})); break;
            case 13: result = Require(builder.VecZip(Type::V128,
                    {Operand::ValueOf(previous), Operand::ValueOf(input), lane, Operand::Immediate(high, Type::U8)})); break;
            case 14: {
                const auto a = (immediate & 1) * 2, b = ((immediate >> 1) & 1) * 2;
                result = Require(builder.VecShuffle32TwoSrc(Type::V128,
                        {Operand::ValueOf(previous), Operand::ValueOf(input), Operand::Immediate(a | ((a + 1) << 2) | (b << 4) | ((b + 1) << 6), Type::U8)})); break;
            }
            case 15: result = Require(builder.VecDupPairs32(Type::V128,
                    {Operand::ValueOf(input), Operand::Immediate(high, Type::U8)})); break;
            case 17: {
                const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
                const auto negative = Require(builder.VecSub(Type::V128, {Operand::ValueOf(zero), Operand::ValueOf(input), lane}));
                result = Require(builder.VecMax(Type::V128,
                        {Operand::ValueOf(input), Operand::ValueOf(negative), lane, Operand::Immediate(1, Type::U8)})); break;
            }
            case 18: {
                const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
                const auto negative = Require(builder.VecSub(Type::V128, {Operand::ValueOf(zero), Operand::ValueOf(previous), lane}));
                const auto negmask = Require(builder.VecCmpGt(Type::V128, {Operand::ValueOf(zero), Operand::ValueOf(input), lane}));
                const auto posmask = Require(builder.VecCmpGt(Type::V128, {Operand::ValueOf(input), Operand::ValueOf(zero), lane}));
                const auto neg = Require(builder.VecAnd(Type::V128, {Operand::ValueOf(negative), Operand::ValueOf(negmask)}));
                const auto pos = Require(builder.VecAnd(Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(posmask)}));
                result = Require(builder.VecOr(Type::V128, {Operand::ValueOf(neg), Operand::ValueOf(pos)})); break;
            }
            case 19: {
                const auto a = Require(builder.VecUnzip(Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input), lane, Operand::Immediate(0, Type::U8)}));
                const auto b = Require(builder.VecUnzip(Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(input), lane, Operand::Immediate(1, Type::U8)}));
                if (sign) result = subtract ? Require(builder.VecSatSub(Type::V128, {Operand::ValueOf(a), Operand::ValueOf(b), lane, signed_lane}))
                                           : Require(builder.VecSatAdd(Type::V128, {Operand::ValueOf(a), Operand::ValueOf(b), lane, signed_lane}));
                else result = subtract ? Require(builder.VecSub(Type::V128, {Operand::ValueOf(a), Operand::ValueOf(b), lane}))
                                       : Require(builder.VecAdd(Type::V128, {Operand::ValueOf(a), Operand::ValueOf(b), lane}));
                break;
            }
            case 20: {
                Value halves[2];
                const auto rounding = VConstant(builder, UINT64_C(0x400000004000), UINT64_C(0x400000004000));
                for (unsigned half = 0; half < 2; ++half) {
                    const auto a = Require(builder.VecZip(Type::V128, {Operand::ValueOf(previous), Operand::ValueOf(previous), Operand::Immediate(16, Type::U8), Operand::Immediate(half, Type::U8)}));
                    const auto b = Require(builder.VecZip(Type::V128, {Operand::ValueOf(input), Operand::ValueOf(input), Operand::Immediate(16, Type::U8), Operand::Immediate(half, Type::U8)}));
                    const auto product = Require(builder.VecMulWiden(Type::V128, {Operand::ValueOf(a), Operand::ValueOf(b), Operand::Immediate(16, Type::U8), Operand::Immediate(1, Type::U8)}));
                    const auto rounded = Require(builder.VecAdd(Type::V128, {Operand::ValueOf(product), Operand::ValueOf(rounding), Operand::Immediate(32, Type::U8)}));
                    halves[half] = Require(builder.VecShiftRightArithmeticImm(Type::V128, {Operand::ValueOf(rounded), Operand::Immediate(15, Type::U8), Operand::Immediate(32, Type::U8)}));
                }
                result = Require(builder.VecUnzip(Type::V128, {Operand::ValueOf(halves[0]), Operand::ValueOf(halves[1]), Operand::Immediate(16, Type::U8), Operand::Immediate(0, Type::U8)})); break;
            }
            case 21: result = VBlend(builder, previous, input, VLaneMask(builder, bits, immediate)); break;
            case 22: {
                auto mask_operand = destination; mask_operand.index = Register::XMM0;
                const auto rawmask = VRead(builder, layout, instruction, mask_operand, pc, next_pc, target);
                const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
                const auto mask = Require(builder.VecCmpGt(Type::V128, {Operand::ValueOf(zero), Operand::ValueOf(rawmask), lane}));
                result = VBlend(builder, previous, input, mask); break;
            }
            case 24: {
                auto minimum = Require(builder.VecExtract16(Type::U64, {Operand::ValueOf(input), Operand::Immediate(0, Type::U8)}));
                auto index = Constant(builder, 0);
                for (unsigned i = 1; i < 8; ++i) {
                    const auto value = Require(builder.VecExtract16(Type::U64, {Operand::ValueOf(input), Operand::Immediate(i, Type::U8)}));
                    const auto difference = Require(builder.Sub(Type::U64, {Operand::ValueOf(value), Operand::ValueOf(minimum)}));
                    const auto less = Require(builder.BitExtract(Type::U64, {Operand::ValueOf(difference), Operand::Immediate(63, Type::U8), Operand::Immediate(1, Type::U8)}));
                    minimum = Require(builder.Select(Type::U64, {Operand::ValueOf(less), Operand::ValueOf(value), Operand::ValueOf(minimum)}));
                    const auto position = Constant(builder, i);
                    index = Require(builder.Select(Type::U64, {Operand::ValueOf(less), Operand::ValueOf(position), Operand::ValueOf(index)}));
                }
                const auto packed = Require(builder.BitInsert(Type::U64, {Operand::ValueOf(minimum), Operand::ValueOf(index), Operand::Immediate(16, Type::U8), Operand::Immediate(3, Type::U8)}));
                result = VLowScalar(builder, packed, 64); break;
            }
            case 25: result = input; break;
            case 26: result = VSadWindow(builder, previous, input, immediate); break;
            case 27: result = VMultiplyAddBytes(builder, previous, input); break;
            default: Unsupported();
        }
    }
    VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
    return true;
}
