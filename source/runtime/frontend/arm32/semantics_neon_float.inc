// Floating-point semantics use the same explicit numerical leaves in IR and
// direct modes. NEON uses StandardFPSCR (round-nearest, DN, FZ), while scalar
// VFP observes the mode carried by the block's complete location key.
Value GetFpscr() {
    return Emit<OpCode::LoadUniform, Type::U32>(Uniform{layout.fpscr_offset, Type::U32});
}

void SetFpscr(Value value) {
    // Trap enables and unsupported vector controls are validated at the state
    // service boundary before another instruction executes.
    Emit<OpCode::StoreUniform, Type::Void>(Uniform{layout.fpscr_offset, Type::U32},
                                           value.WithView(Type::U32));
}

Value GetFpscrNZCV() { return And(GetFpscr(), Imm32(0xf0000000)); }

void SetFpscrNZCV(Value nzcv) {
    SetFpscr(Or(And(GetFpscr(), Imm32(0x0fffffff)),
                And(nzcv.WithView(Type::U32), Imm32(0xf0000000))));
}

bool CanEmitX64FP() const noexcept {
    if constexpr (Sink::IsDirect) {
        return false;
    } else {
        const u32 fpcr = current_location.FPSCR().Value() & 0x07c00000u;
        return target.mode == TargetMode::Known && target.target == 0x8664 &&
               !current_location.FPSCR().AHP() && runtime::arm64::IsSupportedFPCR(fpcr);
    }
}

u32 CurrentX64ImmediateFPCR(bool standard = false) const noexcept {
    constexpr u32 kImmediateTag = 0x80000000u;
    const u32 fpscr = current_location.FPSCR().Value();
    const u32 fpcr = standard ? ((fpscr & (1u << 26)) | (3u << 24)) : (fpscr & 0x07c00000u);
    return kImmediateTag | fpcr;
}

u64 CurrentX64FPOffsets(bool standard = false) const noexcept {
    return u64{CurrentX64ImmediateFPCR(standard)} | (u64{layout.fpscr_offset} << 32);
}

template <Numeric operation> Value ScalarFPBinary(Value a, Value b) {
    const unsigned bits = ScalarWidth(a);
    if constexpr (operation == Numeric::Add || operation == Numeric::Sub ||
                  operation == Numeric::Mul || operation == Numeric::Div) {
        if (CanEmitX64FP() && (bits == 32 || bits == 64)) {
            constexpr u32 kind = operation == Numeric::Add   ? 0u
                                 : operation == Numeric::Sub ? 1u
                                 : operation == Numeric::Mul ? 2u
                                                             : 3u;
            const u32 mode = runtime::arm64::ArmFPBinaryMode(kind, bits, 1);
            auto res = Emit<OpCode::ArmFPBinary, Type::V128>(ZeroExtendToQuad(a),
                                                             ZeroExtendToQuad(b),
                                                             u64(CurrentX64ImmediateFPCR()),
                                                             u64(layout.fpscr_offset),
                                                             u64(mode));
            return VecExtract64(res, Literal{0u}).WithView(a.ViewType());
        }
    }
    return NumericScalar<operation>(bits, a, b).WithView(a.ViewType());
}

#define ARM32_FP_BINARY(name, operation)                                                           \
    Value name(Value a, Value b) { return ScalarFPBinary<Numeric::operation>(a, b); }
ARM32_FP_BINARY(FPAdd, Add)
ARM32_FP_BINARY(FPSub, Sub)
ARM32_FP_BINARY(FPMul, Mul)
ARM32_FP_BINARY(FPDiv, Div)
ARM32_FP_BINARY(FPMaxNumeric, MaxNumeric)
ARM32_FP_BINARY(FPMinNumeric, MinNumeric)
#undef ARM32_FP_BINARY

Value FPAbs(Value a) {
    const u64 sign = u64{1} << (ScalarWidth(a) - 1);
    return And(a, ScalarWidth(a) == 64 ? Imm64(~sign) : Imm32(u32(~sign))).WithView(a.ViewType());
}

Value FPNeg(Value a) {
    const u64 sign = u64{1} << (ScalarWidth(a) - 1);
    return Eor(a, ScalarWidth(a) == 64 ? Imm64(sign) : Imm32(u32(sign))).WithView(a.ViewType());
}

Value FPSqrt(Value a) {
    const unsigned bits = ScalarWidth(a);
    if (CanEmitX64FP() && (bits == 32 || bits == 64)) {
        const u64 encoded = (u64(runtime::arm64::Numeric::Scalar) << 32) |
                            (bits == 64 ? 0x1e61c000u : 0x1e21c000u);
        auto res = Emit<OpCode::ArmFPUnary, Type::V128>(
                ZeroExtendToQuad(a), CurrentX64FPOffsets(), encoded);
        return VecExtract64(res, Literal{0u}).WithView(a.ViewType());
    }
    return NumericScalar<Numeric::Sqrt>(bits, a, Imm64(0)).WithView(a.ViewType());
}

