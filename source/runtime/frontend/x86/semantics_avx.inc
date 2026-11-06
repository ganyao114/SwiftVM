// VEX semantics use the same sink and two transient V128 values per YMM.
static u32 AvxHighOffset(StateLayout layout, const DecodedOperand& reg) {
    if (!VRegister(reg) || layout.ymm_high_offset > UINT32_MAX - 255) Unsupported();
    return layout.ymm_high_offset + (reg.index - Register::XMM0) * 16;
}
static std::array<Value, 2> AvxRead(Emitter& builder,
                                    StateLayout layout,
                                    const InstructionData& instruction,
                                    const DecodedOperand& operand,
                                    unsigned bytes,
                                    u64 pc,
                                    u64 next_pc,
                                    TargetInfo target,
                                    bool aligned = false) {
    std::array<Value, 2> values{};
    if (VRegister(operand)) {
        values[0] = VRead(builder, layout, instruction, operand, pc, next_pc, target);
        if (bytes == 32)
            values[1] = Require(builder.LoadUniform(
                    Type::V128, {Operand::Uniform(AvxHighOffset(layout, operand), Type::V128)}));
        return values;
    }
    if (!Memory(operand) || !bytes || bytes > 32) Unsupported();
    PrepareFaultPC(builder, layout, pc, target);
    u64 displacement{};
    const auto address = Address(builder,
                                 layout,
                                 instruction,
                                 operand,
                                 next_pc,
                                 target,
                                 {},
                                 aligned ? nullptr : &displacement);
    if (aligned)
        Require(builder.CheckMemoryAlignment(
                Type::Void, {Operand::ValueOf(address), Operand::Immediate(bytes - 1, Type::U8)}));
    if (bytes < 16) {
        const auto scalar = LoadMemory(builder, ScalarType(bytes * 8), address, displacement);
        values[0] = Require(builder.VecDup64(Type::V128, {Operand::ValueOf(scalar)}));
    } else {
        values[0] = LoadMemory(builder, Type::V128, address, displacement);
        if (bytes == 32) values[1] = LoadMemory(builder, Type::V128, address, displacement + 16);
    }
    return values;
}
template <OpCode store = OpCode::StoreMemoryTSO>
static void AvxWrite(Emitter& builder,
                     StateLayout layout,
                     const InstructionData& instruction,
                     const DecodedOperand& destination,
                     unsigned bytes,
                     const std::array<Value, 2>& values,
                     u64 pc,
                     u64 next_pc,
                     TargetInfo target,
                     bool aligned = false) {
    if (VRegister(destination)) {
        const auto high = bytes == 32 ? values[1] : Require(builder.VecSharedZero(Type::V128, {}));
        // All source halves have already been captured before either write.
        VWrite(builder, layout, instruction, destination, values[0], pc, next_pc, target);
        Require(builder.StoreUniform(
                Type::Void,
                {Operand::Uniform(AvxHighOffset(layout, destination), Type::V128),
                 Operand::ValueOf(high)}));
        return;
    }
    if (!Memory(destination) || (bytes != 16 && bytes != 32)) Unsupported();
    PrepareFaultPC(builder, layout, pc, target);
    u64 displacement{};
    const auto address = Address(builder,
                                 layout,
                                 instruction,
                                 destination,
                                 next_pc,
                                 target,
                                 {},
                                 aligned ? nullptr : &displacement);
    if (aligned)
        Require(builder.CheckMemoryAlignment(
                Type::Void, {Operand::ValueOf(address), Operand::Immediate(bytes - 1, Type::U8)}));
    StoreMemory<store>(builder, address, values[0], 128, displacement);
    if (bytes == 32) StoreMemory<store>(builder, address, values[1], 128, displacement + 16);
}
static Value AvxReplicate(Emitter& builder, Value scalar, unsigned bits) {
    auto value =
            Require(builder.ZeroExtend64(Type::U64, {Operand::ValueOf(scalar, ScalarType(bits))}));
    if (bits != 64) {
        const auto scale = bits == 8    ? UINT64_C(0x0101010101010101)
                           : bits == 16 ? UINT64_C(0x0001000100010001)
                                        : UINT64_C(0x0000000100000001);
        value = Require(builder.Mul(
                Type::U64, {Operand::ValueOf(value), Operand::Immediate(scale, Type::U64)}));
    }
    return Require(builder.VecDup64(Type::V128, {Operand::ValueOf(value)}));
}
static Value AvxLane(Emitter& builder,
                     StateLayout layout,
                     Op opcode,
                     Value a,
                     Value b,
                     unsigned immediate = 0,
                     bool immediate_shift = false) {
    const auto left = Operand::ValueOf(a), right = Operand::ValueOf(b);
    switch (opcode) {
#define AVX_BINARY(name, ir)                                                                       \
    case Op::V##name:                                                                              \
        return Require(builder.ir(Type::V128, {left, right}));
        AVX_BINARY(PXOR, VecXor)
        AVX_BINARY(XORPS, VecXor)
        AVX_BINARY(XORPD, VecXor)
        AVX_BINARY(PAND, VecAnd)
        AVX_BINARY(ANDPS, VecAnd)
        AVX_BINARY(ANDPD, VecAnd)
        AVX_BINARY(POR, VecOr)
        AVX_BINARY(ORPS, VecOr)
        AVX_BINARY(ORPD, VecOr)
        AVX_BINARY(PSHUFB, VecTableLookup8)
        AVX_BINARY(PMADDWD, VecMadd16)
        AVX_BINARY(PSADBW, VecAbsDiffSum8)
#undef AVX_BINARY
        case Op::VPANDN:
        case Op::VANDNPS:
        case Op::VANDNPD:
            return Require(builder.VecAndNot(Type::V128, {right, left}));
        case Op::VPMADDUBSW:
            return VMultiplyAddBytes(builder, a, b);
        case Op::VMPSADBW:
            return VSadWindow(builder, a, b, immediate);
        case Op::VMOVHLPS:
        case Op::VMOVLHPS: {
            const bool high = opcode == Op::VMOVHLPS;
            return Require(builder.VecZip(Type::V128,
                                          {Operand::ValueOf(high ? b : a),
                                           Operand::ValueOf(high ? a : b),
                                           Operand::Immediate(64, Type::U8),
                                           Operand::Immediate(high, Type::U8)}));
        }
#define AVX_LANE(name, ir, bits)                                                                   \
    case Op::V##name:                                                                              \
        return Require(builder.ir(Type::V128, {left, right, Operand::Immediate(bits, Type::U8)}));
            AVX_LANE(PADDB, VecAdd, 8)
            AVX_LANE(PADDW, VecAdd, 16)
            AVX_LANE(PADDD, VecAdd, 32)
            AVX_LANE(PADDQ, VecAdd, 64)
            AVX_LANE(PSUBB, VecSub, 8)
            AVX_LANE(PSUBW, VecSub, 16)
            AVX_LANE(PSUBD, VecSub, 32)
            AVX_LANE(PSUBQ, VecSub, 64)
            AVX_LANE(PMULLW, VecMul, 16)
            AVX_LANE(PMULLD, VecMul, 32)
            AVX_LANE(PCMPEQB, VecCmpEq, 8)
            AVX_LANE(PCMPEQW, VecCmpEq, 16)
            AVX_LANE(PCMPEQD, VecCmpEq, 32)
            AVX_LANE(PCMPEQQ, VecCmpEq, 64)
            AVX_LANE(PCMPGTB, VecCmpGt, 8)
            AVX_LANE(PCMPGTW, VecCmpGt, 16)
            AVX_LANE(PCMPGTD, VecCmpGt, 32)
            AVX_LANE(PCMPGTQ, VecCmpGt, 64)
            AVX_LANE(PAVGB, VecAvg, 8)
            AVX_LANE(PAVGW, VecAvg, 16)
            AVX_LANE(ADDPS, VecFAdd, 32)
            AVX_LANE(ADDPD, VecFAdd, 64)
            AVX_LANE(SUBPS, VecFSub, 32)
            AVX_LANE(SUBPD, VecFSub, 64)
            AVX_LANE(MULPS, VecFMul, 32)
            AVX_LANE(MULPD, VecFMul, 64)
            AVX_LANE(DIVPS, VecFDiv, 32)
            AVX_LANE(DIVPD, VecFDiv, 64)
#undef AVX_LANE
#define AVX_SIGNED(name, ir, bits, sign)                                                           \
    case Op::V##name:                                                                              \
        return Require(builder.ir(Type::V128,                                                      \
                                  {left,                                                           \
                                   right,                                                          \
                                   Operand::Immediate(bits, Type::U8),                             \
                                   Operand::Immediate(sign, Type::U8)}));
            AVX_SIGNED(PADDSB, VecSatAdd, 8, 1)
            AVX_SIGNED(PADDSW, VecSatAdd, 16, 1)
            AVX_SIGNED(PADDUSB, VecSatAdd, 8, 0)
            AVX_SIGNED(PADDUSW, VecSatAdd, 16, 0)
            AVX_SIGNED(PSUBSB, VecSatSub, 8, 1)
            AVX_SIGNED(PSUBSW, VecSatSub, 16, 1)
            AVX_SIGNED(PSUBUSB, VecSatSub, 8, 0)
            AVX_SIGNED(PSUBUSW, VecSatSub, 16, 0)
            AVX_SIGNED(PMINUB, VecMin, 8, 0)
            AVX_SIGNED(PMINUW, VecMin, 16, 0)
            AVX_SIGNED(PMINUD, VecMin, 32, 0)
            AVX_SIGNED(PMINSB, VecMin, 8, 1)
            AVX_SIGNED(PMINSW, VecMin, 16, 1)
            AVX_SIGNED(PMINSD, VecMin, 32, 1)
            AVX_SIGNED(PMAXUB, VecMax, 8, 0)
            AVX_SIGNED(PMAXUW, VecMax, 16, 0)
            AVX_SIGNED(PMAXUD, VecMax, 32, 0)
            AVX_SIGNED(PMAXSB, VecMax, 8, 1)
            AVX_SIGNED(PMAXSW, VecMax, 16, 1)
            AVX_SIGNED(PMAXSD, VecMax, 32, 1)
            AVX_SIGNED(PACKSSWB, VecPack, 16, 0)
            AVX_SIGNED(PACKUSWB, VecPack, 16, 1)
            AVX_SIGNED(PACKSSDW, VecPack, 32, 0)
            AVX_SIGNED(PACKUSDW, VecPack, 32, 1)
            AVX_SIGNED(PMULUDQ, VecMulWiden, 32, 0)
            AVX_SIGNED(PMULDQ, VecMulWiden, 32, 1)
            AVX_SIGNED(PUNPCKLBW, VecZip, 8, 0)
            AVX_SIGNED(PUNPCKLWD, VecZip, 16, 0)
            AVX_SIGNED(PUNPCKLDQ, VecZip, 32, 0)
            AVX_SIGNED(PUNPCKLQDQ, VecZip, 64, 0)
            AVX_SIGNED(PUNPCKHBW, VecZip, 8, 1)
            AVX_SIGNED(PUNPCKHWD, VecZip, 16, 1)
            AVX_SIGNED(PUNPCKHDQ, VecZip, 32, 1)
            AVX_SIGNED(PUNPCKHQDQ, VecZip, 64, 1)
            AVX_SIGNED(UNPCKLPS, VecZip, 32, 0)
            AVX_SIGNED(UNPCKHPS, VecZip, 32, 1)
            AVX_SIGNED(UNPCKLPD, VecZip, 64, 0)
            AVX_SIGNED(UNPCKHPD, VecZip, 64, 1)
#undef AVX_SIGNED
        case Op::VPMULHW:
        case Op::VPMULHUW:
            return Require(builder.VecMulHigh16(
                    Type::V128,
                    {left, right, Operand::Immediate(opcode == Op::VPMULHW, Type::U8)}));
#define AVX_SCALAR(name, ir)                                                                       \
    case Op::V##name:                                                                              \
        return Require(builder.ir(Type::V128, {left, right}));
            AVX_SCALAR(ADDSS, VecFAddScalar32)
            AVX_SCALAR(SUBSS, VecFSubScalar32)
            AVX_SCALAR(MULSS, VecFMulScalar32)
            AVX_SCALAR(DIVSS, VecFDivScalar32)
            AVX_SCALAR(ADDSD, VecFAddScalar64)
            AVX_SCALAR(SUBSD, VecFSubScalar64)
            AVX_SCALAR(MULSD, VecFMulScalar64)
            AVX_SCALAR(DIVSD, VecFDivScalar64)
#undef AVX_SCALAR
        case Op::VMINPS:
        case Op::VMINPD:
        case Op::VMINSS:
        case Op::VMINSD:
        case Op::VMAXPS:
        case Op::VMAXPD:
        case Op::VMAXSS:
        case Op::VMAXSD: {
            const bool scalar = opcode == Op::VMINSS || opcode == Op::VMINSD ||
                                opcode == Op::VMAXSS || opcode == Op::VMAXSD;
            const bool wide = opcode == Op::VMINPD || opcode == Op::VMINSD ||
                              opcode == Op::VMAXPD || opcode == Op::VMAXSD;
            const bool maximum = opcode == Op::VMAXPS || opcode == Op::VMAXPD ||
                                 opcode == Op::VMAXSS || opcode == Op::VMAXSD;
            return Require(builder.VecFMinMax(Type::V128,
                                              {left,
                                               right,
                                               Operand::Immediate(wide ? 64 : 32, Type::U8),
                                               Operand::Immediate(maximum, Type::U8),
                                               Operand::Immediate(scalar, Type::U8)}));
        }
        case Op::VSQRTPS:
        case Op::VSQRTPD:
        case Op::VSQRTSS:
        case Op::VSQRTSD:
        case Op::VRCPPS:
        case Op::VRCPSS:
        case Op::VRSQRTPS:
        case Op::VRSQRTSS: {
            const bool scalar = opcode == Op::VSQRTSS || opcode == Op::VSQRTSD ||
                                opcode == Op::VRCPSS || opcode == Op::VRSQRTSS;
            const bool wide = opcode == Op::VSQRTPD || opcode == Op::VSQRTSD;
            const unsigned kind = opcode == Op::VRCPPS || opcode == Op::VRCPSS       ? 1
                                  : opcode == Op::VRSQRTPS || opcode == Op::VRSQRTSS ? 2
                                                                                     : 0;
            return Require(builder.VecFUnary(Type::V128,
                                             {left,
                                              right,
                                              Operand::Immediate(wide ? 64 : 32, Type::U8),
                                              Operand::Immediate(kind, Type::U8),
                                              Operand::Immediate(scalar, Type::U8)}));
        }
        case Op::VPSLLW:
        case Op::VPSLLD:
        case Op::VPSLLQ:
        case Op::VPSRLW:
        case Op::VPSRLD:
        case Op::VPSRLQ:
        case Op::VPSRAW:
        case Op::VPSRAD: {
            const bool l = opcode == Op::VPSLLW || opcode == Op::VPSLLD || opcode == Op::VPSLLQ;
            const bool arithmetic = opcode == Op::VPSRAW || opcode == Op::VPSRAD;
            const unsigned bits =
                    opcode == Op::VPSLLW || opcode == Op::VPSRLW || opcode == Op::VPSRAW ? 16
                    : opcode == Op::VPSLLQ || opcode == Op::VPSRLQ                       ? 64
                                                                                         : 32;
            const auto width = Operand::Immediate(bits, Type::U8);
            if (immediate_shift) {
                const auto count = Operand::Immediate(immediate, Type::U8);
                if (l) return Require(builder.VecShiftLeftImm(Type::V128, {left, count, width}));
                if (arithmetic)
                    return Require(
                            builder.VecShiftRightArithmeticImm(Type::V128, {left, count, width}));
                return Require(builder.VecShiftRightImm(Type::V128, {left, count, width}));
            }
            const auto count = builder.NaturalType(b) == Type::V128 ? VLane(builder, b, 64, 0) : b;
            if (l)
                return Require(
                        builder.VecShiftLeft(Type::V128, {left, Operand::ValueOf(count), width}));
            if (arithmetic)
                return Require(builder.VecShiftRightArithmetic(
                        Type::V128, {left, Operand::ValueOf(count), width}));
            return Require(
                    builder.VecShiftRight(Type::V128, {left, Operand::ValueOf(count), width}));
        }
        case Op::VPSLLVD:
        case Op::VPSLLVQ:
        case Op::VPSRLVD:
        case Op::VPSRLVQ:
        case Op::VPSRAVD: {
            const auto width = Operand::Immediate(
                    opcode == Op::VPSLLVQ || opcode == Op::VPSRLVQ ? 64 : 32, Type::U8);
            if (opcode == Op::VPSLLVD || opcode == Op::VPSLLVQ)
                return Require(builder.VecShiftLeft(Type::V128, {left, right, width}));
            if (opcode == Op::VPSRAVD)
                return Require(builder.VecShiftRightArithmetic(Type::V128, {left, right, width}));
            return Require(builder.VecShiftRight(Type::V128, {left, right, width}));
        }
        case Op::VPSHUFD:
            return Require(builder.VecShuffle32(Type::V128,
                                                {right, Operand::Immediate(immediate, Type::U8)}));
        case Op::VPSHUFLW:
        case Op::VPSHUFHW:
            return Require(
                    builder.VecShuffle16(Type::V128,
                                         {right,
                                          Operand::Immediate(immediate, Type::U8),
                                          Operand::Immediate(opcode == Op::VPSHUFHW, Type::U8)}));
        case Op::VSHUFPS:
            return Require(builder.VecShuffle32TwoSrc(
                    Type::V128, {left, right, Operand::Immediate(immediate, Type::U8)}));
        case Op::VSHUFPD: {
            const auto x = (immediate & 1) * 2, y = ((immediate >> 1) & 1) * 2;
            return Require(builder.VecShuffle32TwoSrc(
                    Type::V128,
                    {left,
                     right,
                     Operand::Immediate(x | ((x + 1) << 2) | (y << 4) | ((y + 1) << 6),
                                        Type::U8)}));
        }
        case Op::VPBLENDW:
        case Op::VBLENDPS:
        case Op::VBLENDPD:
        case Op::VPBLENDD:
            return VBlend(builder,
                          a,
                          b,
                          VLaneMask(builder,
                                    opcode == Op::VPBLENDW   ? 16
                                    : opcode == Op::VBLENDPD ? 64
                                                             : 32,
                                    immediate));
        case Op::VPALIGNR: {
            if (immediate >= 32) return Require(builder.VecSharedZero(Type::V128, {}));
            if (!immediate) return b;
            if (immediate < 16)
                return Require(builder.VecExtractBytes(
                        Type::V128, {right, left, Operand::Immediate(immediate, Type::U8)}));
            if (immediate == 16) return a;
            const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
            return Require(builder.VecByteShift(Type::V128,
                                                {left,
                                                 Operand::ValueOf(zero),
                                                 Operand::Immediate(immediate - 16, Type::U8),
                                                 Operand::Immediate(0, Type::U8)}));
        }
        case Op::VPSLLDQ:
        case Op::VPSRLDQ: {
            if (!immediate) return a;
            const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
            if (immediate >= 16) return zero;
            return Require(
                    builder.VecByteShift(Type::V128,
                                         {left,
                                          Operand::ValueOf(zero),
                                          Operand::Immediate(immediate, Type::U8),
                                          Operand::Immediate(opcode == Op::VPSLLDQ, Type::U8)}));
        }
        case Op::VMOVSLDUP:
        case Op::VMOVSHDUP:
            return Require(builder.VecDupPairs32(
                    Type::V128, {right, Operand::Immediate(opcode == Op::VMOVSHDUP, Type::U8)}));
        case Op::VMOVDDUP:
            return Require(
                    builder.VecDup64(Type::V128, {Operand::ValueOf(VLane(builder, b, 64, 0))}));
        case Op::VPABSB:
        case Op::VPABSW:
        case Op::VPABSD: {
            const auto bits = Operand::Immediate(opcode == Op::VPABSB   ? 8
                                                 : opcode == Op::VPABSW ? 16
                                                                        : 32,
                                                 Type::U8);
            const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
            const auto negative =
                    Require(builder.VecSub(Type::V128, {Operand::ValueOf(zero), right, bits}));
            return Require(builder.VecMax(
                    Type::V128,
                    {right, Operand::ValueOf(negative), bits, Operand::Immediate(1, Type::U8)}));
        }
        case Op::VPSIGNB:
        case Op::VPSIGNW:
        case Op::VPSIGND: {
            const auto bits = Operand::Immediate(opcode == Op::VPSIGNB   ? 8
                                                 : opcode == Op::VPSIGNW ? 16
                                                                         : 32,
                                                 Type::U8);
            const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
            const auto negative =
                    Require(builder.VecSub(Type::V128, {Operand::ValueOf(zero), left, bits}));
            const auto negmask =
                    Require(builder.VecCmpGt(Type::V128, {Operand::ValueOf(zero), right, bits}));
            const auto posmask =
                    Require(builder.VecCmpGt(Type::V128, {right, Operand::ValueOf(zero), bits}));
            const auto neg = Require(builder.VecAnd(
                    Type::V128, {Operand::ValueOf(negative), Operand::ValueOf(negmask)}));
            const auto pos = Require(builder.VecAnd(Type::V128, {left, Operand::ValueOf(posmask)}));
            return Require(
                    builder.VecOr(Type::V128, {Operand::ValueOf(neg), Operand::ValueOf(pos)}));
        }
        case Op::VCMPPS:
        case Op::VCMPPD:
        case Op::VCMPSS:
        case Op::VCMPSD: {
            constexpr unsigned relations[] = {2, 1, 3, 8, 13, 14, 12, 7, 10, 9, 11, 0, 5, 6, 4, 15};
            const bool wide = opcode == Op::VCMPPD || opcode == Op::VCMPSD;
            return Require(builder.VecFCmpMask(
                    Type::V128,
                    {left,
                     right,
                     Operand::Immediate(wide ? 64 : 32, Type::U8),
                     Operand::Immediate(relations[immediate & 15], Type::U8),
                     Operand::Immediate(opcode == Op::VCMPSS || opcode == Op::VCMPSD, Type::U8)}));
        }
        case Op::VPHADDW:
        case Op::VPHADDD:
        case Op::VPHADDSW:
        case Op::VPHSUBW:
        case Op::VPHSUBD:
        case Op::VPHSUBSW:
        case Op::VHADDPS:
        case Op::VHADDPD:
        case Op::VHSUBPS:
        case Op::VHSUBPD: {
            const bool fp = opcode == Op::VHADDPS || opcode == Op::VHADDPD ||
                            opcode == Op::VHSUBPS || opcode == Op::VHSUBPD;
            const bool wide = opcode == Op::VHADDPD || opcode == Op::VHSUBPD;
            const bool dword = opcode == Op::VPHADDD || opcode == Op::VPHSUBD;
            const bool saturate = opcode == Op::VPHADDSW || opcode == Op::VPHSUBSW;
            const bool sub = opcode == Op::VPHSUBW || opcode == Op::VPHSUBD ||
                             opcode == Op::VPHSUBSW || opcode == Op::VHSUBPS ||
                             opcode == Op::VHSUBPD;
            const auto bits = Operand::Immediate(fp ? (wide ? 64 : 32) : dword ? 32 : 16, Type::U8);
            const auto even = Require(builder.VecUnzip(
                    Type::V128, {left, right, bits, Operand::Immediate(0, Type::U8)}));
            const auto odd = Require(builder.VecUnzip(
                    Type::V128, {left, right, bits, Operand::Immediate(1, Type::U8)}));
            if (fp)
                return sub ? Require(builder.VecFSub(
                                     Type::V128,
                                     {Operand::ValueOf(even), Operand::ValueOf(odd), bits}))
                           : Require(builder.VecFAdd(
                                     Type::V128,
                                     {Operand::ValueOf(even), Operand::ValueOf(odd), bits}));
            if (saturate)
                return sub ? Require(builder.VecSatSub(Type::V128,
                                                       {Operand::ValueOf(even),
                                                        Operand::ValueOf(odd),
                                                        bits,
                                                        Operand::Immediate(1, Type::U8)}))
                           : Require(builder.VecSatAdd(Type::V128,
                                                       {Operand::ValueOf(even),
                                                        Operand::ValueOf(odd),
                                                        bits,
                                                        Operand::Immediate(1, Type::U8)}));
            return sub ? Require(builder.VecSub(
                                 Type::V128, {Operand::ValueOf(even), Operand::ValueOf(odd), bits}))
                       : Require(builder.VecAdd(
                                 Type::V128,
                                 {Operand::ValueOf(even), Operand::ValueOf(odd), bits}));
        }
        case Op::VADDSUBPS:
        case Op::VADDSUBPD: {
            const unsigned bits = opcode == Op::VADDSUBPS ? 32 : 64;
            const auto width = Operand::Immediate(bits, Type::U8);
            const auto add = Require(builder.VecFAdd(Type::V128, {left, right, width}));
            const auto sub = Require(builder.VecFSub(Type::V128, {left, right, width}));
            return VBlend(builder, add, sub, VLaneMask(builder, bits, 5));
        }
        case Op::VPMULHRSW: {
            std::array<Value, 2> halves{};
            const auto rounding =
                    VConstant(builder, UINT64_C(0x400000004000), UINT64_C(0x400000004000));
            for (unsigned half = 0; half < 2; ++half) {
                const auto x = Require(builder.VecZip(Type::V128,
                                                      {left,
                                                       left,
                                                       Operand::Immediate(16, Type::U8),
                                                       Operand::Immediate(half, Type::U8)}));
                const auto y = Require(builder.VecZip(Type::V128,
                                                      {right,
                                                       right,
                                                       Operand::Immediate(16, Type::U8),
                                                       Operand::Immediate(half, Type::U8)}));
                const auto product =
                        Require(builder.VecMulWiden(Type::V128,
                                                    {Operand::ValueOf(x),
                                                     Operand::ValueOf(y),
                                                     Operand::Immediate(16, Type::U8),
                                                     Operand::Immediate(1, Type::U8)}));
                const auto rounded = Require(builder.VecAdd(Type::V128,
                                                            {Operand::ValueOf(product),
                                                             Operand::ValueOf(rounding),
                                                             Operand::Immediate(32, Type::U8)}));
                halves[half] = Require(
                        builder.VecShiftRightArithmeticImm(Type::V128,
                                                           {Operand::ValueOf(rounded),
                                                            Operand::Immediate(15, Type::U8),
                                                            Operand::Immediate(32, Type::U8)}));
            }
            return Require(builder.VecUnzip(Type::V128,
                                            {Operand::ValueOf(halves[0]),
                                             Operand::ValueOf(halves[1]),
                                             Operand::Immediate(16, Type::U8),
                                             Operand::Immediate(0, Type::U8)}));
        }
        case Op::VDPPS:
        case Op::VDPPD: {
            const unsigned bits = opcode == Op::VDPPS ? 32 : 64;
            const auto width = Operand::Immediate(bits, Type::U8);
            const auto mask = VLaneMask(builder, bits, immediate >> 4);
            const auto x = Require(builder.VecAnd(Type::V128, {left, Operand::ValueOf(mask)}));
            const auto y = Require(builder.VecAnd(Type::V128, {right, Operand::ValueOf(mask)}));
            auto sum = Require(
                    builder.VecFMul(Type::V128, {Operand::ValueOf(x), Operand::ValueOf(y), width}));
            for (unsigned step = 0; step < (bits == 32 ? 2u : 1u); ++step) {
                const auto even = Require(builder.VecUnzip(Type::V128,
                                                           {Operand::ValueOf(sum),
                                                            Operand::ValueOf(sum),
                                                            width,
                                                            Operand::Immediate(0, Type::U8)}));
                const auto odd = Require(builder.VecUnzip(Type::V128,
                                                          {Operand::ValueOf(sum),
                                                           Operand::ValueOf(sum),
                                                           width,
                                                           Operand::Immediate(1, Type::U8)}));
                sum = Require(builder.VecFAdd(
                        Type::V128, {Operand::ValueOf(even), Operand::ValueOf(odd), width}));
            }
            const auto output_mask = VLaneMask(builder, bits, immediate);
            return Require(builder.VecAnd(Type::V128,
                                          {Operand::ValueOf(sum), Operand::ValueOf(output_mask)}));
        }
        case Op::VROUNDPS:
        case Op::VROUNDPD:
        case Op::VROUNDSS:
        case Op::VROUNDSD: {
            const unsigned bits = opcode == Op::VROUNDPD || opcode == Op::VROUNDSD ? 64 : 32;
            const bool scalar = opcode == Op::VROUNDSS || opcode == Op::VROUNDSD;
            const auto round = [&](unsigned mode) {
                return Require(builder.VecFRoundInt(Type::V128,
                                                    {left,
                                                     right,
                                                     Operand::Immediate(bits, Type::U8),
                                                     Operand::Immediate(mode, Type::U8),
                                                     Operand::Immediate(scalar, Type::U8)}));
            };
            if (!(immediate & 4)) return round(immediate & 3);
            if (layout.mxcsr_offset > UINT32_MAX - 3) Unsupported();
            const auto raw = Require(builder.LoadUniform(
                    Type::U32, {Operand::Uniform(layout.mxcsr_offset, Type::U32)}));
            const auto mode = Require(builder.BitExtract(Type::U32,
                                                         {Operand::ValueOf(raw),
                                                          Operand::Immediate(13, Type::U8),
                                                          Operand::Immediate(2, Type::U8)}));
            auto result = round(0);
            for (unsigned i = 1; i < 4; ++i) {
                const auto mismatch = Require(builder.Xor(
                        Type::U32, {Operand::ValueOf(mode), Operand::Immediate(i, Type::U32)}));
                const auto test =
                        Require(builder.TestZero(Type::Bool, {Operand::ValueOf(mismatch)}));
                const auto all =
                        Require(builder.Neg(Type::U64, {Operand::ValueOf(test, Type::U64)}));
                const auto mask = Require(builder.VecDup64(Type::V128, {Operand::ValueOf(all)}));
                result = VBlend(builder, result, round(i), mask);
            }
            return result;
        }
        default:
            Unsupported();
    }
}
static bool AvxFmaInfo(Op opcode, unsigned& bits, unsigned& order, unsigned& flags) {
    switch (opcode) {
#define AVX_FMA_ONE(name, width, permutation, controls)                                            \
    case Op::V##name:                                                                              \
        bits = width;                                                                              \
        order = permutation;                                                                       \
        flags = controls;                                                                          \
        return true;
#define AVX_FMA_ORDER(stem, permutation, controls)                                                 \
    AVX_FMA_ONE(stem##permutation##PS, 32, permutation, controls)                                  \
    AVX_FMA_ONE(stem##permutation##PD, 64, permutation, controls)                                  \
    AVX_FMA_ONE(stem##permutation##SS, 32, permutation, (controls) | 4)                            \
    AVX_FMA_ONE(stem##permutation##SD, 64, permutation, (controls) | 4)
#define AVX_FMA_KIND(stem, controls)                                                               \
    AVX_FMA_ORDER(stem, 132, controls)                                                             \
    AVX_FMA_ORDER(stem, 213, controls)                                                             \
    AVX_FMA_ORDER(stem, 231, controls)
        AVX_FMA_KIND(FMADD, 0)
        AVX_FMA_KIND(FMSUB, 2)
        AVX_FMA_KIND(FNMADD, 1)
        AVX_FMA_KIND(FNMSUB, 3)
#undef AVX_FMA_KIND
#undef AVX_FMA_ORDER
#define AVX_FMA_ALTERNATE(stem, permutation, controls)                                             \
    AVX_FMA_ONE(stem##permutation##PS, 32, permutation, controls)                                  \
    AVX_FMA_ONE(stem##permutation##PD, 64, permutation, controls)
        AVX_FMA_ALTERNATE(FMADDSUB, 132, 8)
        AVX_FMA_ALTERNATE(FMADDSUB, 213, 8)
        AVX_FMA_ALTERNATE(FMADDSUB, 231, 8)
        AVX_FMA_ALTERNATE(FMSUBADD, 132, 24)
        AVX_FMA_ALTERNATE(FMSUBADD, 213, 24)
        AVX_FMA_ALTERNATE(FMSUBADD, 231, 24)
#undef AVX_FMA_ALTERNATE
#undef AVX_FMA_ONE
        default:
            return false;
    }
}
static Value AvxPermute32(Emitter& builder,
                          const std::array<Value, 2>& source,
                          Value indices,
                          bool wide) {
    const auto mask =
            VConstant(builder, UINT64_C(0x0000000700000007), UINT64_C(0x0000000700000007));
    const auto selected = Require(
            builder.VecAnd(Type::V128, {Operand::ValueOf(indices), Operand::ValueOf(mask)}));
    const auto scale =
            VConstant(builder, UINT64_C(0x0404040404040404), UINT64_C(0x0404040404040404));
    const auto repeated = Require(builder.VecMul(Type::V128,
                                                 {Operand::ValueOf(selected),
                                                  Operand::ValueOf(scale),
                                                  Operand::Immediate(32, Type::U8)}));
    const auto bytes =
            VConstant(builder, UINT64_C(0x0302010003020100), UINT64_C(0x0302010003020100));
    const auto offsets = Require(builder.VecAdd(Type::V128,
                                                {Operand::ValueOf(repeated),
                                                 Operand::ValueOf(bytes),
                                                 Operand::Immediate(8, Type::U8)}));
    if (!wide)
        return Require(builder.VecTableLookup8(
                Type::V128, {Operand::ValueOf(source[0]), Operand::ValueOf(offsets)}));
    const auto halves =
            VConstant(builder, UINT64_C(0x1010101010101010), UINT64_C(0x1010101010101010));
    const auto bit = Require(
            builder.VecAnd(Type::V128, {Operand::ValueOf(offsets), Operand::ValueOf(halves)}));
    const auto high = Require(builder.VecShiftLeftImm(Type::V128,
                                                      {Operand::ValueOf(bit),
                                                       Operand::Immediate(3, Type::U8),
                                                       Operand::Immediate(8, Type::U8)}));
    const auto low_indices =
            Require(builder.VecOr(Type::V128, {Operand::ValueOf(offsets), Operand::ValueOf(high)}));
    const auto invert =
            VConstant(builder, UINT64_C(0x8080808080808080), UINT64_C(0x8080808080808080));
    const auto high_indices = Require(
            builder.VecXor(Type::V128, {Operand::ValueOf(low_indices), Operand::ValueOf(invert)}));
    const auto low = Require(builder.VecTableLookup8(
            Type::V128, {Operand::ValueOf(source[0]), Operand::ValueOf(low_indices)}));
    const auto upper = Require(builder.VecTableLookup8(
            Type::V128, {Operand::ValueOf(source[1]), Operand::ValueOf(high_indices)}));
    return Require(builder.VecOr(Type::V128, {Operand::ValueOf(low), Operand::ValueOf(upper)}));
}
static bool Avx(Emitter& builder,
                StateLayout layout,
                const InstructionData& instruction,
                PendingFlags& pending,
                u64 pc,
                u64 next_pc,
                TargetInfo target) {
    if (!instruction.vector_bytes) return false;
    const unsigned bytes = instruction.vector_bytes;
    if (bytes != 16 && bytes != 32) Unsupported();
    for (const auto& operand : instruction.ops)
        if (Memory(operand)) {
            pending.Flush(builder);
            break;
        }
    const auto opcode = instruction.opcode;
    const auto destination = instruction.ops[0];
    const auto read = [&](const DecodedOperand& operand, unsigned size, bool aligned = false) {
        return AvxRead(builder, layout, instruction, operand, size, pc, next_pc, target, aligned);
    };
    const auto write = [&](const std::array<Value, 2>& values, unsigned size) {
        AvxWrite(builder, layout, instruction, destination, size, values, pc, next_pc, target);
    };
    const auto immediate = static_cast<unsigned>(static_cast<std::uint8_t>(instruction.immediate));
    if (opcode == Op::VZEROUPPER || opcode == Op::VZEROALL) {
        if (layout.ymm_high_offset > UINT32_MAX - 255 ||
            (opcode == Op::VZEROALL && layout.fprs_offset > UINT32_MAX - 255))
            Unsupported();
        const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
        for (unsigned slot = 0; slot < 16; ++slot) {
            if (opcode == Op::VZEROALL)
                builder.WriteXmm(layout.fprs_offset + slot * 16, slot, zero);
            Require(builder.StoreUniform(
                    Type::Void,
                    {Operand::Uniform(layout.ymm_high_offset + slot * 16, Type::V128),
                     Operand::ValueOf(zero)}));
        }
        return true;
    }
    unsigned fma_bits{}, fma_order{}, fma_flags{};
    if (AvxFmaInfo(opcode, fma_bits, fma_order, fma_flags)) {
        if (!VRegister(destination) || !VRegister(instruction.ops[1])) Unsupported();
        const auto old = read(destination, bytes), second = read(instruction.ops[1], bytes);
        const auto third = read(instruction.ops[2], fma_flags & 4 ? fma_bits / 8 : bytes);
        std::array<Value, 2> result{};
        for (unsigned half = 0; half < bytes / 16; ++half) {
            const auto a = fma_order == 132 ? old[half] : second[half];
            const auto b = fma_order == 213 ? old[half] : third[half];
            const auto c = fma_order == 132   ? second[half]
                           : fma_order == 213 ? third[half]
                                              : old[half];
            result[half] = Require(builder.VecFMulAdd(Type::V128,
                                                      {Operand::ValueOf(a),
                                                       Operand::ValueOf(b),
                                                       Operand::ValueOf(c),
                                                       Operand::Immediate(fma_bits, Type::U8),
                                                       Operand::Immediate(fma_flags, Type::U8)}));
        }
        if (fma_flags & 4)
            result[0] = VReplaceLane(
                    builder, old[0], VLane(builder, result[0], fma_bits, 0), fma_bits, 0);
        write(result, bytes);
        return true;
    }
    switch (opcode) {
        case Op::VMOVUPS:
        case Op::VMOVUPD:
        case Op::VMOVDQU:
        case Op::VMOVAPS:
        case Op::VMOVAPD:
        case Op::VMOVDQA:
        case Op::VLDDQU:
        case Op::VMOVNTDQA:
        case Op::VMOVNTDQ:
        case Op::VMOVNTPS:
        case Op::VMOVNTPD: {
            const bool aligned = opcode == Op::VMOVAPS || opcode == Op::VMOVAPD ||
                                 opcode == Op::VMOVDQA || opcode == Op::VMOVNTDQA ||
                                 opcode == Op::VMOVNTDQ || opcode == Op::VMOVNTPS ||
                                 opcode == Op::VMOVNTPD;
            const bool streaming =
                    opcode == Op::VMOVNTDQ || opcode == Op::VMOVNTPS || opcode == Op::VMOVNTPD;
            if (streaming && (!Memory(destination) || !VRegister(instruction.ops[1])))
                Unsupported();
            if (opcode == Op::VMOVNTDQA && (!VRegister(destination) || !Memory(instruction.ops[1])))
                Unsupported();
            const auto values = read(instruction.ops[1], bytes, aligned);
            if (streaming)
                AvxWrite<OpCode::StoreMemory>(builder,
                                              layout,
                                              instruction,
                                              destination,
                                              bytes,
                                              values,
                                              pc,
                                              next_pc,
                                              target,
                                              true);
            else
                AvxWrite(builder,
                         layout,
                         instruction,
                         destination,
                         bytes,
                         values,
                         pc,
                         next_pc,
                         target,
                         aligned);
            return true;
        }
        case Op::VMOVD:
        case Op::VMOVQ: {
            const unsigned bits = opcode == Op::VMOVQ ? 64 : 32;
            if (VRegister(destination)) {
                const auto low = VScalarRead(builder,
                                             layout,
                                             instruction,
                                             instruction.ops[1],
                                             bits,
                                             pc,
                                             next_pc,
                                             target);
                write({VLowScalar(builder, low, bits), {}}, 16);
            } else {
                const auto low = VScalarRead(builder,
                                             layout,
                                             instruction,
                                             instruction.ops[1],
                                             bits,
                                             pc,
                                             next_pc,
                                             target);
                if (destination.type == GuestOperandKind::Register)
                    WriteRegister(builder, layout, destination, low);
                else {
                    if (!Memory(destination) || destination.size != bits) Unsupported();
                    PrepareFaultPC(builder, layout, pc, target);
                    u64 displacement{};
                    const auto address = Address(builder,
                                                 layout,
                                                 instruction,
                                                 destination,
                                                 next_pc,
                                                 target,
                                                 {},
                                                 &displacement);
                    StoreMemory(builder, address, low, bits, displacement);
                }
            }
            return true;
        }
        case Op::VMOVSS:
        case Op::VMOVSD: {
            const unsigned bits = opcode == Op::VMOVSS ? 32 : 64;
            if (Memory(destination)) {
                const auto low = VScalarRead(builder,
                                             layout,
                                             instruction,
                                             instruction.ops[1],
                                             bits,
                                             pc,
                                             next_pc,
                                             target);
                PrepareFaultPC(builder, layout, pc, target);
                u64 displacement{};
                const auto address = Address(builder,
                                             layout,
                                             instruction,
                                             destination,
                                             next_pc,
                                             target,
                                             {},
                                             &displacement);
                StoreMemory(builder, address, low, bits, displacement);
            } else if (Memory(instruction.ops[1])) {
                const auto low = VScalarRead(builder,
                                             layout,
                                             instruction,
                                             instruction.ops[1],
                                             bits,
                                             pc,
                                             next_pc,
                                             target);
                write({VLowScalar(builder, low, bits), {}}, 16);
            } else {
                const auto merge = read(instruction.ops[1], 16);
                const auto low = VScalarRead(builder,
                                             layout,
                                             instruction,
                                             instruction.ops[2],
                                             bits,
                                             pc,
                                             next_pc,
                                             target);
                write({VReplaceLane(builder, merge[0], low, bits, 0), {}}, 16);
            }
            return true;
        }
        case Op::VMOVLPS:
        case Op::VMOVLPD:
        case Op::VMOVHPS:
        case Op::VMOVHPD: {
            const bool high = opcode == Op::VMOVHPS || opcode == Op::VMOVHPD;
            if (Memory(destination)) {
                const auto value = read(instruction.ops[1], 16);
                const auto raw = VLane(builder, value[0], 64, high);
                PrepareFaultPC(builder, layout, pc, target);
                u64 displacement{};
                const auto address = Address(builder,
                                             layout,
                                             instruction,
                                             destination,
                                             next_pc,
                                             target,
                                             {},
                                             &displacement);
                StoreMemory(builder, address, raw, 64, displacement);
            } else {
                const auto merge = read(instruction.ops[1], 16);
                const auto raw = VScalarRead(
                        builder, layout, instruction, instruction.ops[2], 64, pc, next_pc, target);
                write({VReplaceLane(builder, merge[0], raw, 64, high), {}}, 16);
            }
            return true;
        }
        case Op::VCVTSI2SS:
        case Op::VCVTSI2SD: {
            const unsigned bits = opcode == Op::VCVTSI2SS ? 32 : 64;
            const auto merge = read(instruction.ops[1], 16);
            const auto source = instruction.ops[2];
            if (source.size != 32 && source.size != 64) Unsupported();
            if (Memory(source)) PrepareFaultPC(builder, layout, pc, target);
            const auto value = ReadData(builder, layout, instruction, source, next_pc, target);
            const auto converted = Require(
                    builder.VecFCvtIntToFloat(ScalarType(bits),
                                              {Operand::ValueOf(value, ScalarType(source.size)),
                                               Operand::Immediate(source.size, Type::U8),
                                               Operand::Immediate(bits, Type::U8)}));
            write({VReplaceLane(builder, merge[0], converted, bits, 0), {}}, 16);
            return true;
        }
        case Op::VCVTSS2SI:
        case Op::VCVTTSS2SI:
        case Op::VCVTSD2SI:
        case Op::VCVTTSD2SI: {
            const unsigned bits = opcode == Op::VCVTSS2SI || opcode == Op::VCVTTSS2SI ? 32 : 64;
            const bool truncate = opcode == Op::VCVTTSS2SI || opcode == Op::VCVTTSD2SI;
            if (destination.type != GuestOperandKind::Register ||
                (destination.size != 32 && destination.size != 64))
                Unsupported();
            const auto raw = VScalarRead(
                    builder, layout, instruction, instruction.ops[1], bits, pc, next_pc, target);
            const auto converted = Require(
                    builder.VecFCvtFloatToInt(ScalarType(destination.size),
                                              {Operand::ValueOf(raw, ScalarType(bits)),
                                               Operand::Immediate(bits, Type::U8),
                                               Operand::Immediate(destination.size, Type::U8),
                                               Operand::Immediate(truncate, Type::U8)}));
            WriteRegister(builder, layout, destination, converted);
            return true;
        }
        case Op::VCVTSS2SD:
        case Op::VCVTSD2SS: {
            const unsigned source_bits = opcode == Op::VCVTSS2SD ? 32 : 64,
                           output_bits = source_bits == 32 ? 64 : 32;
            const auto merge = read(instruction.ops[1], 16);
            const auto raw = VScalarRead(builder,
                                         layout,
                                         instruction,
                                         instruction.ops[2],
                                         source_bits,
                                         pc,
                                         next_pc,
                                         target);
            const auto converted =
                    Require(builder.VecFCvtScalar(ScalarType(output_bits),
                                                  {Operand::ValueOf(raw, ScalarType(source_bits)),
                                                   Operand::Immediate(source_bits, Type::U8)}));
            write({VReplaceLane(builder, merge[0], converted, output_bits, 0), {}}, 16);
            return true;
        }
        case Op::VCVTDQ2PS:
        case Op::VCVTDQ2PD:
        case Op::VCVTPS2DQ:
        case Op::VCVTTPS2DQ:
        case Op::VCVTPD2DQ:
        case Op::VCVTTPD2DQ:
        case Op::VCVTPS2PD:
        case Op::VCVTPD2PS: {
            const unsigned kind = opcode == Op::VCVTDQ2PS    ? 0
                                  : opcode == Op::VCVTDQ2PD  ? 1
                                  : opcode == Op::VCVTPS2DQ  ? 2
                                  : opcode == Op::VCVTTPS2DQ ? 3
                                  : opcode == Op::VCVTPD2DQ  ? 4
                                  : opcode == Op::VCVTTPD2DQ ? 5
                                  : opcode == Op::VCVTPS2PD  ? 6
                                                             : 7;
            const bool widen = kind == 1 || kind == 6, narrow = kind == 4 || kind == 5 || kind == 7;
            const auto input = read(instruction.ops[1], widen ? bytes / 2 : bytes);
            std::array<Value, 2> output{};
            const auto convert = [&](Value value) {
                return Require(builder.VecFCvtPacked(
                        Type::V128, {Operand::ValueOf(value), Operand::Immediate(kind, Type::U8)}));
            };
            output[0] = convert(input[0]);
            if (bytes == 32) {
                if (widen) {
                    const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
                    const auto high =
                            Require(builder.VecByteShift(Type::V128,
                                                         {Operand::ValueOf(input[0]),
                                                          Operand::ValueOf(zero),
                                                          Operand::Immediate(8, Type::U8),
                                                          Operand::Immediate(0, Type::U8)}));
                    output[1] = convert(high);
                } else if (narrow) {
                    const auto high = convert(input[1]);
                    output[0] = Require(builder.VecZip(Type::V128,
                                                       {Operand::ValueOf(output[0]),
                                                        Operand::ValueOf(high),
                                                        Operand::Immediate(64, Type::U8),
                                                        Operand::Immediate(0, Type::U8)}));
                } else
                    output[1] = convert(input[1]);
            }
            write(output, narrow ? 16 : bytes);
            return true;
        }
        case Op::VMOVMSKPS:
        case Op::VMOVMSKPD:
        case Op::VPMOVMSKB: {
            const unsigned bits = opcode == Op::VMOVMSKPS ? 32 : opcode == Op::VMOVMSKPD ? 64 : 8;
            const auto input = read(instruction.ops[1], bytes);
            auto value = Require(builder.VecMovMask(
                    Type::U32, {Operand::ValueOf(input[0]), Operand::Immediate(bits, Type::U8)}));
            if (bytes == 32) {
                const auto high = Require(builder.VecMovMask(
                        Type::U32,
                        {Operand::ValueOf(input[1]), Operand::Immediate(bits, Type::U8)}));
                const auto shifted = Require(builder.LslImm(
                        Type::U32,
                        {Operand::ValueOf(high), Operand::Immediate(128 / bits, Type::U8)}));
                value = Require(builder.Or(Type::U32,
                                           {Operand::ValueOf(value), Operand::ValueOf(shifted)}));
            }
            WriteRegister(builder, layout, destination, value);
            return true;
        }
        case Op::VCOMISS:
        case Op::VUCOMISS:
        case Op::VCOMISD:
        case Op::VUCOMISD: {
            const unsigned bits = opcode == Op::VCOMISS || opcode == Op::VUCOMISS ? 32 : 64;
            const auto left = read(destination, 16), right = read(instruction.ops[1], bits / 8);
            const auto value = Require(builder.VecFCmp(Type::U32,
                                                       {Operand::ValueOf(left[0]),
                                                        Operand::ValueOf(right[0]),
                                                        Operand::Immediate(bits, Type::U8)}));
            Require(builder.PublishFCmpFlags(Type::Void, {Operand::ValueOf(value)}));
            pending = {};
            return true;
        }
        case Op::VPTEST:
        case Op::VTESTPS:
        case Op::VTESTPD: {
            const auto left = read(destination, bytes), right = read(instruction.ops[1], bytes);
            Value both{}, inverse{};
            for (unsigned half = 0; half < bytes / 16; ++half) {
                const auto a = Require(builder.VecAnd(
                        Type::V128, {Operand::ValueOf(left[half]), Operand::ValueOf(right[half])}));
                const auto b = Require(builder.VecAndNot(
                        Type::V128, {Operand::ValueOf(right[half]), Operand::ValueOf(left[half])}));
                const auto reduce = [&](Value vector) {
                    if (opcode != Op::VPTEST)
                        return Require(builder.VecMovMask(
                                Type::U64,
                                {Operand::ValueOf(vector),
                                 Operand::Immediate(opcode == Op::VTESTPS ? 32 : 64, Type::U8)}));
                    const auto lo = VLane(builder, vector, 64, 0),
                               hi = VLane(builder, vector, 64, 1);
                    return Require(
                            builder.Or(Type::U64, {Operand::ValueOf(lo), Operand::ValueOf(hi)}));
                };
                const auto x = reduce(a), y = reduce(b);
                both = half ? Require(builder.Or(Type::U64,
                                                 {Operand::ValueOf(both), Operand::ValueOf(x)}))
                            : x;
                inverse =
                        half ? Require(builder.Or(Type::U64,
                                                  {Operand::ValueOf(inverse), Operand::ValueOf(y)}))
                             : y;
            }
            const auto carry = Require(builder.TestZero(Type::Bool, {Operand::ValueOf(inverse)}));
            const auto zero = Require(builder.TestZero(Type::Bool, {Operand::ValueOf(both)}));
            const auto flags = Require(builder.BitInsert(Type::U32,
                                                         {Operand::ValueOf(carry, Type::U32),
                                                          Operand::ValueOf(zero, Type::U32),
                                                          Operand::Immediate(2, Type::U8),
                                                          Operand::Immediate(1, Type::U8)}));
            Require(builder.PublishFCmpFlags(Type::Void, {Operand::ValueOf(flags)}));
            pending = {};
            return true;
        }
        default:
            break;
    }
    switch (opcode) {
        case Op::VPEXTRB:
        case Op::VPEXTRW:
        case Op::VPEXTRD:
        case Op::VPEXTRQ:
        case Op::VEXTRACTPS: {
            auto legacy = instruction;
            legacy.opcode = opcode == Op::VPEXTRB   ? Op::PEXTRB
                            : opcode == Op::VPEXTRW ? Op::PEXTRW
                            : opcode == Op::VPEXTRD ? Op::PEXTRD
                            : opcode == Op::VPEXTRQ ? Op::PEXTRQ
                                                    : Op::EXTRACTPS;
            return SseExtended(builder, layout, legacy, pc, next_pc, target);
        }
        case Op::VPINSRB:
        case Op::VPINSRW:
        case Op::VPINSRD:
        case Op::VPINSRQ:
        case Op::VINSERTPS: {
            const unsigned bits = opcode == Op::VPINSRB   ? 8
                                  : opcode == Op::VPINSRW ? 16
                                  : opcode == Op::VPINSRQ ? 64
                                                          : 32;
            const auto merge = read(instruction.ops[1], 16);
            Value scalar;
            auto source = instruction.ops[2];
            unsigned lane = immediate & (128 / bits - 1);
            if (opcode == Op::VINSERTPS) {
                scalar = VRegister(source) ? VLane(builder, read(source, 16)[0], 32, immediate >> 6)
                                           : VScalarRead(builder,
                                                         layout,
                                                         instruction,
                                                         source,
                                                         32,
                                                         pc,
                                                         next_pc,
                                                         target);
                lane = (immediate >> 4) & 3;
            } else {
                if (Memory(source)) {
                    PrepareFaultPC(builder, layout, pc, target);
                    source.size = static_cast<std::uint16_t>(bits);
                }
                if ((source.type != GuestOperandKind::Register && !Memory(source)) ||
                    VRegister(source))
                    Unsupported();
                scalar = ReadData(builder, layout, instruction, source, next_pc, target);
            }
            auto value = VReplaceLane(builder, merge[0], scalar, bits, lane);
            if (opcode == Op::VINSERTPS && (immediate & 15)) {
                const auto mask = VLaneMask(builder, 32, ~immediate & 15);
                value = Require(builder.VecAnd(Type::V128,
                                               {Operand::ValueOf(value), Operand::ValueOf(mask)}));
            }
            write({value, {}}, 16);
            return true;
        }
        case Op::VINSERTF128:
        case Op::VINSERTI128: {
            if (bytes != 32) Unsupported();
            auto value = read(instruction.ops[1], 32);
            const auto insert = read(instruction.ops[2], 16);
            value[immediate & 1] = insert[0];
            write(value, 32);
            return true;
        }
        case Op::VEXTRACTF128:
        case Op::VEXTRACTI128: {
            const auto input = read(instruction.ops[1], 32);
            write({input[immediate & 1], {}}, 16);
            return true;
        }
        case Op::VPERM2F128:
        case Op::VPERM2I128: {
            const auto a = read(instruction.ops[1], 32), b = read(instruction.ops[2], 32);
            const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
            const std::array<Value, 4> halves{a[0], a[1], b[0], b[1]};
            write({immediate & 8 ? zero : halves[immediate & 3],
                   immediate & 128 ? zero : halves[(immediate >> 4) & 3]},
                  32);
            return true;
        }
        case Op::VPERMQ:
        case Op::VPERMPD: {
            if (bytes != 32) Unsupported();
            const auto source = read(instruction.ops[1], 32);
            std::array<Value, 2> result{};
            for (unsigned half = 0; half < 2; ++half) {
                const auto x = (immediate >> (half * 4)) & 3, y = (immediate >> (half * 4 + 2)) & 3;
                const auto a = Require(builder.VecDup64(
                        Type::V128, {Operand::ValueOf(VLane(builder, source[x / 2], 64, x & 1))}));
                const auto b = Require(builder.VecDup64(
                        Type::V128, {Operand::ValueOf(VLane(builder, source[y / 2], 64, y & 1))}));
                result[half] = Require(builder.VecZip(Type::V128,
                                                      {Operand::ValueOf(a),
                                                       Operand::ValueOf(b),
                                                       Operand::Immediate(64, Type::U8),
                                                       Operand::Immediate(0, Type::U8)}));
            }
            write(result, 32);
            return true;
        }
        case Op::VPERMD:
        case Op::VPERMPS: {
            const auto indices = read(instruction.ops[1], bytes),
                       source = read(instruction.ops[2], bytes);
            std::array<Value, 2> output{};
            for (unsigned half = 0; half < bytes / 16; ++half)
                output[half] = AvxPermute32(builder, source, indices[half], bytes == 32);
            write(output, bytes);
            return true;
        }
        case Op::VPERMILPS:
        case Op::VPERMILPD: {
            const auto source = read(instruction.ops[1], bytes);
            std::array<Value, 2> output{};
            if (instruction.ops[2].type == GuestOperandKind::Immediate) {
                for (unsigned half = 0; half < bytes / 16; ++half) {
                    if (opcode == Op::VPERMILPS)
                        output[half] = Require(
                                builder.VecShuffle32(Type::V128,
                                                     {Operand::ValueOf(source[half]),
                                                      Operand::Immediate(immediate, Type::U8)}));
                    else {
                        const auto x = ((immediate >> (half * 2)) & 1) * 2,
                                   y = ((immediate >> (half * 2 + 1)) & 1) * 2;
                        output[half] = Require(builder.VecShuffle32(
                                Type::V128,
                                {Operand::ValueOf(source[half]),
                                 Operand::Immediate(x | ((x + 1) << 2) | (y << 4) | ((y + 1) << 6),
                                                    Type::U8)}));
                    }
                }
            } else {
                const auto controls = read(instruction.ops[2], bytes);
                for (unsigned half = 0; half < bytes / 16; ++half) {
                    if (opcode == Op::VPERMILPS)
                        output[half] =
                                AvxPermute32(builder, {source[half], {}}, controls[half], false);
                    else {
                        const auto shifted = Require(
                                builder.VecShiftLeftImm(Type::V128,
                                                        {Operand::ValueOf(controls[half]),
                                                         Operand::Immediate(62, Type::U8),
                                                         Operand::Immediate(64, Type::U8)}));
                        const auto mask = Require(builder.VecShiftRightArithmeticImm(
                                Type::V128,
                                {Operand::ValueOf(shifted),
                                 Operand::Immediate(63, Type::U8),
                                 Operand::Immediate(64, Type::U8)}));
                        const auto eight = VConstant(builder,
                                                     UINT64_C(0x0808080808080808),
                                                     UINT64_C(0x0808080808080808));
                        const auto selected = Require(builder.VecAnd(
                                Type::V128, {Operand::ValueOf(mask), Operand::ValueOf(eight)}));
                        const auto order = VConstant(builder,
                                                     UINT64_C(0x0706050403020100),
                                                     UINT64_C(0x0706050403020100));
                        const auto indices = Require(builder.VecOr(
                                Type::V128, {Operand::ValueOf(selected), Operand::ValueOf(order)}));
                        output[half] = Require(builder.VecTableLookup8(
                                Type::V128,
                                {Operand::ValueOf(source[half]), Operand::ValueOf(indices)}));
                    }
                }
            }
            write(output, bytes);
            return true;
        }
        case Op::VPBLENDVB:
        case Op::VBLENDVPS:
        case Op::VBLENDVPD: {
            const auto a = read(instruction.ops[1], bytes), b = read(instruction.ops[2], bytes),
                       raw_mask = read(instruction.ops[3], bytes);
            const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
            const unsigned bits = opcode == Op::VPBLENDVB ? 8 : opcode == Op::VBLENDVPS ? 32 : 64;
            std::array<Value, 2> output{};
            for (unsigned half = 0; half < bytes / 16; ++half) {
                const auto mask = Require(builder.VecCmpGt(Type::V128,
                                                           {Operand::ValueOf(zero),
                                                            Operand::ValueOf(raw_mask[half]),
                                                            Operand::Immediate(bits, Type::U8)}));
                output[half] = VBlend(builder, a[half], b[half], mask);
            }
            write(output, bytes);
            return true;
        }
        case Op::VPBROADCASTB:
        case Op::VPBROADCASTW:
        case Op::VPBROADCASTD:
        case Op::VPBROADCASTQ:
        case Op::VBROADCASTSS:
        case Op::VBROADCASTSD: {
            const unsigned bits = opcode == Op::VPBROADCASTB                                 ? 8
                                  : opcode == Op::VPBROADCASTW                               ? 16
                                  : opcode == Op::VPBROADCASTQ || opcode == Op::VBROADCASTSD ? 64
                                                                                             : 32;
            const auto source = instruction.ops[1];
            Value raw;
            if (VRegister(source))
                raw = VLane(builder, read(source, 16)[0], bits, 0);
            else {
                if (!Memory(source) || source.size != bits) Unsupported();
                PrepareFaultPC(builder, layout, pc, target);
                raw = ReadData(builder, layout, instruction, source, next_pc, target);
            }
            const auto repeated = AvxReplicate(builder, raw, bits);
            write({repeated, repeated}, bytes);
            return true;
        }
        case Op::VBROADCASTF128:
        case Op::VBROADCASTI128: {
            if (bytes != 32 || !Memory(instruction.ops[1])) Unsupported();
            const auto source = read(instruction.ops[1], 16);
            write({source[0], source[0]}, 32);
            return true;
        }
        case Op::VPHMINPOSUW: {
            auto legacy = instruction;
            legacy.opcode = Op::PHMINPOSUW;
            if (!SseExtended(builder, layout, legacy, pc, next_pc, target)) Unsupported();
            const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
            Require(builder.StoreUniform(
                    Type::Void,
                    {Operand::Uniform(AvxHighOffset(layout, destination), Type::V128),
                     Operand::ValueOf(zero)}));
            return true;
        }
        default:
            break;
    }
    unsigned extend_source{}, extend_destination{};
    bool signed_extend{};
    switch (opcode) {
        case Op::VPMOVSXBW:
            signed_extend = true;
            [[fallthrough]];
        case Op::VPMOVZXBW:
            extend_source = 8;
            extend_destination = 16;
            break;
        case Op::VPMOVSXBD:
            signed_extend = true;
            [[fallthrough]];
        case Op::VPMOVZXBD:
            extend_source = 8;
            extend_destination = 32;
            break;
        case Op::VPMOVSXBQ:
            signed_extend = true;
            [[fallthrough]];
        case Op::VPMOVZXBQ:
            extend_source = 8;
            extend_destination = 64;
            break;
        case Op::VPMOVSXWD:
            signed_extend = true;
            [[fallthrough]];
        case Op::VPMOVZXWD:
            extend_source = 16;
            extend_destination = 32;
            break;
        case Op::VPMOVSXWQ:
            signed_extend = true;
            [[fallthrough]];
        case Op::VPMOVZXWQ:
            extend_source = 16;
            extend_destination = 64;
            break;
        case Op::VPMOVSXDQ:
            signed_extend = true;
            [[fallthrough]];
        case Op::VPMOVZXDQ:
            extend_source = 32;
            extend_destination = 64;
            break;
        default:
            break;
    }
    if (extend_source) {
        const auto source = read(instruction.ops[1], bytes * extend_source / extend_destination);
        std::array<Value, 2> output{};
        const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
        for (unsigned half = 0; half < bytes / 16; ++half) {
            auto input = source[0];
            if (half)
                input = Require(builder.VecByteShift(
                        Type::V128,
                        {Operand::ValueOf(input),
                         Operand::ValueOf(zero),
                         Operand::Immediate(16 * extend_source / extend_destination, Type::U8),
                         Operand::Immediate(0, Type::U8)}));
            output[half] =
                    Require(builder.VecExtend(Type::V128,
                                              {Operand::ValueOf(input),
                                               Operand::Immediate(extend_source, Type::U8),
                                               Operand::Immediate(extend_destination, Type::U8),
                                               Operand::Immediate(signed_extend, Type::U8)}));
        }
        write(output, bytes);
        return true;
    }
    if (opcode == Op::VMASKMOVPS || opcode == Op::VMASKMOVPD || opcode == Op::VPMASKMOVD ||
        opcode == Op::VPMASKMOVQ) {
        const unsigned bits = opcode == Op::VMASKMOVPD || opcode == Op::VPMASKMOVQ ? 64 : 32;
        const bool store = Memory(destination);
        const auto memory = store ? destination : instruction.ops[1];
        if (!Memory(memory) || !VRegister(instruction.ops[2])) Unsupported();
        const auto mask = read(instruction.ops[2], bytes);
        const auto data = store ? read(instruction.ops[1], bytes) : std::array<Value, 2>{};
        builder.PublishRegisters();
        const auto address = Address(builder, layout, instruction, memory, next_pc, target);
        std::array<Value, 2> output{};
        if (!store) output[0] = output[1] = Require(builder.VecSharedZero(Type::V128, {}));
        for (unsigned i = 0; i < bytes * 8 / bits; ++i) {
            const auto half = i * bits / 128, lane = i % (128 / bits);
            const auto raw_mask = VLane(builder, mask[half], bits, lane);
            const auto enabled =
                    Require(builder.TestBit(Type::Bool,
                                            {Operand::ValueOf(raw_mask, ScalarType(bits)),
                                             Operand::Immediate(bits - 1, Type::U8)}));
            const auto zero = store ? Value{} : Constant(builder, 0, ScalarType(bits));
            auto loaded = zero;
            builder.IfNonZero(enabled, [&] {
                PrepareFaultPC(builder, layout, pc, target);
                if (store)
                    StoreMemory(builder,
                                address,
                                VLane(builder, data[half], bits, lane),
                                bits,
                                i * bits / 8);
                else
                    loaded = LoadMemory(builder, ScalarType(bits), address, i * bits / 8);
            });
            if (!store) {
                // The load remains ordered inside its conditional region.
                // Select never commits its unexecuted arm on a zero mask;
                // DirectSink leaves `loaded` equal to the real zero value.
                const auto selected = Require(builder.Select(ScalarType(bits),
                                                             {Operand::ValueOf(enabled),
                                                              Operand::ValueOf(loaded),
                                                              Operand::ValueOf(zero)}));
                output[half] = VReplaceLane(builder, output[half], selected, bits, lane);
            }
        }
        if (!store) write(output, bytes);
        return true;
    }
    unsigned gather_element{}, gather_index{};
    switch (opcode) {
        case Op::VPGATHERDD:
        case Op::VGATHERDPS:
            gather_element = 32;
            gather_index = 32;
            break;
        case Op::VPGATHERDQ:
        case Op::VGATHERDPD:
            gather_element = 64;
            gather_index = 32;
            break;
        case Op::VPGATHERQD:
        case Op::VGATHERQPS:
            gather_element = 32;
            gather_index = 64;
            break;
        case Op::VPGATHERQQ:
        case Op::VGATHERQPD:
            gather_element = 64;
            gather_index = 64;
            break;
        default:
            break;
    }
    if (gather_element) {
        const auto memory = instruction.ops[1], mask_reg = instruction.ops[2];
        if (!VRegister(destination) || !VRegister(mask_reg) ||
            memory.type != GuestOperandKind::IndexedMemory || memory.index < Register::XMM0 ||
            memory.index > Register::XMM15 || destination.index == mask_reg.index ||
            destination.index == memory.index || mask_reg.index == memory.index)
            Unsupported();
        const unsigned count = bytes * 8 / std::max(gather_element, gather_index);
        const unsigned used_bits = count * gather_element;
        const DecodedOperand index_reg{
                GuestOperandKind::Register,
                memory.index,
                static_cast<std::uint16_t>(count * gather_index > 128 ? 256 : 128)};
        const auto indices = read(index_reg, count * gather_index > 128 ? 32 : 16),
                   mask = read(mask_reg, bytes);
        builder.PublishRegisters();
        builder.InvalidateXmm(destination.index - Register::XMM0);
        builder.InvalidateXmm(mask_reg.index - Register::XMM0);
        const auto slot = [&](const DecodedOperand& reg, unsigned bit) {
            return bit < 128 ? VOffset(layout, reg) + bit / 8
                             : AvxHighOffset(layout, reg) + (bit - 128) / 8;
        };
        const auto zero_unused = [&] {
            const auto zero = Constant(builder, 0);
            for (unsigned bit = used_bits; bit < 256; bit += 64)
                Require(builder.StoreUniform(Type::Void,
                                             {Operand::Uniform(slot(destination, bit), Type::U64),
                                              Operand::ValueOf(zero)}));
        };
        auto address_instruction = instruction;
        auto base_operand = memory;
        base_operand.index = Register::None;
        const unsigned scale = instruction.scale == 8   ? 3
                               : instruction.scale == 4 ? 2
                               : instruction.scale == 2 ? 1
                                                        : 0;
        if (instruction.scale != 1 && instruction.scale != 2 && instruction.scale != 4 &&
            instruction.scale != 8)
            Unsupported();
        for (unsigned i = 0; i < count; ++i) {
            const auto mask_value = VLane(builder,
                                          mask[i * gather_element / 128],
                                          gather_element,
                                          i % (128 / gather_element));
            const auto enabled = Require(
                    builder.TestBit(Type::Bool,
                                    {Operand::ValueOf(mask_value, ScalarType(gather_element)),
                                     Operand::Immediate(gather_element - 1, Type::U8)}));
            builder.IfNonZero(enabled, [&] {
                const auto raw_index = VLane(builder,
                                             indices[i * gather_index / 128],
                                             gather_index,
                                             i % (128 / gather_index));
                auto offset =
                        gather_index == 32
                                ? Require(builder.SignExtend(
                                          Type::U64, {Operand::ValueOf(raw_index, Type::U32)}))
                                : raw_index;
                if (scale)
                    offset = Require(builder.LslImm(
                            Type::U64,
                            {Operand::ValueOf(offset), Operand::Immediate(scale, Type::U8)}));
                PrepareFaultPC(builder, layout, pc, target);
                const auto address = Address(builder,
                                             layout,
                                             address_instruction,
                                             base_operand,
                                             next_pc,
                                             target,
                                             {},
                                             nullptr,
                                             Type::U64,
                                             offset);
                const auto value = LoadMemory(builder, ScalarType(gather_element), address);
                Require(builder.StoreUniform(
                        Type::Void,
                        {Operand::Uniform(slot(destination, i * gather_element),
                                          ScalarType(gather_element)),
                         Operand::ValueOf(value)}));
                const auto zero = Constant(builder, 0, ScalarType(gather_element));
                Require(builder.StoreUniform(Type::Void,
                                             {Operand::Uniform(slot(mask_reg, i * gather_element),
                                                               ScalarType(gather_element)),
                                              Operand::ValueOf(zero)}));
                zero_unused();
            });
        }
        zero_unused();
        const auto zero = Require(builder.VecSharedZero(Type::V128, {}));
        builder.WriteXmm(VOffset(layout, mask_reg), mask_reg.index - Register::XMM0, zero);
        Require(builder.StoreUniform(Type::Void,
                                     {Operand::Uniform(AvxHighOffset(layout, mask_reg), Type::V128),
                                      Operand::ValueOf(zero)}));
        return true;
    }
    bool unary{}, scalar{}, immediate_shift{};
    unsigned scalar_bits = 32;
    switch (opcode) {
        case Op::VSQRTPS:
        case Op::VSQRTPD:
        case Op::VRCPPS:
        case Op::VRSQRTPS:
        case Op::VPSHUFD:
        case Op::VPSHUFLW:
        case Op::VPSHUFHW:
        case Op::VMOVSLDUP:
        case Op::VMOVSHDUP:
        case Op::VMOVDDUP:
        case Op::VPABSB:
        case Op::VPABSW:
        case Op::VPABSD:
        case Op::VROUNDPS:
        case Op::VROUNDPD:
            unary = true;
            break;
        case Op::VADDSS:
        case Op::VSUBSS:
        case Op::VMULSS:
        case Op::VDIVSS:
        case Op::VSQRTSS:
        case Op::VRCPSS:
        case Op::VRSQRTSS:
        case Op::VMINSS:
        case Op::VMAXSS:
        case Op::VCMPSS:
        case Op::VROUNDSS:
            scalar = true;
            break;
        case Op::VADDSD:
        case Op::VSUBSD:
        case Op::VMULSD:
        case Op::VDIVSD:
        case Op::VSQRTSD:
        case Op::VMINSD:
        case Op::VMAXSD:
        case Op::VCMPSD:
        case Op::VROUNDSD:
            scalar = true;
            scalar_bits = 64;
            break;
        case Op::VPSLLDQ:
        case Op::VPSRLDQ:
            unary = true;
            immediate_shift = true;
            break;
        case Op::VPSLLW:
        case Op::VPSLLD:
        case Op::VPSLLQ:
        case Op::VPSRLW:
        case Op::VPSRLD:
        case Op::VPSRLQ:
        case Op::VPSRAW:
        case Op::VPSRAD:
            immediate_shift = instruction.ops[2].type == GuestOperandKind::Immediate;
            unary = immediate_shift;
            break;
        default:
            break;
    }
    if (!VRegister(destination)) Unsupported();
    const auto a =
            read(instruction.ops[1],
                 opcode == Op::VMOVDDUP && bytes == 16 && Memory(instruction.ops[1]) ? 8 : bytes);
    std::array<Value, 2> b{};
    if (unary)
        b = a;
    else {
        bool common_count = false;
        switch (opcode) {
            case Op::VPSLLW:
            case Op::VPSLLD:
            case Op::VPSLLQ:
            case Op::VPSRLW:
            case Op::VPSRLD:
            case Op::VPSRLQ:
            case Op::VPSRAW:
            case Op::VPSRAD:
                common_count = true;
                break;
            default:
                break;
        }
        b = read(instruction.ops[2], scalar ? scalar_bits / 8 : common_count ? 16 : bytes);
        if (common_count) b[0] = b[1] = VLane(builder, b[0], 64, 0);
    }
    std::array<Value, 2> output{};
    for (unsigned half = 0; half < bytes / 16; ++half) {
        unsigned control = immediate;
        if (opcode == Op::VSHUFPD || opcode == Op::VBLENDPD)
            control >>= half * 2;
        else if (opcode == Op::VBLENDPS || opcode == Op::VPBLENDD)
            control >>= half * 4;
        else if (opcode == Op::VMPSADBW)
            control >>= half * 3;
        output[half] = AvxLane(builder, layout, opcode, a[half], b[half], control, immediate_shift);
    }
    write(output, bytes);
    return true;
}
