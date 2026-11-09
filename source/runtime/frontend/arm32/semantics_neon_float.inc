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

template <Numeric operation> Value ScalarFPBinary(Value a, Value b) {
    return NumericScalar<operation>(ScalarWidth(a), a, b).WithView(a.ViewType());
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
    return NumericScalar<Numeric::Sqrt>(ScalarWidth(a), a, Imm64(0)).WithView(a.ViewType());
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
    return NumericScalar<Numeric::Precision>(
                   ScalarWidth(a), a, Imm64(runtime::arm32::NumericConvert(destination, rounding)))
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
    constexpr auto operation = unsign ? Numeric::FromUnsigned : Numeric::FromSigned;
    return NumericScalar<operation>(
                   destination,
                   a,
                   Imm64(runtime::arm32::NumericConvert(ScalarWidth(a), rounding, fbits)))
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
    constexpr auto operation = unsign ? Numeric::ToUnsigned : Numeric::ToSigned;
    return NumericScalar<operation>(
                   ScalarWidth(a),
                   a,
                   Imm64(runtime::arm32::NumericConvert(destination, rounding, fbits)))
            .WithView(NeonIntegerType(destination));
}

Value FPToFixedS16(Value a, size_t f, RoundingMode r) { return FPToInteger<false>(a, 16, f, r); }

Value FPToFixedU16(Value a, size_t f, RoundingMode r) { return FPToInteger<true>(a, 16, f, r); }

Value FPToFixedS32(Value a, size_t f, RoundingMode r) { return FPToInteger<false>(a, 32, f, r); }

Value FPToFixedU32(Value a, size_t f, RoundingMode r) { return FPToInteger<true>(a, 32, f, r); }

template <Numeric operation, bool standard = false> Value FusedFP(Value addend, Value a, Value b) {
    Emit<OpCode::StoreUniform, Type::Void>(
            Uniform{u32(layout.gprs_offset + offsetof(GuestState, numeric_addend)), Type::U64},
            addend.WithView(Type::U64));
    return NumericScalar<operation, standard>(ScalarWidth(a), a, b).WithView(a.ViewType());
}

Value FPMulAdd(Value addend, Value a, Value b) { return FusedFP<Numeric::Fused>(addend, a, b); }

Value FPMulSub(Value addend, Value a, Value b) {
    return FusedFP<Numeric::FusedSubtract>(addend, a, b);
}

#define ARM32_FP_VECTOR_BINARY(name, operation)                                                    \
    Value name(size_t bits, Value a, Value b, bool controlled = true) {                            \
        return controlled ? NumericVector<Numeric::operation>(bits, a, b)                          \
                          : NumericVector<Numeric::operation, true>(bits, a, b);                   \
    }
ARM32_FP_VECTOR_BINARY(FPVectorAdd, Add)
ARM32_FP_VECTOR_BINARY(FPVectorSub, Sub)
ARM32_FP_VECTOR_BINARY(FPVectorMul, Mul)
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