Value FPCompare(Value a, Value b, bool signal) {
    return (signal ? NumericScalar<Numeric::CompareFlagsSignal>(ScalarWidth(a), a, b)
                   : NumericScalar<Numeric::CompareFlags>(ScalarWidth(a), a, b))
            .WithView(Type::U32);
}

Value FPRoundInt(Value a, RoundingMode rounding, bool exact) {
    return NumericScalar<Numeric::Round>(
                   ScalarWidth(a),
                   a,
                   Imm64(runtime::arm32::NumericConvert(ScalarWidth(a), rounding, 0, exact)))
            .WithView(a.ViewType());
}

Value FPPrecision(Value a, unsigned destination, RoundingMode rounding) {
    const unsigned src = ScalarWidth(a);
    if (CanEmitX64FP() && ((src == 32 && destination == 64) || (src == 64 && destination == 32)) &&
        static_cast<unsigned>(rounding) == current_location.FPSCR().RMode()) {
        const u64 encoded = (u64(runtime::arm64::Numeric::Scalar) << 32) |
                            (src == 32 ? 0x1e22c000u : 0x1e624000u);
        auto res = Emit<OpCode::ArmFPUnary, Type::V128>(
                ZeroExtendToQuad(a), CurrentX64FPOffsets(), encoded);
        return VecExtract64(res, Literal{0u}).WithView(NeonIntegerType(destination));
    }
    return NumericScalar<Numeric::Precision>(
                   src, a, Imm64(runtime::arm32::NumericConvert(destination, rounding)))
            .WithView(NeonIntegerType(destination));
}

Value FPHalfToSingle(Value a, RoundingMode r) { return FPPrecision(a.WithView(Type::U16), 32, r); }

Value FPHalfToDouble(Value a, RoundingMode r) { return FPPrecision(a.WithView(Type::U16), 64, r); }

Value FPSingleToHalf(Value a, RoundingMode r) { return FPPrecision(a.WithView(Type::U32), 16, r); }

Value FPDoubleToHalf(Value a, RoundingMode r) { return FPPrecision(a.WithView(Type::U64), 16, r); }

Value FPSingleToDouble(Value a, RoundingMode r) {
    return FPPrecision(a.WithView(Type::U32), 64, r);
}

Value FPDoubleToSingle(Value a, RoundingMode r) {
    return FPPrecision(a.WithView(Type::U64), 32, r);
}

template <bool unsign>
Value FPFromInteger(Value a, unsigned destination, size_t fbits, RoundingMode rounding) {
    const unsigned int_bits = ScalarWidth(a);
    if (CanEmitX64FP() && (destination == 32 || destination == 64) && int_bits <= 32 &&
        fbits <= 32 && static_cast<unsigned>(rounding) == current_location.FPSCR().RMode()) {
        const auto input = int_bits == 16 ? (unsign ? ZeroExtendHalfToWord(a)
                                                    : SignExtendHalfToWord(a))
                                          : a;
        const u32 type_bit = destination == 64 ? (1u << 22) : 0u;
        const u32 word = fbits == 0 ? (0x1e220000u | (u32(unsign) << 16) | type_bit)
                                    : (0x1e020000u | (u32(unsign) << 16) | type_bit |
                                       (u32(64 - fbits) << 10));
        const u64 encoded = (u64(runtime::arm64::Numeric::Scalar) << 32) | word;
        auto res = Emit<OpCode::ArmFPConvert, Type::V128>(
                input.WithView(Type::U64), CurrentX64FPOffsets(), encoded);
        return VecExtract64(res, Literal{0u}).WithView(NeonIntegerType(destination));
    }
    constexpr auto operation = unsign ? Numeric::FromUnsigned : Numeric::FromSigned;
    return NumericScalar<operation>(
                   destination, a, Imm64(runtime::arm32::NumericConvert(int_bits, rounding, fbits)))
            .WithView(NeonIntegerType(destination));
}

Value FPSignedFixedToSingle(Value a, size_t f, RoundingMode r) {
    return FPFromInteger<false>(a, 32, f, r);
}

Value FPUnsignedFixedToSingle(Value a, size_t f, RoundingMode r) {
    return FPFromInteger<true>(a, 32, f, r);
}

Value FPSignedFixedToDouble(Value a, size_t f, RoundingMode r) {
    return FPFromInteger<false>(a, 64, f, r);
}

Value FPUnsignedFixedToDouble(Value a, size_t f, RoundingMode r) {
    return FPFromInteger<true>(a, 64, f, r);
}

