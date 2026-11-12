// Neutral vector construction and numerical lane adapters. Values retain their
// active D/Q width so unused lanes never contribute floating-point exceptions.
using RoundingMode = runtime::arm32::RoundingMode;
using Numeric = runtime::arm32::Numeric;

struct NeonTable {
    std::array<Value, 4> entries{};
    size_t count{};
};

template <size_t B, class T> static constexpr bool NeonBit(T value) { return (value >> B) & 1; }

template <class T> static constexpr bool NeonBit(unsigned bit, T value) {
    return (value >> bit) & 1;
}

template <size_t B, size_t E, class T> static constexpr auto NeonBits(T value) {
    return bits::get_bits<B, E>(value);
}

template <class T> static constexpr auto NeonBits(unsigned begin, unsigned end, T value) {
    return (value >> begin) & (~T{0} >> (sizeof(T) * 8 - 1 - end + begin));
}

template <class T> static constexpr auto NeonHighestBit(T value) {
    return bits::highest_set_bit(value);
}

template <class T> static constexpr auto NeonLowestBit(T value) {
    return bits::lowest_set_bit(value);
}

static constexpr u64 NeonOnes(unsigned count) { return bits::ones<u64>(count); }

static constexpr Type NeonIntegerType(unsigned bits) {
    return bits == 8 ? Type::U8 : bits == 16 ? Type::U16 : bits == 32 ? Type::U32 : Type::U64;
}

static unsigned NeonWidth(Value value) { return value.ViewType() == Type::V64 ? 64 : 128; }

static unsigned ScalarWidth(Value value) {
    return value.ViewType() == Type::U64 || value.ViewType() == Type::S64   ? 64
           : value.ViewType() == Type::U16 || value.ViewType() == Type::S16 ? 16
                                                                            : 32;
}

Value ZeroVector() { return Emit<OpCode::VecSharedZero, Type::V128>(); }

Value ZeroExtendToQuad(Value value) {
    auto duplicate = Emit<OpCode::VecDup64, Type::V128>(value.WithView(Type::U64));
    return Emit<OpCode::VecZip, Type::V128>(duplicate, ZeroVector(), u64(64), u64(0));
}

Value VectorGetElement(size_t esize, Value vector, size_t index) {
    auto half = Emit<OpCode::VecExtract64, Type::U64>(vector.WithView(Type::V128),
                                                      u64(index * esize / 64));
    if (esize == 64) return half;
    return Emit<OpCode::BitExtract, Type::U64>(half, u64(index * esize % 64), u64(esize))
            .WithView(NeonIntegerType(esize));
}

Value VectorSetElement(size_t esize, Value vector, size_t index, Value element) {
    const auto type = vector.ViewType();
    if (esize == 16)
        return Emit<OpCode::VecInsert16, Type::V128>(
                       vector.WithView(Type::V128), element, u64(index))
                .WithView(type);
    if (esize == 8) {
        auto word = VectorGetElement(16, vector, index / 2).WithView(Type::U64);
        word = Emit<OpCode::BitInsert, Type::U64>(
                word, element.WithView(Type::U64), u64((index & 1) * 8), u64(8));
        return Emit<OpCode::VecInsert16, Type::V128>(
                       vector.WithView(Type::V128), word.WithView(Type::U16), u64(index / 2))
                .WithView(type);
    }
    if (esize == 32) {
        auto low = element.WithView(Type::U16);
        auto high = Emit<OpCode::LsrImm, Type::U64>(element.WithView(Type::U64), u64(16))
                            .WithView(Type::U16);
        auto result = Emit<OpCode::VecInsert16, Type::V128>(
                vector.WithView(Type::V128), low, u64(index * 2));
        return Emit<OpCode::VecInsert16, Type::V128>(result, high, u64(index * 2 + 1))
                .WithView(type);
    }
    auto duplicate = Emit<OpCode::VecDup64, Type::V128>(element.WithView(Type::U64));
    if (index == 1)
        return Emit<OpCode::VecZip, Type::V128>(
                       vector.WithView(Type::V128), duplicate, u64(64), u64(0))
                .WithView(type);
    auto upper = Emit<OpCode::VecExtract64, Type::U64>(vector.WithView(Type::V128), u64(1));
    auto upper_vector = Emit<OpCode::VecDup64, Type::V128>(upper);
    return Emit<OpCode::VecZip, Type::V128>(duplicate, upper_vector, u64(64), u64(0))
            .WithView(type);
}

Value VectorBroadcast(size_t esize, Value scalar) {
    auto result = Emit<OpCode::VecDup64, Type::V128>(scalar.WithView(Type::U64));
    if (esize == 64) return result;
    // Repeated ZIP duplicates the first lane without reading a neighboring
    // guest register or manufacturing an instruction-local heap buffer.
    for (size_t width = esize; width < 64; width *= 2)
        result = Emit<OpCode::VecZip, Type::V128>(result, result, u64(width), u64(0));
    return Emit<OpCode::VecZip, Type::V128>(result, result, u64(64), u64(0));
}

Value VectorBroadcastElement(size_t esize, Value vector, size_t index) {
    return VectorBroadcast(esize, VectorGetElement(esize, vector, index));
}

#define ARM32_VECTOR_BINARY(name, operation)                                                       \
    Value name(size_t bits, Value a, Value b) {                                                    \
        return Emit<OpCode::operation, Type::V128>(                                                \
                       a.WithView(Type::V128), b.WithView(Type::V128), u64(bits))                  \
                .WithView(a.ViewType());                                                           \
    }
ARM32_VECTOR_BINARY(VectorAdd, VecAdd)
ARM32_VECTOR_BINARY(VectorSub, VecSub)
ARM32_VECTOR_BINARY(VectorMultiply, VecMul)
ARM32_VECTOR_BINARY(VectorEqual, VecCmpEq)
ARM32_VECTOR_BINARY(VectorGreaterSigned, VecCmpGt)
#undef ARM32_VECTOR_BINARY
#define ARM32_VECTOR_LOGICAL(name, operation)                                                      \
    Value name(Value a, Value b) {                                                                 \
        return Emit<OpCode::operation, Type::V128>(a.WithView(Type::V128), b.WithView(Type::V128)) \
                .WithView(a.ViewType());                                                           \
    }
ARM32_VECTOR_LOGICAL(VectorAnd, VecAnd)
ARM32_VECTOR_LOGICAL(VectorAndNot, VecAndNot)
ARM32_VECTOR_LOGICAL(VectorOr, VecOr)
ARM32_VECTOR_LOGICAL(VectorEor, VecXor)
#undef ARM32_VECTOR_LOGICAL

Value VectorNot(Value a) {
    return VectorEor(a, Emit<OpCode::VecLoadConst, Type::V128>(~u64{}, ~u64{}));
}

Value VectorGreaterUnsigned(size_t bits, Value a, Value b) {
    auto sign = VectorBroadcast(bits, Imm64(u64{1} << (bits - 1)));
    return VectorGreaterSigned(bits, VectorEor(a, sign), VectorEor(b, sign));
}

Value VectorGreaterEqualSigned(size_t bits, Value a, Value b) {
    return VectorNot(VectorGreaterSigned(bits, b, a));
}

Value VectorGreaterEqualUnsigned(size_t bits, Value a, Value b) {
    return VectorNot(VectorGreaterUnsigned(bits, b, a));
}

Value VectorLessSigned(size_t bits, Value a, Value b) { return VectorGreaterSigned(bits, b, a); }

Value VectorLessUnsigned(size_t bits, Value a, Value b) {
    return VectorGreaterUnsigned(bits, b, a);
}

Value VectorLessEqualSigned(size_t bits, Value a, Value b) {
    return VectorGreaterEqualSigned(bits, b, a);
}

Value VectorLessEqualUnsigned(size_t bits, Value a, Value b) {
    return VectorGreaterEqualUnsigned(bits, b, a);
}

#define ARM32_VECTOR_MINMAX(name, operation, sign)                                                 \
    Value name(size_t bits, Value a, Value b) {                                                    \
        return Emit<OpCode::operation, Type::V128>(                                                \
                       a.WithView(Type::V128), b.WithView(Type::V128), u64(bits), u64(sign))       \
                .WithView(a.ViewType());                                                           \
    }
ARM32_VECTOR_MINMAX(VectorMaxSigned, VecMax, 1)
ARM32_VECTOR_MINMAX(VectorMaxUnsigned, VecMax, 0)
ARM32_VECTOR_MINMAX(VectorMinSigned, VecMin, 1)
ARM32_VECTOR_MINMAX(VectorMinUnsigned, VecMin, 0)
#undef ARM32_VECTOR_MINMAX
#define ARM32_VECTOR_SHIFT(name, operation)                                                        \
    Value name(size_t bits, Value a, u8 count) {                                                   \
        return Emit<OpCode::operation, Type::V128>(a.WithView(Type::V128), u64(count), u64(bits))  \
                .WithView(a.ViewType());                                                           \
    }
ARM32_VECTOR_SHIFT(VectorLogicalShiftLeft, VecShiftLeftImm)
ARM32_VECTOR_SHIFT(VectorLogicalShiftRight, VecShiftRightImm)
ARM32_VECTOR_SHIFT(VectorArithmeticShiftRight, VecShiftRightArithmeticImm)
#undef ARM32_VECTOR_SHIFT

Value VectorInterleaveLower(size_t bits, Value a, Value b) {
    return Emit<OpCode::VecZip, Type::V128>(
            a.WithView(Type::V128), b.WithView(Type::V128), u64(bits), u64(0));
}

Value VectorInterleaveUpper(size_t bits, Value a, Value b) {
    return Emit<OpCode::VecZip, Type::V128>(
            a.WithView(Type::V128), b.WithView(Type::V128), u64(bits), u64(1));
}

Value VectorDeinterleaveEven(size_t bits, Value a, Value b) {
    return Emit<OpCode::VecUnzip, Type::V128>(
            a.WithView(Type::V128), b.WithView(Type::V128), u64(bits), u64(0));
}

Value VectorDeinterleaveOdd(size_t bits, Value a, Value b) {
    return Emit<OpCode::VecUnzip, Type::V128>(
            a.WithView(Type::V128), b.WithView(Type::V128), u64(bits), u64(1));
}

Value VectorDeinterleaveLower(size_t bits, Value a, Value b, bool odd) {
    auto joined = VectorInterleaveLower(64, a, b);
    return Emit<OpCode::VecUnzip, Type::V128>(joined, ZeroVector(), u64(bits), u64(odd))
            .WithView(Type::V64);
}

Value VectorDeinterleaveEvenLower(size_t bits, Value a, Value b) {
    return VectorDeinterleaveLower(bits, a, b, false);
}

Value VectorDeinterleaveOddLower(size_t bits, Value a, Value b) {
    return VectorDeinterleaveLower(bits, a, b, true);
}

Value VectorTranspose(size_t bits, Value a, Value b, bool part) {
    auto first = Emit<OpCode::VecUnzip, Type::V128>(
            a.WithView(Type::V128), b.WithView(Type::V128), u64(bits), u64(part));
    auto low = VectorGetElement(64, first, 0), high = VectorGetElement(64, first, 1);
    return VectorInterleaveLower(bits, VectorBroadcast(64, low), VectorBroadcast(64, high))
            .WithView(a.ViewType());
}

Value VectorExtract(Value a, Value b, size_t bits) {
    return Emit<OpCode::VecExtractBytes, Type::V128>(
            a.WithView(Type::V128), b.WithView(Type::V128), u64(bits / 8));
}

Value VectorExtractLower(Value a, Value b, size_t bits) {
    auto joined = VectorInterleaveLower(64, a, b);
    return VectorExtract(joined, ZeroVector(), bits).WithView(Type::V64);
}

Value VectorSignExtend(size_t bits, Value a) {
    return Emit<OpCode::VecExtend, Type::V128>(
            a.WithView(Type::V128), u64(bits), u64(bits * 2), u64(1));
}