template <bool unsign>
Value FPToInteger(Value a, unsigned destination, size_t fbits, RoundingMode rounding) {
    const unsigned src = ScalarWidth(a);
    if (CanEmitX64FP() && (src == 32 || src == 64) && destination == 32 && fbits <= 32 &&
        rounding == RoundingMode::TowardsZero) {
        const u32 type_bit = src == 64 ? (1u << 22) : 0u;
        const u32 word = fbits == 0 ? (0x1e380000u | (u32(unsign) << 16) | type_bit)
                                    : (0x1e180000u | (u32(unsign) << 16) | type_bit |
                                       (u32(64 - fbits) << 10));
        const u64 encoded = (u64(runtime::arm64::Numeric::Scalar) << 32) | word;
        return Emit<OpCode::ArmFPConvert, Type::U64>(
                       ZeroExtendToQuad(a), CurrentX64FPOffsets(), encoded)
                .WithView(Type::U32);
    }
    constexpr auto operation = unsign ? Numeric::ToUnsigned : Numeric::ToSigned;
    return NumericScalar<operation>(
                   src, a, Imm64(runtime::arm32::NumericConvert(destination, rounding, fbits)))
            .WithView(NeonIntegerType(destination));
}

Value FPToFixedS16(Value a, size_t f, RoundingMode r) { return FPToInteger<false>(a, 16, f, r); }

Value FPToFixedU16(Value a, size_t f, RoundingMode r) { return FPToInteger<true>(a, 16, f, r); }

Value FPToFixedS32(Value a, size_t f, RoundingMode r) { return FPToInteger<false>(a, 32, f, r); }

Value FPToFixedU32(Value a, size_t f, RoundingMode r) { return FPToInteger<true>(a, 32, f, r); }

template <Numeric operation, bool standard = false> Value FusedFP(Value addend, Value a, Value b) {
    const unsigned bits = ScalarWidth(a);
    if constexpr (operation == Numeric::Fused || operation == Numeric::FusedSubtract) {
        if (CanEmitX64FP() && (bits == 32 || bits == 64)) {
            const u32 word = (bits == 64 ? 0x1f400000u : 0x1f000000u) |
                             (operation == Numeric::FusedSubtract ? 0x8000u : 0u);
            const u64 encoded = (u64(runtime::arm64::Numeric::Scalar) << 32) | word;
            auto res = Emit<OpCode::ArmFPTernary, Type::V128>(ZeroExtendToQuad(a),
                                                              ZeroExtendToQuad(b),
                                                              ZeroExtendToQuad(addend),
                                                              CurrentX64FPOffsets(standard),
                                                              encoded);
            return VecExtract64(res, Literal{0u}).WithView(a.ViewType());
        }
    }
    Emit<OpCode::StoreUniform, Type::Void>(
            Uniform{u32(layout.gprs_offset + offsetof(GuestState, numeric_addend)), Type::U64},
            addend.WithView(Type::U64));
    return NumericScalar<operation, standard>(bits, a, b).WithView(a.ViewType());
}

Value FPMulAdd(Value addend, Value a, Value b) { return FusedFP<Numeric::Fused>(addend, a, b); }

Value FPMulSub(Value addend, Value a, Value b) {
    return FusedFP<Numeric::FusedSubtract>(addend, a, b);
}

template <Numeric operation>
Value FPVectorArith(size_t bits, Value a, Value b, bool controlled) {
    if (CanEmitX64FP() && bits == 32) {
        constexpr u32 kind = operation == Numeric::Add ? 0u : operation == Numeric::Sub ? 1u : 2u;
        const u32 lanes = NeonWidth(a) / 32;
        const u32 mode = runtime::arm64::ArmFPBinaryMode(kind, 32, lanes);
        return Emit<OpCode::ArmFPBinary, Type::V128>(a.WithView(Type::V128),
                                                     b.WithView(Type::V128),
                                                     u64(CurrentX64ImmediateFPCR(!controlled)),
                                                     u64(layout.fpscr_offset),
                                                     u64(mode))
                .WithView(a.ViewType());
    }
    return controlled ? NumericVector<operation>(bits, a, b)
                      : NumericVector<operation, true>(bits, a, b);
}

Value FPVectorAdd(size_t bits, Value a, Value b, bool controlled = true) {
    return FPVectorArith<Numeric::Add>(bits, a, b, controlled);
}

Value FPVectorSub(size_t bits, Value a, Value b, bool controlled = true) {
    return FPVectorArith<Numeric::Sub>(bits, a, b, controlled);
}

Value FPVectorMul(size_t bits, Value a, Value b, bool controlled = true) {
    return FPVectorArith<Numeric::Mul>(bits, a, b, controlled);
}

#define ARM32_FP_VECTOR_BINARY(name, operation)                                                    \
    Value name(size_t bits, Value a, Value b, bool controlled = true) {                            \
        return controlled ? NumericVector<Numeric::operation>(bits, a, b)                          \
                          : NumericVector<Numeric::operation, true>(bits, a, b);                   \
    }
ARM32_FP_VECTOR_BINARY(FPVectorEqual, CompareEqual)
ARM32_FP_VECTOR_BINARY(FPVectorGreater, CompareGreater)
ARM32_FP_VECTOR_BINARY(FPVectorGreaterEqual, CompareGreaterEqual)
ARM32_FP_VECTOR_BINARY(FPVectorMax, Max)
ARM32_FP_VECTOR_BINARY(FPVectorMin, Min)
ARM32_FP_VECTOR_BINARY(FPVectorMaxNumeric, MaxNumeric)
ARM32_FP_VECTOR_BINARY(FPVectorMinNumeric, MinNumeric)
ARM32_FP_VECTOR_BINARY(FPVectorRecipStepFused, ReciprocalStep)
ARM32_FP_VECTOR_BINARY(FPVectorRSqrtStepFused, RSqrtStep)
#undef ARM32_FP_VECTOR_BINARY

Value FPVectorAbs(size_t bits, Value a) {
    return VectorAnd(a, VectorBroadcast(bits, Imm64(~(u64{1} << (bits - 1)))));
}

Value FPVectorNeg(size_t bits, Value a) {
    return VectorEor(a, VectorBroadcast(bits, Imm64(u64{1} << (bits - 1))));
}

Value FPVectorMulAdd(size_t bits, Value addend, Value a, Value b, bool controlled = true) {
    if (CanEmitX64FP() && bits == 32) {
        const u32 q = NeonWidth(a) == 128 ? 0x40000000u : 0u;
        const u64 encoded = (u64(runtime::arm64::Numeric::Fused) << 32) | q | 0x0e20cc00u;
        return Emit<OpCode::ArmFPTernary, Type::V128>(a.WithView(Type::V128),
                                                      b.WithView(Type::V128),
                                                      addend.WithView(Type::V128),
                                                      CurrentX64FPOffsets(!controlled),
                                                      encoded)
                .WithView(a.ViewType());
    }
    auto result = ZeroVector().WithView(a.ViewType());
    for (size_t lane = 0; lane < NeonWidth(a) / bits; ++lane) {
        auto acc = VectorGetElement(bits, addend, lane), x = VectorGetElement(bits, a, lane),
             y = VectorGetElement(bits, b, lane);
        auto value = controlled ? FusedFP<Numeric::Fused>(acc, x, y)
                                : FusedFP<Numeric::Fused, true>(acc, x, y);
        result = VectorSetElement(bits, result, lane, value);
    }
    return result;
}

Value FPVectorPairedAddLower(size_t bits, Value a, Value b, bool controlled = true) {
    return FPVectorAdd(bits,
                       VectorDeinterleaveEvenLower(bits, a, b),
                       VectorDeinterleaveOddLower(bits, a, b),
                       controlled);
}

#define ARM32_FP_VECTOR_UNARY(name, operation)                                                     \
    Value name(size_t bits, Value a, bool controlled = true) {                                     \
        return controlled ? NumericVector<Numeric::operation>(bits, a, ZeroVector())               \
                          : NumericVector<Numeric::operation, true>(bits, a, ZeroVector());        \
    }
ARM32_FP_VECTOR_UNARY(FPVectorRecipEstimate, ReciprocalEstimate)
ARM32_FP_VECTOR_UNARY(FPVectorRSqrtEstimate, RSqrtEstimate)
#undef ARM32_FP_VECTOR_UNARY

template <Numeric operation> Value FPVectorConvert(size_t bits,
                                                   Value a,
                                                   size_t destination,
                                                   size_t fbits,
                                                   RoundingMode rounding,
                                                   bool controlled,
                                                   bool exact = false) {
    if constexpr (operation == Numeric::FromSigned || operation == Numeric::FromUnsigned ||
                  operation == Numeric::ToSigned || operation == Numeric::ToUnsigned) {
        const bool from_int =
                operation == Numeric::FromSigned || operation == Numeric::FromUnsigned;
        const bool unsign =
                operation == Numeric::FromUnsigned || operation == Numeric::ToUnsigned;
        const unsigned expected_rmode =
                controlled ? current_location.FPSCR().RMode()
                           : static_cast<unsigned>(RoundingMode::ToNearest_TieEven);
        const bool mode_ok = from_int ? (static_cast<unsigned>(rounding) == expected_rmode)
                                      : (rounding == RoundingMode::TowardsZero);
        if (CanEmitX64FP() && !exact && bits == 32 && destination == 32 && fbits <= 32 && mode_ok) {
            const u32 q = NeonWidth(a) == 128 ? 0x40000000u : 0u;
            const u32 u = unsign ? 0x20000000u : 0u;
            const auto family = fbits == 0 ? runtime::arm64::Numeric::Integer
                                           : runtime::arm64::Numeric::Fixed;
            const u32 word = fbits == 0
                                     ? (q | u | (from_int ? 0x0e21d800u : 0x0ea1b800u))
                                     : (q | u | (from_int ? 0x0f00e400u : 0x0f00fc00u) |
                                        (u32(64 - fbits) << 16));
            const u64 encoded = (u64(family) << 32) | word;
            return Emit<OpCode::ArmFPConvert, Type::V128>(
                           a.WithView(Type::V128), CurrentX64FPOffsets(!controlled), encoded)
                    .WithView(a.ViewType());
        }
    }
    const auto descriptor =
            Imm64(runtime::arm32::NumericConvert(destination, rounding, fbits, exact));
    auto result = ZeroVector().WithView(a.ViewType());
    const size_t count = NeonWidth(a) / bits;
    for (size_t lane = 0; lane < count; ++lane) {
        auto input = VectorGetElement(bits, a, lane);
        auto value = controlled ? NumericScalar<operation>(bits, input, descriptor)
                                : NumericScalar<operation, true>(bits, input, descriptor);
        result = VectorSetElement(destination, result, lane, value);
    }
    return result;
}