Value VectorZeroExtend(size_t bits, Value a) {
    return Emit<OpCode::VecExtend, Type::V128>(
            a.WithView(Type::V128), u64(bits), u64(bits * 2), u64(0));
}

Value VectorMultiplySignedWiden(size_t bits, Value a, Value b) {
    return Emit<OpCode::VecSignedMulWidenLow, Type::V128>(
            a.WithView(Type::V128), b.WithView(Type::V128), u64(bits));
}

Value VectorMultiplyUnsignedWiden(size_t bits, Value a, Value b) {
    auto left = VectorInterleaveLower(bits, a, a), right = VectorInterleaveLower(bits, b, b);
    return Emit<OpCode::VecMulWiden, Type::V128>(left, right, u64(bits), u64(0));
}

Value VectorNarrow(size_t bits, Value a) {
    return VectorDeinterleaveEven(bits / 2, a, ZeroVector()).WithView(Type::V64);
}

Value VectorPairedAddLower(size_t bits, Value a, Value b) {
    return VectorAdd(
            bits, VectorDeinterleaveEvenLower(bits, a, b), VectorDeinterleaveOddLower(bits, a, b));
}

Value VectorPairedAddWiden(size_t bits, Value a, bool sign) {
    // The low 64 bits of each widened input contain alternating source lanes.
    auto even = VectorDeinterleaveEven(bits, a, ZeroVector());
    auto odd = VectorDeinterleaveOdd(bits, a, ZeroVector());
    auto left = sign ? VectorSignExtend(bits, even) : VectorZeroExtend(bits, even);
    auto right = sign ? VectorSignExtend(bits, odd) : VectorZeroExtend(bits, odd);
    return VectorAdd(bits * 2, left, right).WithView(a.ViewType());
}

Value VectorPairedAddSignedWiden(size_t bits, Value a) {
    return VectorPairedAddWiden(bits, a, true);
}

Value VectorPairedAddUnsignedWiden(size_t bits, Value a) {
    return VectorPairedAddWiden(bits, a, false);
}

Value VectorAbs(size_t bits, Value a) {
    auto negative = VectorGreaterSigned(bits, ZeroVector().WithView(a.ViewType()), a);
    return VectorSub(bits, VectorEor(a, negative), negative);
}

Value VectorUnsignedAbsoluteDifference(size_t bits, Value a, Value b) {
    return VectorSub(bits, VectorMaxUnsigned(bits, a, b), VectorMinUnsigned(bits, a, b));
}

Value VectorSignedAbsoluteDifference(size_t bits, Value a, Value b) {
    return VectorSub(bits, VectorMaxSigned(bits, a, b), VectorMinSigned(bits, a, b));
}

Value VectorReverseGroups(size_t bits, Value a, unsigned group) {
    auto result = ZeroVector().WithView(a.ViewType());
    for (unsigned lane = 0; lane < NeonWidth(a) / bits; ++lane)
        result = VectorSetElement(
                bits, result, lane, VectorGetElement(bits, a, lane ^ (group / bits - 1)));
    return result;
}

Value VectorReverseElementsInHalfGroups(size_t bits, Value a) {
    return VectorReverseGroups(bits, a, 16);
}

Value VectorReverseElementsInWordGroups(size_t bits, Value a) {
    return VectorReverseGroups(bits, a, 32);
}

Value VectorReverseElementsInLongGroups(size_t bits, Value a) {
    return VectorReverseGroups(bits, a, 64);
}

Value VectorHalving(size_t bits, Value a, Value b, bool sign, bool subtract, bool round) {
    const auto different = VectorEor(a, b);
    const auto half = sign ? VectorArithmeticShiftRight(bits, different, 1)
                           : VectorLogicalShiftRight(bits, different, 1);
    if (subtract) return VectorSub(bits, half, VectorAndNot(b, a));
    if (round) return VectorSub(bits, VectorOr(a, b), half);
    return VectorAdd(bits, VectorAnd(a, b), half);
}

Value VectorHalvingAddSigned(size_t bits, Value a, Value b) {
    return VectorHalving(bits, a, b, true, false, false);
}

Value VectorHalvingAddUnsigned(size_t bits, Value a, Value b) {
    return VectorHalving(bits, a, b, false, false, false);
}

Value VectorHalvingSubSigned(size_t bits, Value a, Value b) {
    return VectorHalving(bits, a, b, true, true, false);
}

Value VectorHalvingSubUnsigned(size_t bits, Value a, Value b) {
    return VectorHalving(bits, a, b, false, true, false);
}

Value VectorRoundingHalvingAddSigned(size_t bits, Value a, Value b) {
    return VectorHalving(bits, a, b, true, false, true);
}

Value VectorRoundingHalvingAddUnsigned(size_t bits, Value a, Value b) {
    return VectorHalving(bits, a, b, false, false, true);
}

void AccumulateVectorSaturation(size_t bits, Value wrapped, Value saturated) {
    auto unchanged = VectorEqual(bits, wrapped, saturated);
    auto mask = Emit<OpCode::VecMovMask, Type::U32>(unchanged.WithView(Type::V128), u64(bits));
    auto active = Imm32(u32((u64{1} << (NeonWidth(wrapped) / bits)) - 1));
    auto differs = Eor(And(mask, active), active);
    auto status = Emit<OpCode::LoadUniform, Type::U32>(Uniform{layout.fpscr_offset, Type::U32});
    auto qc = ConditionalSelect(TestNotZero(differs), Imm32(1u << 27), Imm32(0));
    Emit<OpCode::StoreUniform, Type::Void>(Uniform{layout.fpscr_offset, Type::U32}, Or(status, qc));
}

Value VectorSaturatedArithmetic(size_t bits, Value a, Value b, bool sign, bool subtract) {
    auto result =
            subtract
                    ? Emit<OpCode::VecSatSub, Type::V128>(
                              a.WithView(Type::V128), b.WithView(Type::V128), u64(bits), u64(sign))
                    : Emit<OpCode::VecSatAdd, Type::V128>(
                              a.WithView(Type::V128), b.WithView(Type::V128), u64(bits), u64(sign));
    result = result.WithView(a.ViewType());
    AccumulateVectorSaturation(
            bits, subtract ? VectorSub(bits, a, b) : VectorAdd(bits, a, b), result);
    return result;
}

Value VectorSignedSaturatedAdd(size_t bits, Value a, Value b) {
    return VectorSaturatedArithmetic(bits, a, b, true, false);
}

Value VectorUnsignedSaturatedAdd(size_t bits, Value a, Value b) {
    return VectorSaturatedArithmetic(bits, a, b, false, false);
}

Value VectorSignedSaturatedSub(size_t bits, Value a, Value b) {
    return VectorSaturatedArithmetic(bits, a, b, true, true);
}

Value VectorUnsignedSaturatedSub(size_t bits, Value a, Value b) {
    return VectorSaturatedArithmetic(bits, a, b, false, true);
}

Value VectorSignedSaturatedNeg(size_t bits, Value a) {
    return VectorSignedSaturatedSub(bits, ZeroVector().WithView(a.ViewType()), a);
}

Value VectorSignedSaturatedAbs(size_t bits, Value a) {
    auto mask = VectorGreaterSigned(bits, ZeroVector().WithView(a.ViewType()), a);
    auto negative = VectorSignedSaturatedNeg(bits, a);
    return VectorOr(VectorAnd(negative, mask), VectorAndNot(a, mask));
}

Value VectorCountLeadingZeros(size_t bits, Value a) {
    auto result = ZeroVector().WithView(a.ViewType());
    for (size_t lane = 0; lane < NeonWidth(a) / bits; ++lane) {
        auto input = VectorGetElement(bits, a, lane);
        auto count =
                bits == 64
                        ? Emit<OpCode::CountLeadingZeros64, Type::U64>(input)
                        : Emit<OpCode::CountLeadingZeros32, Type::U32>(input.WithView(Type::U32));
        if (bits < 32) count = Sub(count, Imm32(32 - bits));
        result = VectorSetElement(bits, result, lane, count);
    }
    return result;
}

template <Numeric operation, bool standard = false>
Value NumericScalar(size_t bits, Value a, Value b) {
    runtime::arm32::NumericCallback callback;
    switch (bits) {
        case 8:
            callback = &runtime::arm32::NumericLane<operation, 8, standard>;
            break;
        case 16:
            callback = &runtime::arm32::NumericLane<operation, 16, standard>;
            break;
        case 32:
            callback = &runtime::arm32::NumericLane<operation, 32, standard>;
            break;
        default:
            callback = &runtime::arm32::NumericLane<operation, 64, standard>;
            break;
    }
    auto address = Emit<OpCode::GetUniformAddress, Type::U64>(u64(layout.fpscr_offset));
    Operand lambda;
    lambda.kind = OperandKind::Lambda;
    return Emit<OpCode::CallLambda, Type::U64>(lambda,
                                               u64(reinterpret_cast<uintptr_t>(callback)),
                                               address,
                                               a.WithView(Type::U64),
                                               b.WithView(Type::U64));
}

template <Numeric operation, bool standard = false>
Value NumericVector(size_t bits, Value a, Value b, size_t output_bits = 0) {
    const size_t out = output_bits ? output_bits : bits;
    auto result = ZeroVector().WithView(a.ViewType());
    for (size_t lane = 0; lane < NeonWidth(a) / bits; ++lane) {
        auto first = VectorGetElement(bits, a, lane), second = VectorGetElement(bits, b, lane);
        result = VectorSetElement(
                out, result, lane, NumericScalar<operation, standard>(bits, first, second));
    }
    return result;
}

#define ARM32_VECTOR_NUMERIC_BINARY(name, operation)                                               \
    Value name(size_t bits, Value a, Value b) {                                                    \
        return NumericVector<Numeric::operation>(bits, a, b);                                      \
    }
ARM32_VECTOR_NUMERIC_BINARY(VectorArithmeticVShift, ShiftSigned)
ARM32_VECTOR_NUMERIC_BINARY(VectorLogicalVShift, ShiftUnsigned)
ARM32_VECTOR_NUMERIC_BINARY(VectorRoundingShiftLeftSigned, RoundingShiftSigned)
ARM32_VECTOR_NUMERIC_BINARY(VectorRoundingShiftLeftUnsigned, RoundingShiftUnsigned)
ARM32_VECTOR_NUMERIC_BINARY(VectorSignedSaturatedShiftLeft, SaturatingShiftSigned)
ARM32_VECTOR_NUMERIC_BINARY(VectorUnsignedSaturatedShiftLeft, SaturatingShiftUnsigned)
ARM32_VECTOR_NUMERIC_BINARY(VectorSignedSaturatedDoublingMultiplyHigh, DoublingMultiplyHigh)
ARM32_VECTOR_NUMERIC_BINARY(VectorSignedSaturatedDoublingMultiplyHighRounding,
                            RoundingDoublingMultiplyHigh)
#undef ARM32_VECTOR_NUMERIC_BINARY
#define ARM32_VECTOR_NUMERIC_UNARY(name, operation)                                                \
    Value name(size_t bits, Value a) {                                                             \
        return NumericVector<Numeric::operation>(bits, a, ZeroVector());                           \
    }
#undef ARM32_VECTOR_NUMERIC_UNARY

Value VectorPopulationCount(Value a) {
    return NumericVector<Numeric::PopulationCount>(8, a, ZeroVector());
}

Value VectorUnsignedRecipEstimate(Value a) {
    return NumericVector<Numeric::UnsignedReciprocalEstimate>(32, a, ZeroVector());
}

Value VectorUnsignedRecipSqrtEstimate(Value a) {
    return NumericVector<Numeric::UnsignedRSqrtEstimate>(32, a, ZeroVector());
}

Value VectorSignedSaturatedNarrowToSigned(size_t bits, Value a) {
    return NumericVector<Numeric::SaturatingNarrowSigned>(bits, a, ZeroVector(), bits / 2)
            .WithView(Type::V64);
}