Value FPVectorRoundInt(size_t bits, Value a, RoundingMode r, bool exact, bool controlled = true) {
    return FPVectorConvert<Numeric::Round>(bits, a, bits, 0, r, controlled, exact);
}

Value FPVectorToSignedFixed(
        size_t bits, Value a, size_t f, RoundingMode r, bool controlled = true) {
    return FPVectorConvert<Numeric::ToSigned>(bits, a, bits, f, r, controlled);
}

Value FPVectorToUnsignedFixed(
        size_t bits, Value a, size_t f, RoundingMode r, bool controlled = true) {
    return FPVectorConvert<Numeric::ToUnsigned>(bits, a, bits, f, r, controlled);
}

Value FPVectorFromSignedFixed(
        size_t bits, Value a, size_t f, RoundingMode r, bool controlled = true) {
    return FPVectorConvert<Numeric::FromSigned>(bits, a, bits, f, r, controlled);
}

Value FPVectorFromUnsignedFixed(
        size_t bits, Value a, size_t f, RoundingMode r, bool controlled = true) {
    return FPVectorConvert<Numeric::FromUnsigned>(bits, a, bits, f, r, controlled);
}

Value FPVectorToHalf(size_t bits, Value a, RoundingMode r, bool controlled = true) {
    return FPVectorConvert<Numeric::Precision>(bits, a, 16, 0, r, controlled).WithView(Type::V64);
}

Value FPVectorFromHalf(size_t bits, Value a, RoundingMode r, bool controlled = true) {
    // Four H lanes become four S lanes; the source's D view remains an input
    // width, while the destination is a full Q value.
    auto descriptor = Imm64(runtime::arm32::NumericConvert(bits, r));
    auto result = ZeroVector();
    for (size_t lane = 0; lane < 128 / bits; ++lane) {
        auto input = VectorGetElement(16, a, lane);
        auto value = controlled ? NumericScalar<Numeric::Precision>(16, input, descriptor)
                                : NumericScalar<Numeric::Precision, true>(16, input, descriptor);
        result = VectorSetElement(bits, result, lane, value);
    }
    return result;
}

/* NEON instruction handlers adapted from dynarmic A32 frontend.
 * Copyright (c) 2020 MerryMage
 * SPDX-License-Identifier: 0BSD
 */
template <typename Callable> bool FloatingPointInstruction(
        bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm, Callable fn) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    if (sz == 0b1) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const auto reg_d = ir.GetVector(d);
    const auto reg_n = ir.GetVector(n);
    const auto reg_m = ir.GetVector(m);
    const auto result = fn(reg_d, reg_n, reg_m);

    ir.SetVector(d, result);
    return true;
}

bool FloatComparison(bool D,
                     bool sz,
                     size_t Vn,
                     size_t Vd,
                     bool N,
                     bool Q,
                     bool M,
                     size_t Vm,
                     ThreeRegsComparison comparison) {
    if (sz) {
        return UndefinedInstruction();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vn) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto n = ToVector(Q, Vn, N);

    const auto reg_n = ir.GetVector(n);
    const auto reg_m = ir.GetVector(m);
    const auto result = [&] {
        switch (comparison) {
            case ThreeRegsComparison::GE:
                return ir.FPVectorGreaterEqual(32, reg_n, reg_m, false);
            case ThreeRegsComparison::GT:
                return ir.FPVectorGreater(32, reg_n, reg_m, false);
            case ThreeRegsComparison::EQ:
                return ir.FPVectorEqual(32, reg_n, reg_m, false);
            case ThreeRegsComparison::AbsoluteGE:
                return ir.FPVectorGreaterEqual(
                        32, ir.FPVectorAbs(32, reg_n), ir.FPVectorAbs(32, reg_m), false);
            case ThreeRegsComparison::AbsoluteGT:
                return ir.FPVectorGreater(
                        32, ir.FPVectorAbs(32, reg_n), ir.FPVectorAbs(32, reg_m), false);
            default:
                return Value{};
        }
    }();

    ir.SetVector(d, result);
    return true;
}

bool v8_VMAXNM(bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return FloatingPointInstruction(
            D, sz, Vn, Vd, N, Q, M, Vm, [this](const auto&, const auto& reg_n, const auto& reg_m) {
                return ir.FPVectorMaxNumeric(32, reg_n, reg_m, false);
            });
}

bool v8_VMINNM(bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return FloatingPointInstruction(
            D, sz, Vn, Vd, N, Q, M, Vm, [this](const auto&, const auto& reg_n, const auto& reg_m) {
                return ir.FPVectorMinNumeric(32, reg_n, reg_m, false);
            });
}

bool asimd_VFMA(bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return FloatingPointInstruction(
            D,
            sz,
            Vn,
            Vd,
            N,
            Q,
            M,
            Vm,
            [this](const auto& reg_d, const auto& reg_n, const auto& reg_m) {
                return ir.FPVectorMulAdd(32, reg_d, reg_n, reg_m, false);
            });
}

bool asimd_VFMS(bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return FloatingPointInstruction(
            D,
            sz,
            Vn,
            Vd,
            N,
            Q,
            M,
            Vm,
            [this](const auto& reg_d, const auto& reg_n, const auto& reg_m) {
                return ir.FPVectorMulAdd(32, reg_d, ir.FPVectorNeg(32, reg_n), reg_m, false);
            });
}

bool asimd_VADD_float(bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return FloatingPointInstruction(
            D, sz, Vn, Vd, N, Q, M, Vm, [this](const auto&, const auto& reg_n, const auto& reg_m) {
                return ir.FPVectorAdd(32, reg_n, reg_m, false);
            });
}

bool asimd_VSUB_float(bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return FloatingPointInstruction(
            D, sz, Vn, Vd, N, Q, M, Vm, [this](const auto&, const auto& reg_n, const auto& reg_m) {
                return ir.FPVectorSub(32, reg_n, reg_m, false);
            });
}

bool asimd_VPADD_float(bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (Q) {
        return UndefinedInstruction();
    }
    return FloatingPointInstruction(
            D, sz, Vn, Vd, N, Q, M, Vm, [this](const auto&, const auto& reg_n, const auto& reg_m) {
                return ir.FPVectorPairedAddLower(32, reg_n, reg_m, false);
            });
}

bool asimd_VABD_float(bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return FloatingPointInstruction(
            D, sz, Vn, Vd, N, Q, M, Vm, [this](const auto&, const auto& reg_n, const auto& reg_m) {
                return ir.FPVectorAbs(32, ir.FPVectorSub(32, reg_n, reg_m, false));
            });
}

bool asimd_VMLA_float(bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return FloatingPointInstruction(
            D,
            sz,
            Vn,
            Vd,
            N,
            Q,
            M,
            Vm,
            [this](const auto& reg_d, const auto& reg_n, const auto& reg_m) {
                const auto product = ir.FPVectorMul(32, reg_n, reg_m, false);
                return ir.FPVectorAdd(32, reg_d, product, false);
            });
}

bool asimd_VMLS_float(bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return FloatingPointInstruction(
            D,
            sz,
            Vn,
            Vd,
            N,
            Q,
            M,
            Vm,
            [this](const auto& reg_d, const auto& reg_n, const auto& reg_m) {
                const auto product = ir.FPVectorMul(32, reg_n, reg_m, false);
                return ir.FPVectorAdd(32, reg_d, ir.FPVectorNeg(32, product), false);
            });
}

bool asimd_VMUL_float(bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return FloatingPointInstruction(
            D, sz, Vn, Vd, N, Q, M, Vm, [this](const auto&, const auto& reg_n, const auto& reg_m) {
                return ir.FPVectorMul(32, reg_n, reg_m, false);
            });
}

bool asimd_VCEQ_reg_float(
        bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return FloatComparison(D, sz, Vn, Vd, N, Q, M, Vm, ThreeRegsComparison::EQ);
}

bool asimd_VCGE_reg_float(
        bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return FloatComparison(D, sz, Vn, Vd, N, Q, M, Vm, ThreeRegsComparison::GE);
}

bool asimd_VCGT_reg_float(
        bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return FloatComparison(D, sz, Vn, Vd, N, Q, M, Vm, ThreeRegsComparison::GT);
}

bool asimd_VMAX_float(bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return FloatingPointInstruction(
            D, sz, Vn, Vd, N, Q, M, Vm, [this](const auto&, const auto& reg_n, const auto& reg_m) {
                return ir.FPVectorMax(32, reg_n, reg_m, false);
            });
}

bool asimd_VMIN_float(bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return FloatingPointInstruction(
            D, sz, Vn, Vd, N, Q, M, Vm, [this](const auto&, const auto& reg_n, const auto& reg_m) {
                return ir.FPVectorMin(32, reg_n, reg_m, false);
            });
}