Value VectorSignedSaturatedNarrowToUnsigned(size_t bits, Value a) {
    return NumericVector<Numeric::SaturatingNarrowSignedToUnsigned>(bits, a, ZeroVector(), bits / 2)
            .WithView(Type::V64);
}

Value VectorUnsignedSaturatedNarrow(size_t bits, Value a) {
    return NumericVector<Numeric::SaturatingNarrowUnsigned>(bits, a, ZeroVector(), bits / 2)
            .WithView(Type::V64);
}

Value VectorSignedSaturatedShiftLeftUnsigned(size_t bits, Value a, u8 shift) {
    return NumericVector<Numeric::SaturatingShiftSignedToUnsigned>(
            bits, a, VectorBroadcast(bits, Imm64(shift)));
}

Value VectorSignedSaturatedDoublingMultiplyLong(size_t bits, Value a, Value b) {
    auto result = ZeroVector();
    for (size_t lane = 0; lane < 64 / bits; ++lane)
        result = VectorSetElement(
                bits * 2,
                result,
                lane,
                NumericScalar<Numeric::DoublingMultiplyLong>(
                        bits, VectorGetElement(bits, a, lane), VectorGetElement(bits, b, lane)));
    return result;
}

Value VectorPolynomialMultiply(Value a, Value b) {
    return NumericVector<Numeric::PolynomialMultiply>(8, a, b);
}

Value VectorPolynomialMultiplyLong(size_t bits, Value a, Value b) {
    if (bits == 64)
        return Emit<OpCode::VecPclMul, Type::V128>(
                a.WithView(Type::V128), b.WithView(Type::V128), u64(0));
    auto result = ZeroVector();
    for (size_t lane = 0; lane < 64 / bits; ++lane)
        result = VectorSetElement(
                bits * 2,
                result,
                lane,
                NumericScalar<Numeric::PolynomialMultiply>(
                        bits, VectorGetElement(bits, a, lane), VectorGetElement(bits, b, lane)));
    return result;
}

Value VectorTableLookup(Value defaults, const NeonTable& table, Value indices) {
    auto result = VectorBroadcast(64, defaults).WithView(Type::V64);
    auto source_indices = VectorBroadcast(64, indices);
    auto matched = ZeroVector();
    auto chosen = ZeroVector();
    for (size_t first = 0; first < table.count; first += 2) {
        auto lo = VectorBroadcast(64, table.entries[first]);
        auto hi = first + 1 < table.count ? VectorBroadcast(64, table.entries[first + 1])
                                          : ZeroVector();
        auto input = VectorInterleaveLower(64, lo, hi);
        auto adjusted = VectorSub(8, source_indices, VectorBroadcast(8, Imm8(first * 8)));
        auto valid = VectorGreaterUnsigned(
                8, VectorBroadcast(8, Imm8((first + 1 < table.count) ? 16 : 8)), adjusted);
        auto candidate =
                Emit<OpCode::VecTableLookup8, Type::V128>(input, adjusted.WithView(Type::V128));
        chosen = VectorOr(chosen, VectorAnd(candidate, valid));
        matched = VectorOr(matched, valid);
    }
    result = VectorOr(VectorAnd(chosen, matched), VectorAndNot(result, matched));
    return VectorGetElement(64, result, 0);
}

Value AESDecryptSingleRound(Value state) {
    return Emit<OpCode::VecAesDecLast, Type::V128>(state.WithView(Type::V128), ZeroVector());
}

Value AESEncryptSingleRound(Value state) {
    return Emit<OpCode::VecAesEncLast, Type::V128>(state.WithView(Type::V128), ZeroVector());
}

Value AESInverseMixColumns(Value state) {
    return Emit<OpCode::VecAesImc, Type::V128>(state.WithView(Type::V128));
}

Value AESMixColumns(Value state) {
    // MixColumns is the inverse transform applied three times.
    return AESInverseMixColumns(AESInverseMixColumns(AESInverseMixColumns(state)));
}

Value SHA256Rotate(Value a, unsigned count) {
    return Emit<OpCode::RorImm, Type::U32>(a.WithView(Type::U32), u64(count));
}

Value SHA256SmallSigma(Value a, bool second) {
    auto first = SHA256Rotate(a, second ? 17 : 7), next = SHA256Rotate(a, second ? 19 : 18);
    auto shift = Emit<OpCode::LsrImm, Type::U32>(a.WithView(Type::U32), u64(second ? 10 : 3));
    return Eor(Eor(first, next), shift);
}

Value SHA256BigSigma(Value a, bool second) {
    return Eor(Eor(SHA256Rotate(a, second ? 6 : 2), SHA256Rotate(a, second ? 11 : 13)),
               SHA256Rotate(a, second ? 25 : 22));
}

Value SHA256MessageSchedule0(Value a, Value b) {
    auto result = ZeroVector();
    for (unsigned lane = 0; lane < 4; ++lane) {
        auto next = lane < 3 ? VectorGetElement(32, a, lane + 1) : VectorGetElement(32, b, 0);
        result =
                VectorSetElement(32,
                                 result,
                                 lane,
                                 Add(VectorGetElement(32, a, lane), SHA256SmallSigma(next, false)));
    }
    return result;
}

Value SHA256MessageSchedule1(Value a, Value b, Value c) {
    auto result = ZeroVector();
    for (unsigned lane = 0; lane < 4; ++lane) {
        auto middle = lane < 3 ? VectorGetElement(32, b, lane + 1) : VectorGetElement(32, c, 0);
        auto source = lane < 2 ? VectorGetElement(32, c, lane + 2)
                               : VectorGetElement(32, result, lane - 2);
        auto value =
                Add(Add(VectorGetElement(32, a, lane), middle), SHA256SmallSigma(source, true));
        result = VectorSetElement(32, result, lane, value);
    }
    return result;
}

Value SHA256Hash(Value x, Value y, Value schedule, bool part) {
    std::array<Value, 8> work;
    for (unsigned i = 0; i < 4; ++i) {
        work[i] = VectorGetElement(32, x, i);
        work[i + 4] = VectorGetElement(32, y, i);
    }
    for (unsigned round = 0; round < 4; ++round) {
        auto choice = Eor(And(work[4], work[5]), And(Not(work[4]), work[6]));
        auto majority =
                Eor(Eor(And(work[0], work[1]), And(work[0], work[2])), And(work[1], work[2]));
        auto first = Add(Add(Add(work[7], SHA256BigSigma(work[4], true)), choice),
                         VectorGetElement(32, schedule, round));
        auto second = Add(SHA256BigSigma(work[0], false), majority);
        work = {Add(first, second),
                work[0],
                work[1],
                work[2],
                Add(work[3], first),
                work[4],
                work[5],
                work[6]};
    }
    auto result = ZeroVector();
    for (unsigned lane = 0; lane < 4; ++lane)
        result = VectorSetElement(32, result, lane, work[(part ? 0 : 4) + lane]);
    return result;
}

/* This file is part of the dynarmic project.
 * Copyright (c) 2020 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

enum class ThreeRegsAccumulateBehavior {
    None,
    Accumulate,
};

enum class ThreeRegsWidenBehaviour {
    Second,
    Both,
};

template <bool WithDst, typename Callable> bool BitwiseInstruction(
        bool D, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm, Callable fn) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    if constexpr (WithDst) {
        const Value reg_d = ir.GetVector(d);
        const Value reg_m = ir.GetVector(m);
        const Value reg_n = ir.GetVector(n);
        const Value result = fn(reg_d, reg_n, reg_m);
        ir.SetVector(d, result);
    } else {
        const Value reg_m = ir.GetVector(m);
        const Value reg_n = ir.GetVector(n);
        const Value result = fn(reg_n, reg_m);
        ir.SetVector(d, result);
    }

    return true;
}

bool IntegerComparison(bool U,
                       bool D,
                       size_t sz,
                       size_t Vn,
                       size_t Vd,
                       bool N,
                       bool Q,
                       bool M,
                       size_t Vm,
                       ThreeRegsComparison comparison) {
    if (sz == 0b11) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const size_t esize = 8 << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const auto reg_n = ir.GetVector(n);
    const auto reg_m = ir.GetVector(m);
    const auto result = [&] {
        switch (comparison) {
            case ThreeRegsComparison::GT:
                return U ? ir.VectorGreaterUnsigned(esize, reg_n, reg_m)
                         : ir.VectorGreaterSigned(esize, reg_n, reg_m);
            case ThreeRegsComparison::GE:
                return U ? ir.VectorGreaterEqualUnsigned(esize, reg_n, reg_m)
                         : ir.VectorGreaterEqualSigned(esize, reg_n, reg_m);
            case ThreeRegsComparison::EQ:
                return ir.VectorEqual(esize, reg_n, reg_m);
            default:
                return Value{};
        }
    }();

    ir.SetVector(d, result);
    return true;
}

bool AbsoluteDifference(bool U,
                        bool D,
                        size_t sz,
                        size_t Vn,
                        size_t Vd,
                        bool N,
                        bool Q,
                        bool M,
                        size_t Vm,
                        ThreeRegsAccumulateBehavior accumulate) {
    if (sz == 0b11) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const auto reg_m = ir.GetVector(m);
    const auto reg_n = ir.GetVector(n);
    const auto result = [&] {
        const auto absdiff = U ? ir.VectorUnsignedAbsoluteDifference(esize, reg_n, reg_m)
                               : ir.VectorSignedAbsoluteDifference(esize, reg_n, reg_m);

        if (accumulate == ThreeRegsAccumulateBehavior::Accumulate) {
            const auto reg_d = ir.GetVector(d);
            return ir.VectorAdd(esize, reg_d, absdiff);
        }

        return absdiff;
    }();

    ir.SetVector(d, result);
    return true;
}

bool AbsoluteDifferenceLong(bool U,
                            bool D,
                            size_t sz,
                            size_t Vn,
                            size_t Vd,
                            bool N,
                            bool M,
                            size_t Vm,
                            ThreeRegsAccumulateBehavior accumulate) {
    if (sz == 0b11) {
        return DecodeError();
    }

    if (NeonBit<0>(Vd)) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(true, Vd, D);
    const auto m = ToVector(false, Vm, M);
    const auto n = ToVector(false, Vn, N);

    const auto reg_m = ir.GetVector(m);
    const auto reg_n = ir.GetVector(n);
    const auto operand_m =
            ir.VectorZeroExtend(esize, ir.ZeroExtendToQuad(ir.VectorGetElement(64, reg_m, 0)));
    const auto operand_n =
            ir.VectorZeroExtend(esize, ir.ZeroExtendToQuad(ir.VectorGetElement(64, reg_n, 0)));
    const auto result = [&] {
        const auto absdiff = U ? ir.VectorUnsignedAbsoluteDifference(esize, operand_m, operand_n)
                               : ir.VectorSignedAbsoluteDifference(esize, operand_m, operand_n);

        if (accumulate == ThreeRegsAccumulateBehavior::Accumulate) {
            const auto reg_d = ir.GetVector(d);
            return ir.VectorAdd(2 * esize, reg_d, absdiff);
        }

        return absdiff;
    }();

    ir.SetVector(d, result);
    return true;
}

template <typename Callable> bool WideInstruction(bool U,
                                                  bool D,
                                                  size_t sz,
                                                  size_t Vn,
                                                  size_t Vd,
                                                  bool N,
                                                  bool M,
                                                  size_t Vm,
                                                  ThreeRegsWidenBehaviour widen_behaviour,
                                                  Callable fn) {
    const size_t esize = 8U << sz;
    const bool widen_first = widen_behaviour == ThreeRegsWidenBehaviour::Both;

    if (sz == 0b11) {
        return DecodeError();
    }

    if (NeonBit<0>(Vd) || (!widen_first && NeonBit<0>(Vn))) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(true, Vd, D);
    const auto m = ToVector(false, Vm, M);
    const auto n = ToVector(!widen_first, Vn, N);

    const auto reg_d = ir.GetVector(d);
    const auto reg_m = ir.GetVector(m);
    const auto reg_n = ir.GetVector(n);
    const auto wide_n = U ? ir.VectorZeroExtend(esize, reg_n) : ir.VectorSignExtend(esize, reg_n);
    const auto wide_m = U ? ir.VectorZeroExtend(esize, reg_m) : ir.VectorSignExtend(esize, reg_m);
    const auto result = fn(esize * 2, reg_d, widen_first ? wide_n : reg_n, wide_m);

    ir.SetVector(d, result);
    return true;
}

// ASIMD Three registers of the same length

bool asimd_VHADD(
        bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    if (sz == 0b11) {
        return UndefinedInstruction();
    }

    const size_t esize = 8 << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const Value reg_n = ir.GetVector(n);
    const Value reg_m = ir.GetVector(m);
    const Value result = U ? ir.VectorHalvingAddUnsigned(esize, reg_n, reg_m)
                           : ir.VectorHalvingAddSigned(esize, reg_n, reg_m);
    ir.SetVector(d, result);

    return true;
}

bool asimd_VQADD(
        bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    if (sz == 0b11) {
        return UndefinedInstruction();
    }

    const size_t esize = 8 << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const Value reg_n = ir.GetVector(n);
    const Value reg_m = ir.GetVector(m);
    const Value result = U ? ir.VectorUnsignedSaturatedAdd(esize, reg_n, reg_m)
                           : ir.VectorSignedSaturatedAdd(esize, reg_n, reg_m);
    ir.SetVector(d, result);

    return true;
}

bool asimd_VRHADD(
        bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    if (sz == 0b11) {
        return UndefinedInstruction();
    }

    const size_t esize = 8 << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const Value reg_n = ir.GetVector(n);
    const Value reg_m = ir.GetVector(m);
    const Value result = U ? ir.VectorRoundingHalvingAddUnsigned(esize, reg_n, reg_m)
                           : ir.VectorRoundingHalvingAddSigned(esize, reg_n, reg_m);
    ir.SetVector(d, result);

    return true;
}

bool asimd_VAND_reg(bool D, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return BitwiseInstruction<false>(
            D, Vn, Vd, N, Q, M, Vm, [this](const auto& reg_n, const auto& reg_m) {
                return ir.VectorAnd(reg_n, reg_m);
            });
}

bool asimd_VBIC_reg(bool D, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return BitwiseInstruction<false>(
            D, Vn, Vd, N, Q, M, Vm, [this](const auto& reg_n, const auto& reg_m) {
                return ir.VectorAndNot(reg_n, reg_m);
            });
}

bool asimd_VORR_reg(bool D, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return BitwiseInstruction<false>(
            D, Vn, Vd, N, Q, M, Vm, [this](const auto& reg_n, const auto& reg_m) {
                return ir.VectorOr(reg_n, reg_m);
            });
}

bool asimd_VORN_reg(bool D, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return BitwiseInstruction<false>(
            D, Vn, Vd, N, Q, M, Vm, [this](const auto& reg_n, const auto& reg_m) {
                return ir.VectorOr(reg_n, ir.VectorNot(reg_m));
            });
}

bool asimd_VEOR_reg(bool D, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return BitwiseInstruction<false>(
            D, Vn, Vd, N, Q, M, Vm, [this](const auto& reg_n, const auto& reg_m) {
                return ir.VectorEor(reg_n, reg_m);
            });
}

bool asimd_VBSL(bool D, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return BitwiseInstruction<true>(
            D,
            Vn,
            Vd,
            N,
            Q,
            M,
            Vm,
            [this](const auto& reg_d, const auto& reg_n, const auto& reg_m) {
                return ir.VectorOr(ir.VectorAnd(reg_n, reg_d), ir.VectorAndNot(reg_m, reg_d));
            });
}

bool asimd_VBIT(bool D, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return BitwiseInstruction<true>(
            D,
            Vn,
            Vd,
            N,
            Q,
            M,
            Vm,
            [this](const auto& reg_d, const auto& reg_n, const auto& reg_m) {
                return ir.VectorOr(ir.VectorAnd(reg_n, reg_m), ir.VectorAndNot(reg_d, reg_m));
            });
}

bool asimd_VBIF(bool D, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return BitwiseInstruction<true>(
            D,
            Vn,
            Vd,
            N,
            Q,
            M,
            Vm,
            [this](const auto& reg_d, const auto& reg_n, const auto& reg_m) {
                return ir.VectorOr(ir.VectorAnd(reg_d, reg_m), ir.VectorAndNot(reg_n, reg_m));
            });
}

bool asimd_VHSUB(
        bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    if (sz == 0b11) {
        return UndefinedInstruction();
    }

    const size_t esize = 8 << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const Value reg_n = ir.GetVector(n);
    const Value reg_m = ir.GetVector(m);
    const Value result = U ? ir.VectorHalvingSubUnsigned(esize, reg_n, reg_m)
                           : ir.VectorHalvingSubSigned(esize, reg_n, reg_m);
    ir.SetVector(d, result);

    return true;
}

bool asimd_VQSUB(
        bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    if (sz == 0b11) {
        return UndefinedInstruction();
    }

    const size_t esize = 8 << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const Value reg_n = ir.GetVector(n);
    const Value reg_m = ir.GetVector(m);
    const Value result = U ? ir.VectorUnsignedSaturatedSub(esize, reg_n, reg_m)
                           : ir.VectorSignedSaturatedSub(esize, reg_n, reg_m);
    ir.SetVector(d, result);

    return true;
}

bool asimd_VCGT_reg(
        bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return IntegerComparison(U, D, sz, Vn, Vd, N, Q, M, Vm, ThreeRegsComparison::GT);
}

bool asimd_VCGE_reg(
        bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return IntegerComparison(U, D, sz, Vn, Vd, N, Q, M, Vm, ThreeRegsComparison::GE);
}

bool asimd_VABD(
        bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return AbsoluteDifference(U, D, sz, Vn, Vd, N, Q, M, Vm, ThreeRegsAccumulateBehavior::None);
}

bool asimd_VABA(
        bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return AbsoluteDifference(
            U, D, sz, Vn, Vd, N, Q, M, Vm, ThreeRegsAccumulateBehavior::Accumulate);
}

bool asimd_VADD_int(bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const auto reg_m = ir.GetVector(m);
    const auto reg_n = ir.GetVector(n);
    const auto result = ir.VectorAdd(esize, reg_n, reg_m);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VSUB_int(bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const auto reg_m = ir.GetVector(m);
    const auto reg_n = ir.GetVector(n);
    const auto result = ir.VectorSub(esize, reg_n, reg_m);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VSHL_reg(
        bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const auto reg_m = ir.GetVector(m);
    const auto reg_n = ir.GetVector(n);
    const auto result = U ? ir.VectorLogicalVShift(esize, reg_m, reg_n)
                          : ir.VectorArithmeticVShift(esize, reg_m, reg_n);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VQSHL_reg(
        bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const auto reg_m = ir.GetVector(m);
    const auto reg_n = ir.GetVector(n);
    const auto result = U ? ir.VectorUnsignedSaturatedShiftLeft(esize, reg_m, reg_n)
                          : ir.VectorSignedSaturatedShiftLeft(esize, reg_m, reg_n);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VRSHL(
        bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const auto reg_m = ir.GetVector(m);
    const auto reg_n = ir.GetVector(n);
    const auto result = U ? ir.VectorRoundingShiftLeftUnsigned(esize, reg_m, reg_n)
                          : ir.VectorRoundingShiftLeftSigned(esize, reg_m, reg_n);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VMAX(bool U,
                bool D,
                size_t sz,
                size_t Vn,
                size_t Vd,
                bool N,
                bool Q,
                bool M,
                bool op,
                size_t Vm) {
    if (sz == 0b11) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const auto reg_m = ir.GetVector(m);
    const auto reg_n = ir.GetVector(n);
    const auto result = [&] {
        if (op) {
            return U ? ir.VectorMinUnsigned(esize, reg_n, reg_m)
                     : ir.VectorMinSigned(esize, reg_n, reg_m);
        } else {
            return U ? ir.VectorMaxUnsigned(esize, reg_n, reg_m)
                     : ir.VectorMaxSigned(esize, reg_n, reg_m);
        }
    }();

    ir.SetVector(d, result);
    return true;
}

bool asimd_VTST(bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    if (sz == 0b11) {
        return UndefinedInstruction();
    }

    const size_t esize = 8 << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const auto reg_n = ir.GetVector(n);
    const auto reg_m = ir.GetVector(m);
    const auto anded = ir.VectorAnd(reg_n, reg_m);
    const auto result = ir.VectorNot(ir.VectorEqual(esize, anded, ir.ZeroVector()));

    ir.SetVector(d, result);
    return true;
}

bool asimd_VCEQ_reg(bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return IntegerComparison(false, D, sz, Vn, Vd, N, Q, M, Vm, ThreeRegsComparison::EQ);
}

bool asimd_VMLA(
        bool op, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (sz == 0b11) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const auto reg_n = ir.GetVector(n);
    const auto reg_m = ir.GetVector(m);
    const auto reg_d = ir.GetVector(d);
    const auto multiply = ir.VectorMultiply(esize, reg_n, reg_m);
    const auto result =
            op ? ir.VectorSub(esize, reg_d, multiply) : ir.VectorAdd(esize, reg_d, multiply);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VMUL(
        bool P, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (sz == 0b11 || (P && sz != 0b00)) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const auto reg_n = ir.GetVector(n);
    const auto reg_m = ir.GetVector(m);
    const auto result =
            P ? ir.VectorPolynomialMultiply(reg_n, reg_m) : ir.VectorMultiply(esize, reg_n, reg_m);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VPMAX_int(bool U,
                     bool D,
                     size_t sz,
                     size_t Vn,
                     size_t Vd,
                     bool N,
                     bool Q,
                     bool M,
                     bool op,
                     size_t Vm) {
    if (sz == 0b11 || Q) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(false, Vd, D);
    const auto m = ToVector(false, Vm, M);
    const auto n = ToVector(false, Vn, N);

    const auto reg_m = ir.GetVector(m);
    const auto reg_n = ir.GetVector(n);

    const auto bottom = ir.VectorDeinterleaveEvenLower(esize, reg_n, reg_m);
    const auto top = ir.VectorDeinterleaveOddLower(esize, reg_n, reg_m);

    const auto result = [&] {
        if (op) {
            return U ? ir.VectorMinUnsigned(esize, bottom, top)
                     : ir.VectorMinSigned(esize, bottom, top);
        } else {
            return U ? ir.VectorMaxUnsigned(esize, bottom, top)
                     : ir.VectorMaxSigned(esize, bottom, top);
        }
    }();

    ir.SetVector(d, result);
    return true;
}

bool asimd_VQDMULH(bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    if (sz == 0b00 || sz == 0b11) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const auto reg_n = ir.GetVector(n);
    const auto reg_m = ir.GetVector(m);
    const auto result = ir.VectorSignedSaturatedDoublingMultiplyHigh(esize, reg_n, reg_m);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VQRDMULH(bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    if (sz == 0b00 || sz == 0b11) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const auto reg_n = ir.GetVector(n);
    const auto reg_m = ir.GetVector(m);
    const auto result = ir.VectorSignedSaturatedDoublingMultiplyHighRounding(esize, reg_n, reg_m);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VPADD(bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (Q || sz == 0b11) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const auto reg_n = ir.GetVector(n);
    const auto reg_m = ir.GetVector(m);
    const auto result = ir.VectorPairedAddLower(esize, reg_n, reg_m);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VACGE(
        bool D, bool op, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    const auto comparison = op ? ThreeRegsComparison::AbsoluteGT : ThreeRegsComparison::AbsoluteGE;
    return FloatComparison(D, sz, Vn, Vd, N, Q, M, Vm, comparison);
}

bool v8_SHA256H(bool D, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (!Q || NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm)) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(Q, Vd, D);
    const auto n = ToVector(Q, Vn, N);
    const auto m = ToVector(Q, Vm, M);

    const auto x = ir.GetVector(d);
    const auto y = ir.GetVector(n);
    const auto w = ir.GetVector(m);
    const auto result = ir.SHA256Hash(x, y, w, true);

    ir.SetVector(d, result);
    return true;
}

bool v8_SHA256H2(bool D, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (!Q || NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm)) {
        return UndefinedInstruction();
    }

    const auto n = ToVector(Q, Vn, N);
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);

    const auto x = ir.GetVector(n);
    const auto y = ir.GetVector(d);
    const auto w = ir.GetVector(m);
    const auto result = ir.SHA256Hash(x, y, w, false);

    ir.SetVector(d, result);
    return true;
}

bool v8_SHA256SU1(bool D, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (!Q || NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm)) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(Q, Vd, D);
    const auto n = ToVector(Q, Vn, N);
    const auto m = ToVector(Q, Vm, M);

    const auto x = ir.GetVector(d);
    const auto y = ir.GetVector(n);
    const auto z = ir.GetVector(m);
    const auto result = ir.SHA256MessageSchedule1(x, y, z);

    ir.SetVector(d, result);
    return true;
}

// ASIMD Three registers of different length

bool asimd_VADDL(
        bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool op, bool N, bool M, size_t Vm) {
    return WideInstruction(U,
                           D,
                           sz,
                           Vn,
                           Vd,
                           N,
                           M,
                           Vm,
                           op ? ThreeRegsWidenBehaviour::Second : ThreeRegsWidenBehaviour::Both,
                           [this](size_t esize, const auto&, const auto& reg_n, const auto& reg_m) {
                               return ir.VectorAdd(esize, reg_n, reg_m);
                           });
}

bool asimd_VSUBL(
        bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool op, bool N, bool M, size_t Vm) {
    return WideInstruction(U,
                           D,
                           sz,
                           Vn,
                           Vd,
                           N,
                           M,
                           Vm,
                           op ? ThreeRegsWidenBehaviour::Second : ThreeRegsWidenBehaviour::Both,
                           [this](size_t esize, const auto&, const auto& reg_n, const auto& reg_m) {
                               return ir.VectorSub(esize, reg_n, reg_m);
                           });
}

bool asimd_VABAL(bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool M, size_t Vm) {
    return AbsoluteDifferenceLong(
            U, D, sz, Vn, Vd, N, M, Vm, ThreeRegsAccumulateBehavior::Accumulate);
}

bool asimd_VABDL(bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool M, size_t Vm) {
    return AbsoluteDifferenceLong(U, D, sz, Vn, Vd, N, M, Vm, ThreeRegsAccumulateBehavior::None);
}

bool asimd_VMLAL(
        bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool op, bool N, bool M, size_t Vm) {
    const size_t esize = 8U << sz;

    if (sz == 0b11) {
        return DecodeError();
    }

    if (NeonBit<0>(Vd)) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(true, Vd, D);
    const auto m = ToVector(false, Vm, M);
    const auto n = ToVector(false, Vn, N);

    const auto reg_d = ir.GetVector(d);
    const auto reg_m = ir.GetVector(m);
    const auto reg_n = ir.GetVector(n);
    const auto multiply = U ? ir.VectorMultiplyUnsignedWiden(esize, reg_n, reg_m)
                            : ir.VectorMultiplySignedWiden(esize, reg_n, reg_m);
    const auto result = op ? ir.VectorSub(esize * 2, reg_d, multiply)
                           : ir.VectorAdd(esize * 2, reg_d, multiply);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VMULL(
        bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool P, bool N, bool M, size_t Vm) {
    if (sz == 0b11) {
        return DecodeError();
    }

    if ((P & (U || sz == 0b10)) || NeonBit<0>(Vd)) {
        return UndefinedInstruction();
    }

    const size_t esize = P ? (sz == 0b00 ? 8 : 64) : 8U << sz;
    const auto d = ToVector(true, Vd, D);
    const auto m = ToVector(false, Vm, M);
    const auto n = ToVector(false, Vn, N);

    const auto reg_n = ir.GetVector(n);
    const auto reg_m = ir.GetVector(m);
    const auto result = P   ? ir.VectorPolynomialMultiplyLong(esize, reg_n, reg_m)
                        : U ? ir.VectorMultiplyUnsignedWiden(esize, reg_n, reg_m)
                            : ir.VectorMultiplySignedWiden(esize, reg_n, reg_m);

    ir.SetVector(d, result);
    return true;
}

/* This file is part of the dynarmic project.
 * Copyright (c) 2020 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

std::pair<ExtReg, size_t> GetScalarLocation(size_t esize, bool M, size_t Vm) {
    const ExtReg m = ExtReg::Q0 + ((Vm >> 1) & (esize == 16 ? 0b11 : 0b111));
    const size_t index =
            concatenate(Imm<1>{NeonBit<0>(Vm)}, Imm<1>{M}, Imm<1>{NeonBit<3>(Vm)}).ZeroExtend() >>
            (esize == 16 ? 0 : 1);
    return std::make_pair(m, index);
}

enum class TwoRegsScalarMultiplyBehavior {
    Multiply,
    MultiplyAccumulate,
    MultiplySubtract,
};

enum class TwoRegsScalarRounding {
    None,
    Round,
};

bool ScalarMultiply(bool Q,
                    bool D,
                    size_t sz,
                    size_t Vn,
                    size_t Vd,
                    bool F,
                    bool N,
                    bool M,
                    size_t Vm,
                    TwoRegsScalarMultiplyBehavior multiply) {
    if (sz == 0b11) {
        return DecodeError();
    }

    if (sz == 0b00 || (F && sz == 0b01)) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn))) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto n = ToVector(Q, Vn, N);
    const auto [m, index] = GetScalarLocation(esize, M, Vm);

    const auto reg_n = ir.GetVector(n);
    const auto reg_m = ir.VectorBroadcastElement(esize, ir.GetVector(m), index);
    const auto addend =
            F ? ir.FPVectorMul(esize, reg_n, reg_m, false) : ir.VectorMultiply(esize, reg_n, reg_m);
    const auto result = [&] {
        switch (multiply) {
            case TwoRegsScalarMultiplyBehavior::Multiply:
                return addend;
            case TwoRegsScalarMultiplyBehavior::MultiplyAccumulate:
                return F ? ir.FPVectorAdd(esize, ir.GetVector(d), addend, false)
                         : ir.VectorAdd(esize, ir.GetVector(d), addend);
            case TwoRegsScalarMultiplyBehavior::MultiplySubtract:
                return F ? ir.FPVectorSub(esize, ir.GetVector(d), addend, false)
                         : ir.VectorSub(esize, ir.GetVector(d), addend);
            default:
                return Value{};
        }
    }();

    ir.SetVector(d, result);
    return true;
}

bool ScalarMultiplyLong(bool U,
                        bool D,
                        size_t sz,
                        size_t Vn,
                        size_t Vd,
                        bool N,
                        bool M,
                        size_t Vm,
                        TwoRegsScalarMultiplyBehavior multiply) {
    if (sz == 0b11) {
        return DecodeError();
    }

    if (sz == 0b00 || NeonBit<0>(Vd)) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(true, Vd, D);
    const auto n = ToVector(false, Vn, N);
    const auto [m, index] = GetScalarLocation(esize, M, Vm);

    const auto scalar = ir.VectorGetElement(esize, ir.GetVector(m), index);
    const auto reg_n = ir.GetVector(n);
    const auto reg_m = ir.VectorBroadcast(esize, scalar);
    const auto addend = U ? ir.VectorMultiplyUnsignedWiden(esize, reg_n, reg_m)
                          : ir.VectorMultiplySignedWiden(esize, reg_n, reg_m);
    const auto result = [&] {
        switch (multiply) {
            case TwoRegsScalarMultiplyBehavior::Multiply:
                return addend;
            case TwoRegsScalarMultiplyBehavior::MultiplyAccumulate:
                return ir.VectorAdd(esize * 2, ir.GetVector(d), addend);
            case TwoRegsScalarMultiplyBehavior::MultiplySubtract:
                return ir.VectorSub(esize * 2, ir.GetVector(d), addend);
            default:
                return Value{};
        }
    }();

    ir.SetVector(d, result);
    return true;
}

bool ScalarMultiplyDoublingReturnHigh(bool Q,
                                      bool D,
                                      size_t sz,
                                      size_t Vn,
                                      size_t Vd,
                                      bool N,
                                      bool M,
                                      size_t Vm,
                                      TwoRegsScalarRounding round) {
    if (sz == 0b11) {
        return DecodeError();
    }

    if (sz == 0b00) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn))) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto n = ToVector(Q, Vn, N);
    const auto [m, index] = GetScalarLocation(esize, M, Vm);

    const auto reg_n = ir.GetVector(n);
    const auto reg_m = ir.VectorBroadcastElement(esize, ir.GetVector(m), index);
    const auto result =
            round == TwoRegsScalarRounding::None
                    ? ir.VectorSignedSaturatedDoublingMultiplyHigh(esize, reg_n, reg_m)
                    : ir.VectorSignedSaturatedDoublingMultiplyHighRounding(esize, reg_n, reg_m);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VMLA_scalar(bool Q,
                       bool D,
                       size_t sz,
                       size_t Vn,
                       size_t Vd,
                       bool op,
                       bool F,
                       bool N,
                       bool M,
                       size_t Vm) {
    const auto behavior = op ? TwoRegsScalarMultiplyBehavior::MultiplySubtract
                             : TwoRegsScalarMultiplyBehavior::MultiplyAccumulate;
    return ScalarMultiply(Q, D, sz, Vn, Vd, F, N, M, Vm, behavior);
}

bool asimd_VMLAL_scalar(
        bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool op, bool N, bool M, size_t Vm) {
    const auto behavior = op ? TwoRegsScalarMultiplyBehavior::MultiplySubtract
                             : TwoRegsScalarMultiplyBehavior::MultiplyAccumulate;
    return ScalarMultiplyLong(U, D, sz, Vn, Vd, N, M, Vm, behavior);
}

bool asimd_VMUL_scalar(
        bool Q, bool D, size_t sz, size_t Vn, size_t Vd, bool F, bool N, bool M, size_t Vm) {
    return ScalarMultiply(Q, D, sz, Vn, Vd, F, N, M, Vm, TwoRegsScalarMultiplyBehavior::Multiply);
}

bool asimd_VMULL_scalar(
        bool U, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool M, size_t Vm) {
    return ScalarMultiplyLong(U, D, sz, Vn, Vd, N, M, Vm, TwoRegsScalarMultiplyBehavior::Multiply);
}

bool asimd_VQDMULL_scalar(bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool M, size_t Vm) {
    if (sz == 0b11) {
        return DecodeError();
    }

    if (sz == 0b00 || NeonBit<0>(Vd)) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(true, Vd, D);
    const auto n = ToVector(false, Vn, N);
    const auto [m, index] = GetScalarLocation(esize, M, Vm);

    const auto reg_n = ir.GetVector(n);
    const auto reg_m = ir.VectorBroadcastElement(esize, ir.GetVector(m), index);
    const auto result = ir.VectorSignedSaturatedDoublingMultiplyLong(esize, reg_n, reg_m);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VQDMULH_scalar(
        bool Q, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool M, size_t Vm) {
    return ScalarMultiplyDoublingReturnHigh(
            Q, D, sz, Vn, Vd, N, M, Vm, TwoRegsScalarRounding::None);
}

bool asimd_VQRDMULH_scalar(
        bool Q, bool D, size_t sz, size_t Vn, size_t Vd, bool N, bool M, size_t Vm) {
    return ScalarMultiplyDoublingReturnHigh(
            Q, D, sz, Vn, Vd, N, M, Vm, TwoRegsScalarRounding::Round);
}

/* This file is part of the dynarmic project.
 * Copyright (c) 2020 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

enum class TwoRegsMiscComparison {
    EQ,
    GE,
    GT,
    LE,
    LT,
};

bool CompareWithZero(bool D,
                     size_t sz,
                     size_t Vd,
                     bool F,
                     bool Q,
                     bool M,
                     size_t Vm,
                     TwoRegsMiscComparison type) {
    if (sz == 0b11 || (F && sz != 0b10)) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto result = [&] {
        const auto reg_m = ir.GetVector(m);
        const auto zero = ir.ZeroVector();

        if (F) {
            switch (type) {
                case TwoRegsMiscComparison::EQ:
                    return ir.FPVectorEqual(32, reg_m, zero, false);
                case TwoRegsMiscComparison::GE:
                    return ir.FPVectorGreaterEqual(32, reg_m, zero, false);
                case TwoRegsMiscComparison::GT:
                    return ir.FPVectorGreater(32, reg_m, zero, false);
                case TwoRegsMiscComparison::LE:
                    return ir.FPVectorGreaterEqual(32, zero, reg_m, false);
                case TwoRegsMiscComparison::LT:
                    return ir.FPVectorGreater(32, zero, reg_m, false);
            }

            return Value{};
        } else {
            static constexpr std::array fns{
                    &Semantics::VectorEqual,
                    &Semantics::VectorGreaterEqualSigned,
                    &Semantics::VectorGreaterSigned,
                    &Semantics::VectorLessEqualSigned,
                    &Semantics::VectorLessSigned,
            };

            const size_t esize = 8U << sz;
            return (ir.*fns[static_cast<size_t>(type)])(esize, reg_m, zero);
        }
    }();

    ir.SetVector(d, result);
    return true;
}

enum class TwoRegsMiscAccumulateBehavior {
    None,
    Accumulate,
};

bool PairedAddOperation(bool D,
                        size_t sz,
                        size_t Vd,
                        bool op,
                        bool Q,
                        bool M,
                        size_t Vm,
                        TwoRegsMiscAccumulateBehavior accumulate) {
    if (sz == 0b11) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);

    const auto reg_m = ir.GetVector(m);
    const auto result = [&] {
        const auto tmp = op ? ir.VectorPairedAddUnsignedWiden(esize, reg_m)
                            : ir.VectorPairedAddSignedWiden(esize, reg_m);

        if (accumulate == TwoRegsMiscAccumulateBehavior::Accumulate) {
            const auto reg_d = ir.GetVector(d);
            return ir.VectorAdd(esize * 2, reg_d, tmp);
        }

        return tmp;
    }();

    ir.SetVector(d, result);
    return true;
}

bool asimd_VREV(bool D, size_t sz, size_t Vd, size_t op, bool Q, bool M, size_t Vm) {
    if (op + sz >= 3) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto result = [this, m, op, sz] {
        const auto reg_m = ir.GetVector(m);
        const size_t esize = 8 << sz;

        switch (op) {
            case 0b00:
                return ir.VectorReverseElementsInLongGroups(esize, reg_m);
            case 0b01:
                return ir.VectorReverseElementsInWordGroups(esize, reg_m);
            case 0b10:
                return ir.VectorReverseElementsInHalfGroups(esize, reg_m);
        }

        throw Rejected{BuildError::InvalidInput};
    }();

    ir.SetVector(d, result);
    return true;
}

bool asimd_VPADDL(bool D, size_t sz, size_t Vd, bool op, bool Q, bool M, size_t Vm) {
    return PairedAddOperation(D, sz, Vd, op, Q, M, Vm, TwoRegsMiscAccumulateBehavior::None);
}

bool v8_AESD(bool D, size_t sz, size_t Vd, bool M, size_t Vm) {
    if (sz != 0b00 || NeonBit<0>(Vd) || NeonBit<0>(Vm)) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(true, Vd, D);
    const auto m = ToVector(true, Vm, M);
    const auto reg_d = ir.GetVector(d);
    const auto reg_m = ir.GetVector(m);
    const auto result = ir.AESDecryptSingleRound(ir.VectorEor(reg_d, reg_m));

    ir.SetVector(d, result);
    return true;
}

bool v8_AESE(bool D, size_t sz, size_t Vd, bool M, size_t Vm) {
    if (sz != 0b00 || NeonBit<0>(Vd) || NeonBit<0>(Vm)) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(true, Vd, D);
    const auto m = ToVector(true, Vm, M);
    const auto reg_d = ir.GetVector(d);
    const auto reg_m = ir.GetVector(m);
    const auto result = ir.AESEncryptSingleRound(ir.VectorEor(reg_d, reg_m));

    ir.SetVector(d, result);
    return true;
}

bool v8_AESIMC(bool D, size_t sz, size_t Vd, bool M, size_t Vm) {
    if (sz != 0b00 || NeonBit<0>(Vd) || NeonBit<0>(Vm)) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(true, Vd, D);
    const auto m = ToVector(true, Vm, M);
    const auto reg_m = ir.GetVector(m);
    const auto result = ir.AESInverseMixColumns(reg_m);

    ir.SetVector(d, result);
    return true;
}

bool v8_AESMC(bool D, size_t sz, size_t Vd, bool M, size_t Vm) {
    if (sz != 0b00 || NeonBit<0>(Vd) || NeonBit<0>(Vm)) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(true, Vd, D);
    const auto m = ToVector(true, Vm, M);
    const auto reg_m = ir.GetVector(m);
    const auto result = ir.AESMixColumns(reg_m);

    ir.SetVector(d, result);
    return true;
}

bool v8_SHA256SU0(bool D, size_t sz, size_t Vd, bool M, size_t Vm) {
    if (sz != 0b10 || NeonBit<0>(Vd) || NeonBit<0>(Vm)) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(true, Vd, D);
    const auto m = ToVector(true, Vm, M);
    const auto x = ir.GetVector(d);
    const auto y = ir.GetVector(m);
    const auto result = ir.SHA256MessageSchedule0(x, y);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VCLS(bool D, size_t sz, size_t Vd, bool Q, bool M, size_t Vm) {
    if (sz == 0b11) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto result = [this, m, sz] {
        const auto reg_m = ir.GetVector(m);
        const size_t esize = 8U << sz;
        const auto shifted = ir.VectorArithmeticShiftRight(esize, reg_m, static_cast<u8>(esize));
        const auto xored = ir.VectorEor(reg_m, shifted);
        const auto clz = ir.VectorCountLeadingZeros(esize, xored);
        return ir.VectorSub(esize, clz, ir.VectorBroadcast(esize, I(esize, 1)));
    }();

    ir.SetVector(d, result);
    return true;
}

bool asimd_VCLZ(bool D, size_t sz, size_t Vd, bool Q, bool M, size_t Vm) {
    if (sz == 0b11) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto result = [this, m, sz] {
        const auto reg_m = ir.GetVector(m);
        const size_t esize = 8U << sz;

        return ir.VectorCountLeadingZeros(esize, reg_m);
    }();

    ir.SetVector(d, result);
    return true;
}

bool asimd_VCNT(bool D, size_t sz, size_t Vd, bool Q, bool M, size_t Vm) {
    if (sz != 0b00) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto reg_m = ir.GetVector(m);
    const auto result = ir.VectorPopulationCount(reg_m);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VMVN_reg(bool D, size_t sz, size_t Vd, bool Q, bool M, size_t Vm) {
    if (sz != 0b00) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);

    const auto reg_m = ir.GetVector(m);
    const auto result = ir.VectorNot(reg_m);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VPADAL(bool D, size_t sz, size_t Vd, bool op, bool Q, bool M, size_t Vm) {
    return PairedAddOperation(D, sz, Vd, op, Q, M, Vm, TwoRegsMiscAccumulateBehavior::Accumulate);
}

bool asimd_VQABS(bool D, size_t sz, size_t Vd, bool Q, bool M, size_t Vm) {
    if (sz == 0b11) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);

    const auto reg_m = ir.GetVector(m);
    const auto result = ir.VectorSignedSaturatedAbs(esize, reg_m);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VQNEG(bool D, size_t sz, size_t Vd, bool Q, bool M, size_t Vm) {
    if (sz == 0b11) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);

    const auto reg_m = ir.GetVector(m);
    const auto result = ir.VectorSignedSaturatedNeg(esize, reg_m);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VCGT_zero(bool D, size_t sz, size_t Vd, bool F, bool Q, bool M, size_t Vm) {
    return CompareWithZero(D, sz, Vd, F, Q, M, Vm, TwoRegsMiscComparison::GT);
}

bool asimd_VCGE_zero(bool D, size_t sz, size_t Vd, bool F, bool Q, bool M, size_t Vm) {
    return CompareWithZero(D, sz, Vd, F, Q, M, Vm, TwoRegsMiscComparison::GE);
}

bool asimd_VCEQ_zero(bool D, size_t sz, size_t Vd, bool F, bool Q, bool M, size_t Vm) {
    return CompareWithZero(D, sz, Vd, F, Q, M, Vm, TwoRegsMiscComparison::EQ);
}

bool asimd_VCLE_zero(bool D, size_t sz, size_t Vd, bool F, bool Q, bool M, size_t Vm) {
    return CompareWithZero(D, sz, Vd, F, Q, M, Vm, TwoRegsMiscComparison::LE);
}

bool asimd_VCLT_zero(bool D, size_t sz, size_t Vd, bool F, bool Q, bool M, size_t Vm) {
    return CompareWithZero(D, sz, Vd, F, Q, M, Vm, TwoRegsMiscComparison::LT);
}

bool asimd_VABS(bool D, size_t sz, size_t Vd, bool F, bool Q, bool M, size_t Vm) {
    if (sz == 0b11 || (F && sz != 0b10)) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto result = [this, F, m, sz] {
        const auto reg_m = ir.GetVector(m);

        if (F) {
            return ir.FPVectorAbs(32, reg_m);
        }

        const size_t esize = 8U << sz;
        return ir.VectorAbs(esize, reg_m);
    }();

    ir.SetVector(d, result);
    return true;
}

bool asimd_VNEG(bool D, size_t sz, size_t Vd, bool F, bool Q, bool M, size_t Vm) {
    if (sz == 0b11 || (F && sz != 0b10)) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto result = [this, F, m, sz] {
        const auto reg_m = ir.GetVector(m);

        if (F) {
            return ir.FPVectorNeg(32, reg_m);
        }

        const size_t esize = 8U << sz;
        return ir.VectorSub(esize, ir.ZeroVector(), reg_m);
    }();

    ir.SetVector(d, result);
    return true;
}

bool asimd_VSWP(bool D, size_t Vd, bool Q, bool M, size_t Vm) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    // Swapping the same register results in the same contents.
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    if (d == m) {
        return true;
    }

    if (Q) {
        const auto reg_d = ir.GetVector(d);
        const auto reg_m = ir.GetVector(m);

        ir.SetVector(m, reg_d);
        ir.SetVector(d, reg_m);
    } else {
        const auto reg_d = ir.GetExtendedRegister(d);
        const auto reg_m = ir.GetExtendedRegister(m);

        ir.SetExtendedRegister(m, reg_d);
        ir.SetExtendedRegister(d, reg_m);
    }

    return true;
}

bool asimd_VTRN(bool D, size_t sz, size_t Vd, bool Q, bool M, size_t Vm) {
    if (sz == 0b11) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);

    if (d == m) {
        // The destination data is architecturally UNKNOWN, not an illegal
        // instruction. Retain its original bits as a deterministic choice.
        return true;
    }

    const auto reg_d = ir.GetVector(d);
    const auto reg_m = ir.GetVector(m);
    const auto result_d = ir.VectorTranspose(esize, reg_d, reg_m, false);
    const auto result_m = ir.VectorTranspose(esize, reg_d, reg_m, true);

    ir.SetVector(d, result_d);
    ir.SetVector(m, result_m);
    return true;
}

bool asimd_VUZP(bool D, size_t sz, size_t Vd, bool Q, bool M, size_t Vm) {
    if (sz == 0b11 || (!Q && sz == 0b10)) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);

    if (d == m) {
        return true;  // Architecturally UNKNOWN destination data.
    }

    const auto reg_d = ir.GetVector(d);
    const auto reg_m = ir.GetVector(m);
    auto result_d = Q ? ir.VectorDeinterleaveEven(esize, reg_d, reg_m)
                      : ir.VectorDeinterleaveEvenLower(esize, reg_d, reg_m);
    auto result_m = Q ? ir.VectorDeinterleaveOdd(esize, reg_d, reg_m)
                      : ir.VectorDeinterleaveOddLower(esize, reg_d, reg_m);

    ir.SetVector(d, result_d);
    ir.SetVector(m, result_m);
    return true;
}

bool asimd_VZIP(bool D, size_t sz, size_t Vd, bool Q, bool M, size_t Vm) {
    if (sz == 0b11 || (!Q && sz == 0b10)) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);

    if (d == m) {
        return true;  // Architecturally UNKNOWN destination data.
    }

    const auto reg_d = ir.GetVector(d);
    const auto reg_m = ir.GetVector(m);

    if (Q) {
        const auto result_d = ir.VectorInterleaveLower(esize, reg_d, reg_m);
        const auto result_m = ir.VectorInterleaveUpper(esize, reg_d, reg_m);

        ir.SetVector(d, result_d);
        ir.SetVector(m, result_m);
    } else {
        const auto result = ir.VectorInterleaveLower(esize, reg_d, reg_m);

        ir.SetExtendedRegister(d, ir.VectorGetElement(64, result, 0));
        ir.SetExtendedRegister(m, ir.VectorGetElement(64, result, 1));
    }
    return true;
}

bool asimd_VMOVN(bool D, size_t sz, size_t Vd, bool M, size_t Vm) {
    if (sz == 0b11 || NeonBit<0>(Vm)) {
        return UndefinedInstruction();
    }
    const size_t esize = 8U << sz;
    const auto d = ToVector(false, Vd, D);
    const auto m = ToVector(true, Vm, M);

    const auto reg_m = ir.GetVector(m);
    const auto result = ir.VectorNarrow(2 * esize, reg_m);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VQMOVUN(bool D, size_t sz, size_t Vd, bool M, size_t Vm) {
    if (sz == 0b11 || NeonBit<0>(Vm)) {
        return UndefinedInstruction();
    }
    const size_t esize = 8U << sz;
    const auto d = ToVector(false, Vd, D);
    const auto m = ToVector(true, Vm, M);

    const auto reg_m = ir.GetVector(m);
    const auto result = ir.VectorSignedSaturatedNarrowToUnsigned(2 * esize, reg_m);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VQMOVN(bool D, size_t sz, size_t Vd, bool op, bool M, size_t Vm) {
    if (sz == 0b11 || NeonBit<0>(Vm)) {
        return UndefinedInstruction();
    }
    const size_t esize = 8U << sz;
    const auto d = ToVector(false, Vd, D);
    const auto m = ToVector(true, Vm, M);

    const auto reg_m = ir.GetVector(m);
    const auto result = op ? ir.VectorUnsignedSaturatedNarrow(2 * esize, reg_m)
                           : ir.VectorSignedSaturatedNarrowToSigned(2 * esize, reg_m);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VSHLL_max(bool D, size_t sz, size_t Vd, bool M, size_t Vm) {
    if (sz == 0b11 || NeonBit<0>(Vd)) {
        return UndefinedInstruction();
    }
    const size_t esize = 8U << sz;
    const auto d = ToVector(true, Vd, D);
    const auto m = ToVector(false, Vm, M);

    const auto reg_m = ir.GetVector(m);
    const auto result = ir.VectorLogicalShiftLeft(
            2 * esize, ir.VectorZeroExtend(esize, reg_m), static_cast<u8>(esize));

    ir.SetVector(d, result);
    return true;
}

/* This file is part of the dynarmic project.
 * Copyright (c) 2020 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

enum class TwoRegsShiftAccumulating { None, Accumulate };

enum class TwoRegsShiftRounding {
    None,
    Round,
};

enum class TwoRegsShiftNarrowing {
    Truncation,
    SaturateToUnsigned,
    SaturateToSigned,
};

enum class TwoRegsShiftSignedness { Signed, Unsigned };

Value PerformRoundingCorrection(size_t esize, u64 round_value, Value original, Value shifted) {
    const auto round_const = ir.VectorBroadcast(esize, I(esize, round_value));
    const auto round_correction =
            ir.VectorEqual(esize, ir.VectorAnd(original, round_const), round_const);
    return ir.VectorSub(esize, shifted, round_correction);
}

std::pair<size_t, size_t> ElementSizeAndShiftAmount(bool right_shift, bool L, size_t imm6) {
    if (right_shift) {
        if (L) {
            return {64, 64 - imm6};
        }

        const size_t esize = 8U << NeonHighestBit(imm6 >> 3);
        const size_t shift_amount = (esize * 2) - imm6;
        return {esize, shift_amount};
    } else {
        if (L) {
            return {64, imm6};
        }

        const size_t esize = 8U << NeonHighestBit(imm6 >> 3);
        const size_t shift_amount = imm6 - esize;
        return {esize, shift_amount};
    }
}

bool ShiftRight(bool U,
                bool D,
                size_t imm6,
                size_t Vd,
                bool L,
                bool Q,
                bool M,
                size_t Vm,
                TwoRegsShiftAccumulating accumulate,
                TwoRegsShiftRounding rounding) {
    if (!L && NeonBits<3, 5>(imm6) == 0) {
        return DecodeError();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const auto [esize, shift_amount] = ElementSizeAndShiftAmount(true, L, imm6);
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);

    const auto reg_m = ir.GetVector(m);
    auto result = U ? ir.VectorLogicalShiftRight(esize, reg_m, static_cast<u8>(shift_amount))
                    : ir.VectorArithmeticShiftRight(esize, reg_m, static_cast<u8>(shift_amount));

    if (rounding == TwoRegsShiftRounding::Round) {
        const u64 round_value = 1ULL << (shift_amount - 1);
        result = PerformRoundingCorrection(esize, round_value, reg_m, result);
    }

    if (accumulate == TwoRegsShiftAccumulating::Accumulate) {
        const auto reg_d = ir.GetVector(d);
        result = ir.VectorAdd(esize, result, reg_d);
    }

    ir.SetVector(d, result);
    return true;
}

bool ShiftRightNarrowing(bool D,
                         size_t imm6,
                         size_t Vd,
                         bool M,
                         size_t Vm,
                         TwoRegsShiftRounding rounding,
                         TwoRegsShiftNarrowing narrowing,
                         TwoRegsShiftSignedness signedness) {
    if (NeonBits<3, 5>(imm6) == 0) {
        return DecodeError();
    }

    if (NeonBit<0>(Vm)) {
        return UndefinedInstruction();
    }

    const auto [esize, shift_amount_] = ElementSizeAndShiftAmount(true, false, imm6);
    const auto source_esize = 2 * esize;
    const auto shift_amount = static_cast<u8>(shift_amount_);

    const auto d = ToVector(false, Vd, D);
    const auto m = ToVector(true, Vm, M);

    const auto reg_m = ir.GetVector(m);
    auto wide_result = [&] {
        if (signedness == TwoRegsShiftSignedness::Signed) {
            return ir.VectorArithmeticShiftRight(source_esize, reg_m, shift_amount);
        }
        return ir.VectorLogicalShiftRight(source_esize, reg_m, shift_amount);
    }();

    if (rounding == TwoRegsShiftRounding::Round) {
        const u64 round_value = 1ULL << (shift_amount - 1);
        wide_result = PerformRoundingCorrection(source_esize, round_value, reg_m, wide_result);
    }

    const auto result = [&] {
        switch (narrowing) {
            case TwoRegsShiftNarrowing::Truncation:
                return ir.VectorNarrow(source_esize, wide_result);
            case TwoRegsShiftNarrowing::SaturateToUnsigned:
                if (signedness == TwoRegsShiftSignedness::Signed) {
                    return ir.VectorSignedSaturatedNarrowToUnsigned(source_esize, wide_result);
                }
                return ir.VectorUnsignedSaturatedNarrow(source_esize, wide_result);
            case TwoRegsShiftNarrowing::SaturateToSigned:
                // This narrowing mode is selected only for signed source lanes.
                return ir.VectorSignedSaturatedNarrowToSigned(source_esize, wide_result);
        }
        throw Rejected{BuildError::InvalidInput};
    }();

    ir.SetVector(d, result);
    return true;
}

bool asimd_SHR(bool U, bool D, size_t imm6, size_t Vd, bool L, bool Q, bool M, size_t Vm) {
    return ShiftRight(U,
                      D,
                      imm6,
                      Vd,
                      L,
                      Q,
                      M,
                      Vm,
                      TwoRegsShiftAccumulating::None,
                      TwoRegsShiftRounding::None);
}

bool asimd_SRA(bool U, bool D, size_t imm6, size_t Vd, bool L, bool Q, bool M, size_t Vm) {
    return ShiftRight(U,
                      D,
                      imm6,
                      Vd,
                      L,
                      Q,
                      M,
                      Vm,
                      TwoRegsShiftAccumulating::Accumulate,
                      TwoRegsShiftRounding::None);
}

bool asimd_VRSHR(bool U, bool D, size_t imm6, size_t Vd, bool L, bool Q, bool M, size_t Vm) {
    return ShiftRight(U,
                      D,
                      imm6,
                      Vd,
                      L,
                      Q,
                      M,
                      Vm,
                      TwoRegsShiftAccumulating::None,
                      TwoRegsShiftRounding::Round);
}

bool asimd_VRSRA(bool U, bool D, size_t imm6, size_t Vd, bool L, bool Q, bool M, size_t Vm) {
    return ShiftRight(U,
                      D,
                      imm6,
                      Vd,
                      L,
                      Q,
                      M,
                      Vm,
                      TwoRegsShiftAccumulating::Accumulate,
                      TwoRegsShiftRounding::Round);
}

bool asimd_VSRI(bool D, size_t imm6, size_t Vd, bool L, bool Q, bool M, size_t Vm) {
    if (!L && NeonBits<3, 5>(imm6) == 0) {
        return DecodeError();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const auto [esize, shift_amount] = ElementSizeAndShiftAmount(true, L, imm6);
    const u64 mask = shift_amount == esize ? 0 : NeonOnes(esize) >> shift_amount;

    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);

    const auto reg_m = ir.GetVector(m);
    const auto reg_d = ir.GetVector(d);

    const auto shifted = ir.VectorLogicalShiftRight(esize, reg_m, static_cast<u8>(shift_amount));
    const auto mask_vec = ir.VectorBroadcast(esize, I(esize, mask));
    const auto result = ir.VectorOr(ir.VectorAndNot(reg_d, mask_vec), shifted);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VSLI(bool D, size_t imm6, size_t Vd, bool L, bool Q, bool M, size_t Vm) {
    if (!L && NeonBits<3, 5>(imm6) == 0) {
        return DecodeError();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const auto [esize, shift_amount] = ElementSizeAndShiftAmount(false, L, imm6);
    const u64 mask = NeonOnes(esize) << shift_amount;

    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);

    const auto reg_m = ir.GetVector(m);
    const auto reg_d = ir.GetVector(d);

    const auto shifted = ir.VectorLogicalShiftLeft(esize, reg_m, static_cast<u8>(shift_amount));
    const auto mask_vec = ir.VectorBroadcast(esize, I(esize, mask));
    const auto result = ir.VectorOr(ir.VectorAndNot(reg_d, mask_vec), shifted);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VQSHL(
        bool U, bool D, size_t imm6, size_t Vd, bool op, bool L, bool Q, bool M, size_t Vm) {
    if (!L && NeonBits<3, 5>(imm6) == 0) {
        return DecodeError();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    if (!U && !op) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto result = [&] {
        const auto reg_m = ir.GetVector(m);
        const auto [esize, shift_amount] = ElementSizeAndShiftAmount(false, L, imm6);
        const Value shift_vec = ir.VectorBroadcast(esize, I(esize, shift_amount));

        if (U) {
            if (op) {
                return ir.VectorUnsignedSaturatedShiftLeft(esize, reg_m, shift_vec);
            }

            return ir.VectorSignedSaturatedShiftLeftUnsigned(
                    esize, reg_m, static_cast<u8>(shift_amount));
        }
        if (op) {
            return ir.VectorSignedSaturatedShiftLeft(esize, reg_m, shift_vec);
        }

        return Value{};
    }();

    ir.SetVector(d, result);
    return true;
}

bool asimd_VSHL(bool D, size_t imm6, size_t Vd, bool L, bool Q, bool M, size_t Vm) {
    if (!L && NeonBits<3, 5>(imm6) == 0) {
        return DecodeError();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const auto [esize, shift_amount] = ElementSizeAndShiftAmount(false, L, imm6);
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);

    const auto reg_m = ir.GetVector(m);
    const auto result = ir.VectorLogicalShiftLeft(esize, reg_m, static_cast<u8>(shift_amount));

    ir.SetVector(d, result);
    return true;
}

bool asimd_VSHRN(bool D, size_t imm6, size_t Vd, bool M, size_t Vm) {
    return ShiftRightNarrowing(D,
                               imm6,
                               Vd,
                               M,
                               Vm,
                               TwoRegsShiftRounding::None,
                               TwoRegsShiftNarrowing::Truncation,
                               TwoRegsShiftSignedness::Unsigned);
}

bool asimd_VRSHRN(bool D, size_t imm6, size_t Vd, bool M, size_t Vm) {
    return ShiftRightNarrowing(D,
                               imm6,
                               Vd,
                               M,
                               Vm,
                               TwoRegsShiftRounding::Round,
                               TwoRegsShiftNarrowing::Truncation,
                               TwoRegsShiftSignedness::Unsigned);
}

bool asimd_VQRSHRUN(bool D, size_t imm6, size_t Vd, bool M, size_t Vm) {
    return ShiftRightNarrowing(D,
                               imm6,
                               Vd,
                               M,
                               Vm,
                               TwoRegsShiftRounding::Round,
                               TwoRegsShiftNarrowing::SaturateToUnsigned,
                               TwoRegsShiftSignedness::Signed);
}

bool asimd_VQSHRUN(bool D, size_t imm6, size_t Vd, bool M, size_t Vm) {
    return ShiftRightNarrowing(D,
                               imm6,
                               Vd,
                               M,
                               Vm,
                               TwoRegsShiftRounding::None,
                               TwoRegsShiftNarrowing::SaturateToUnsigned,
                               TwoRegsShiftSignedness::Signed);
}

bool asimd_VQSHRN(bool U, bool D, size_t imm6, size_t Vd, bool M, size_t Vm) {
    return ShiftRightNarrowing(
            D,
            imm6,
            Vd,
            M,
            Vm,
            TwoRegsShiftRounding::None,
            U ? TwoRegsShiftNarrowing::SaturateToUnsigned : TwoRegsShiftNarrowing::SaturateToSigned,
            U ? TwoRegsShiftSignedness::Unsigned : TwoRegsShiftSignedness::Signed);
}

bool asimd_VQRSHRN(bool U, bool D, size_t imm6, size_t Vd, bool M, size_t Vm) {
    return ShiftRightNarrowing(
            D,
            imm6,
            Vd,
            M,
            Vm,
            TwoRegsShiftRounding::Round,
            U ? TwoRegsShiftNarrowing::SaturateToUnsigned : TwoRegsShiftNarrowing::SaturateToSigned,
            U ? TwoRegsShiftSignedness::Unsigned : TwoRegsShiftSignedness::Signed);
}

bool asimd_VSHLL(bool U, bool D, size_t imm6, size_t Vd, bool M, size_t Vm) {
    if (NeonBits<3, 5>(imm6) == 0) {
        return DecodeError();
    }

    if (NeonBit<0>(Vd)) {
        return UndefinedInstruction();
    }

    const auto [esize, shift_amount] = ElementSizeAndShiftAmount(false, false, imm6);

    const auto d = ToVector(true, Vd, D);
    const auto m = ToVector(false, Vm, M);

    const auto reg_m = ir.GetVector(m);
    const auto ext_vec = U ? ir.VectorZeroExtend(esize, reg_m) : ir.VectorSignExtend(esize, reg_m);
    const auto result =
            ir.VectorLogicalShiftLeft(esize * 2, ext_vec, static_cast<u8>(shift_amount));

    ir.SetVector(d, result);
    return true;
}

/* This file is part of the dynarmic project.
 * Copyright (c) 2020 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

static u64 AdvSIMDExpandImm(bool op, Imm<4> cmode, Imm<8> encoded) {
    const unsigned mode = cmode.ZeroExtend();
    const u64 imm = encoded.ZeroExtend();
    if (mode < 8) {
        const u64 word = imm << (8 * (mode / 2));
        return word | (word << 32);
    }
    if (mode < 12) {
        const u64 half = imm << (8 * ((mode / 2) & 1));
        return half | (half << 16) | (half << 32) | (half << 48);
    }
    if (mode < 14) {
        const u64 word = mode == 12 ? (imm << 8) | 255 : (imm << 16) | 65535;
        return word | (word << 32);
    }
    if (mode == 14) {
        if (!op) return imm * UINT64_C(0x0101010101010101);
        u64 result = 0;
        for (unsigned byte = 0; byte < 8; ++byte)
            if ((imm >> byte) & 1) result |= UINT64_C(255) << (byte * 8);
        return result;
    }
    const u64 word = ((imm & 128) << 24) |
                     ((imm & 64) ? UINT64_C(0x3e000000) : UINT64_C(0x40000000)) |
                     ((imm & 63) << 19);
    return word | (word << 32);
}

bool asimd_VMOV_imm(Imm<1> a,
                    bool D,
                    Imm<1> b,
                    Imm<1> c,
                    Imm<1> d,
                    size_t Vd,
                    Imm<4> cmode,
                    bool Q,
                    bool op,
                    Imm<1> e,
                    Imm<1> f,
                    Imm<1> g,
                    Imm<1> h) {
    if (Q && NeonBit<0>(Vd)) {
        return UndefinedInstruction();
    }

    const auto d_reg = ToVector(Q, Vd, D);
    const auto imm = AdvSIMDExpandImm(op, cmode, concatenate(a, b, c, d, e, f, g, h));

    // VMOV
    const auto mov = [&] {
        const auto imm64 = ir.Imm64(imm);
        if (Q) {
            ir.SetVector(d_reg, ir.VectorBroadcast(64, imm64));
        } else {
            ir.SetExtendedRegister(d_reg, imm64);
        }
        return true;
    };

    // VMVN
    // mvn is a predefined macro in arm64 MSVC
    const auto mvn_ = [&] {
        const auto imm64 = ir.Imm64(~imm);
        if (Q) {
            ir.SetVector(d_reg, ir.VectorBroadcast(64, imm64));
        } else {
            ir.SetExtendedRegister(d_reg, imm64);
        }
        return true;
    };

    // VORR
    const auto orr = [&] {
        const auto imm64 = ir.Imm64(imm);
        if (Q) {
            const auto reg_value = ir.GetVector(d_reg);
            ir.SetVector(d_reg, ir.VectorOr(reg_value, ir.VectorBroadcast(64, imm64)));
        } else {
            const auto reg_value = ir.GetExtendedRegister(d_reg);
            ir.SetExtendedRegister(d_reg, ir.Or(reg_value, imm64));
        }
        return true;
    };

    // VBIC
    const auto bic = [&] {
        const auto imm64 = ir.Imm64(~imm);
        if (Q) {
            const auto reg_value = ir.GetVector(d_reg);
            ir.SetVector(d_reg, ir.VectorAnd(reg_value, ir.VectorBroadcast(64, imm64)));
        } else {
            const auto reg_value = ir.GetExtendedRegister(d_reg);
            ir.SetExtendedRegister(d_reg, ir.And(reg_value, imm64));
        }
        return true;
    };

    switch (concatenate(cmode, Imm<1>{op}).ZeroExtend()) {
        case 0b00000:
        case 0b00100:
        case 0b01000:
        case 0b01100:
        case 0b10000:
        case 0b10100:
        case 0b11000:
        case 0b11010:
        case 0b11100:
        case 0b11101:
        case 0b11110:
            return mov();
        case 0b11111:
            return UndefinedInstruction();
        case 0b00001:
        case 0b00101:
        case 0b01001:
        case 0b01101:
        case 0b10001:
        case 0b10101:
        case 0b11001:
        case 0b11011:
            return mvn_();
        case 0b00010:
        case 0b00110:
        case 0b01010:
        case 0b01110:
        case 0b10010:
        case 0b10110:
            return orr();
        case 0b00011:
        case 0b00111:
        case 0b01011:
        case 0b01111:
        case 0b10011:
        case 0b10111:
            return bic();
    }

    return DecodeError();
}

/* This file is part of the dynarmic project.
 * Copyright (c) 2020 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

bool TableLookup(
        bool is_vtbl, bool D, size_t Vn, size_t Vd, size_t len, bool N, bool M, size_t Vm) {
    const size_t length = len + 1;
    const auto d = ToVector(false, Vd, D);
    const auto m = ToVector(false, Vm, M);
    const auto n = ToVector(false, Vn, N);

    if (RegNumber(n) + length > 32) {
        return UnpredictableInstruction();
    }

    NeonTable table;
    table.count = length;
    for (size_t i = 0; i < length; ++i) table.entries[i] = ir.GetExtendedRegister(n + i);
    const Value indicies = ir.GetExtendedRegister(m);
    const Value defaults = is_vtbl ? ir.Imm64(0) : Value{ir.GetExtendedRegister(d)};
    const Value result = ir.VectorTableLookup(defaults, table, indicies);

    ir.SetExtendedRegister(d, result);
    return true;
}

bool asimd_VEXT(bool D, size_t Vn, size_t Vd, Imm<4> imm4, bool N, bool Q, bool M, size_t Vm) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    if (!Q && imm4.Bit<3>()) {
        return UndefinedInstruction();
    }

    const u8 position = 8 * imm4.ZeroExtend<u8>();
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const auto reg_n = ir.GetVector(n);
    const auto reg_m = ir.GetVector(m);
    const auto result = Q ? ir.VectorExtract(reg_n, reg_m, position)
                          : ir.VectorExtractLower(reg_n, reg_m, position);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VTBL(bool D, size_t Vn, size_t Vd, size_t len, bool N, bool M, size_t Vm) {
    return TableLookup(true, D, Vn, Vd, len, N, M, Vm);
}

bool asimd_VTBX(bool D, size_t Vn, size_t Vd, size_t len, bool N, bool M, size_t Vm) {
    return TableLookup(false, D, Vn, Vd, len, N, M, Vm);
}

bool asimd_VDUP_scalar(bool D, Imm<4> imm4, size_t Vd, bool Q, bool M, size_t Vm) {
    if (Q && NeonBit<0>(Vd)) {
        return UndefinedInstruction();
    }

    if (imm4.Bits<0, 2>() == 0b000) {
        return UndefinedInstruction();
    }

    const size_t imm4_lsb = NeonLowestBit(imm4.ZeroExtend());
    const size_t esize = 8u << imm4_lsb;
    const size_t index = imm4.ZeroExtend() >> (imm4_lsb + 1);
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(false, Vm, M);

    const auto reg_m = ir.GetVector(m);
    const auto result = ir.VectorBroadcastElement(esize, reg_m, index);

    ir.SetVector(d, result);
    return true;
}