bool asimd_VPMAX_float(bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (Q) {
        return UndefinedInstruction();
    }
    return FloatingPointInstruction(
            D, sz, Vn, Vd, N, Q, M, Vm, [this](const auto&, const auto& reg_n, const auto& reg_m) {
                const auto bottom = ir.VectorDeinterleaveEvenLower(32, reg_n, reg_m);
                const auto top = ir.VectorDeinterleaveOddLower(32, reg_n, reg_m);
                return ir.FPVectorMax(32, bottom, top, false);
            });
}

bool asimd_VPMIN_float(bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    if (Q) {
        return UndefinedInstruction();
    }
    return FloatingPointInstruction(
            D, sz, Vn, Vd, N, Q, M, Vm, [this](const auto&, const auto& reg_n, const auto& reg_m) {
                const auto bottom = ir.VectorDeinterleaveEvenLower(32, reg_n, reg_m);
                const auto top = ir.VectorDeinterleaveOddLower(32, reg_n, reg_m);
                return ir.FPVectorMin(32, bottom, top, false);
            });
}

bool asimd_VRECPS(bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return FloatingPointInstruction(
            D, sz, Vn, Vd, N, Q, M, Vm, [this](const auto&, const auto& reg_n, const auto& reg_m) {
                return ir.FPVectorRecipStepFused(32, reg_n, reg_m, false);
            });
}

bool asimd_VRSQRTS(bool D, bool sz, size_t Vn, size_t Vd, bool N, bool Q, bool M, size_t Vm) {
    return FloatingPointInstruction(
            D, sz, Vn, Vd, N, Q, M, Vm, [this](const auto&, const auto& reg_n, const auto& reg_m) {
                return ir.FPVectorRSqrtStepFused(32, reg_n, reg_m, false);
            });
}

bool RoundFloatToInteger(bool D,
                         size_t sz,
                         size_t Vd,
                         bool Q,
                         bool M,
                         size_t Vm,
                         bool exact,
                         RoundingMode rounding_mode) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    if (sz != 0b10) {
        return UndefinedInstruction();  // TODO: FP16
    }

    const size_t esize = 8 << sz;

    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);

    const auto reg_m = ir.GetVector(m);
    const auto result = ir.FPVectorRoundInt(esize, reg_m, rounding_mode, exact, false);

    ir.SetVector(d, result);
    return true;
}

bool ConvertFloatToInteger(bool D,
                           size_t sz,
                           size_t Vd,
                           bool op,
                           bool Q,
                           bool M,
                           size_t Vm,
                           RoundingMode rounding_mode) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    if (sz != 0b10) {
        return UndefinedInstruction();  // TODO: FP16
    }

    const bool unsigned_ = op;
    const size_t esize = 8 << sz;

    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);

    const auto reg_m = ir.GetVector(m);
    const auto result = unsigned_
                                ? ir.FPVectorToUnsignedFixed(esize, reg_m, 0, rounding_mode, false)
                                : ir.FPVectorToSignedFixed(esize, reg_m, 0, rounding_mode, false);

    ir.SetVector(d, result);
    return true;
}

bool v8_VRINTN(bool D, size_t sz, size_t Vd, bool Q, bool M, size_t Vm) {
    return RoundFloatToInteger(D, sz, Vd, Q, M, Vm, false, RoundingMode::ToNearest_TieEven);
}

bool v8_VRINTX(bool D, size_t sz, size_t Vd, bool Q, bool M, size_t Vm) {
    return RoundFloatToInteger(D, sz, Vd, Q, M, Vm, true, RoundingMode::ToNearest_TieEven);
}

bool v8_VRINTA(bool D, size_t sz, size_t Vd, bool Q, bool M, size_t Vm) {
    return RoundFloatToInteger(D, sz, Vd, Q, M, Vm, false, RoundingMode::ToNearest_TieAwayFromZero);
}

bool v8_VRINTZ(bool D, size_t sz, size_t Vd, bool Q, bool M, size_t Vm) {
    return RoundFloatToInteger(D, sz, Vd, Q, M, Vm, false, RoundingMode::TowardsZero);
}

bool v8_VRINTM(bool D, size_t sz, size_t Vd, bool Q, bool M, size_t Vm) {
    return RoundFloatToInteger(D, sz, Vd, Q, M, Vm, false, RoundingMode::TowardsMinusInfinity);
}

bool v8_VRINTP(bool D, size_t sz, size_t Vd, bool Q, bool M, size_t Vm) {
    return RoundFloatToInteger(D, sz, Vd, Q, M, Vm, false, RoundingMode::TowardsPlusInfinity);
}

bool asimd_VCVT_half(bool D, size_t sz, size_t Vd, bool half_to_single, bool M, size_t Vm) {
    if (sz != 0b01) {
        return UndefinedInstruction();
    }
    if (half_to_single && NeonBit<0>(Vd)) {
        return UndefinedInstruction();
    }
    if (!half_to_single && NeonBit<0>(Vm)) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto rounding_mode = RoundingMode::ToNearest_TieEven;  // StandardFPSCRValue().RMode
    const auto d = ToVector(half_to_single, Vd, D);
    const auto m = ToVector(!half_to_single, Vm, M);

    const auto operand = ir.GetVector(m);
    const Value result = half_to_single
                                 ? ir.FPVectorFromHalf(esize * 2, operand, rounding_mode, false)
                                 : ir.FPVectorToHalf(esize * 2, operand, rounding_mode, false);
    ir.SetVector(d, result);
    return true;
}

bool v8_VCVTA(bool D, size_t sz, size_t Vd, bool op, bool Q, bool M, size_t Vm) {
    return ConvertFloatToInteger(D, sz, Vd, op, Q, M, Vm, RoundingMode::ToNearest_TieAwayFromZero);
}

bool v8_VCVTN(bool D, size_t sz, size_t Vd, bool op, bool Q, bool M, size_t Vm) {
    return ConvertFloatToInteger(D, sz, Vd, op, Q, M, Vm, RoundingMode::ToNearest_TieEven);
}

bool v8_VCVTP(bool D, size_t sz, size_t Vd, bool op, bool Q, bool M, size_t Vm) {
    return ConvertFloatToInteger(D, sz, Vd, op, Q, M, Vm, RoundingMode::TowardsPlusInfinity);
}

bool v8_VCVTM(bool D, size_t sz, size_t Vd, bool op, bool Q, bool M, size_t Vm) {
    return ConvertFloatToInteger(D, sz, Vd, op, Q, M, Vm, RoundingMode::TowardsMinusInfinity);
}

bool asimd_VRECPE(bool D, size_t sz, size_t Vd, bool F, bool Q, bool M, size_t Vm) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    if (sz == 0b00 || sz == 0b11) {
        return UndefinedInstruction();
    }

    if (!F && sz == 0b01) {
        // TODO: Implement 16-bit VectorUnsignedRecipEstimate
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;

    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto reg_m = ir.GetVector(m);
    const auto result = F ? ir.FPVectorRecipEstimate(esize, reg_m, false)
                          : ir.VectorUnsignedRecipEstimate(reg_m);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VRSQRTE(bool D, size_t sz, size_t Vd, bool F, bool Q, bool M, size_t Vm) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    if (sz == 0b00 || sz == 0b11) {
        return UndefinedInstruction();
    }

    if (!F && sz == 0b01) {
        // TODO: Implement 16-bit VectorUnsignedRecipEstimate
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;

    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto reg_m = ir.GetVector(m);
    const auto result = F ? ir.FPVectorRSqrtEstimate(esize, reg_m, false)
                          : ir.VectorUnsignedRecipSqrtEstimate(reg_m);

    ir.SetVector(d, result);
    return true;
}

bool asimd_VCVT_integer(bool D, size_t sz, size_t Vd, bool op, bool U, bool Q, bool M, size_t Vm) {
    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    if (sz != 0b10) {
        return UndefinedInstruction();
    }

    const size_t esize = 8U << sz;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);
    const auto reg_m = ir.GetVector(m);
    const auto result =
            op ? (U ? ir.FPVectorToUnsignedFixed(esize, reg_m, 0, RoundingMode::TowardsZero, false)
                    : ir.FPVectorToSignedFixed(esize, reg_m, 0, RoundingMode::TowardsZero, false))
               : (U ? ir.FPVectorFromUnsignedFixed(
                              esize, reg_m, 0, RoundingMode::ToNearest_TieEven, false)
                    : ir.FPVectorFromSignedFixed(
                              esize, reg_m, 0, RoundingMode::ToNearest_TieEven, false));

    ir.SetVector(d, result);
    return true;
}

bool asimd_VCVT_fixed(
        bool U, bool D, size_t imm6, size_t Vd, bool to_fixed, bool Q, bool M, size_t Vm) {
    if (NeonBits<3, 5>(imm6) == 0) {
        return DecodeError();
    }

    if (Q && (NeonBit<0>(Vd) || NeonBit<0>(Vm))) {
        return UndefinedInstruction();
    }

    if (!NeonBit<5>(imm6)) {
        return UndefinedInstruction();
    }

    const size_t fbits = 64 - imm6;
    const auto d = ToVector(Q, Vd, D);
    const auto m = ToVector(Q, Vm, M);

    const auto reg_m = ir.GetVector(m);
    const auto result =
            to_fixed ? (U ? ir.FPVectorToUnsignedFixed(
                                    32, reg_m, fbits, RoundingMode::TowardsZero, false)
                          : ir.FPVectorToSignedFixed(
                                    32, reg_m, fbits, RoundingMode::TowardsZero, false))
                     : (U ? ir.FPVectorFromUnsignedFixed(
                                    32, reg_m, fbits, RoundingMode::ToNearest_TieEven, false)
                          : ir.FPVectorFromSignedFixed(
                                    32, reg_m, fbits, RoundingMode::ToNearest_TieEven, false));

    ir.SetVector(d, result);
    return true;
}
