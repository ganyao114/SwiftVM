// Packed DSP operations stay neutral integer IR. Each source lane is extended
// before arithmetic, so the scalar flags and GE/Q state remain independent.
Value BoolOr(Value a, Value b) {
    return Or(a.WithView(Type::U32), b.WithView(Type::U32)).WithView(Type::Bool);
}

Value BoolAnd(Value a, Value b) {
    return And(a.WithView(Type::U32), b.WithView(Type::U32)).WithView(Type::Bool);
}

Value BoolNot(Value value) { return TestZero(value); }

Value SignedLess64(Value a, Value b) {
    return TestBit(Sub(a.WithView(Type::U64), b.WithView(Type::U64)), Literal{63u});
}

OverflowResult ClampWide(Value value, unsigned width, bool unsign) {
    const u64 lo = unsign ? 0 : u64{0} - (u64{1} << (width - 1));
    const u64 hi = unsign ? ((u64{1} << width) - 1) : ((u64{1} << (width - 1)) - 1);
    const auto under = SignedLess64(value, Imm64(lo));
    const auto over = SignedLess64(Imm64(hi), value);
    return {Select(under, Imm64(lo), Select(over, Imm64(hi), value)), BoolOr(under, over)};
}

OverflowResult SignedSaturation(const Value& value, size_t bits) {
    auto result = ClampWide(SignExtendWordToLong(value), bits, false);
    result.result = result.result.WithView(Type::U32);
    return result;
}

OverflowResult UnsignedSaturation(const Value& value, size_t bits) {
    auto result = ClampWide(SignExtendWordToLong(value), bits, true);
    result.result = result.result.WithView(Type::U32);
    return result;
}

OverflowResult SignedSaturatedAddWithFlag(Value a, Value b) {
    auto result = ClampWide(Add(SignExtendWordToLong(a), SignExtendWordToLong(b)), 32, false);
    result.result = result.result.WithView(Type::U32);
    return result;
}

OverflowResult SignedSaturatedSubWithFlag(Value a, Value b) {
    auto result = ClampWide(Sub(SignExtendWordToLong(a), SignExtendWordToLong(b)), 32, false);
    result.result = result.result.WithView(Type::U32);
    return result;
}

Value PackedLane(Value word, unsigned bits, unsigned lane, bool sign) {
    auto value = lane ? LsrImm(word.WithView(Type::U32), Literal{bits * lane})
                      : word.WithView(Type::U32);
    value = value.WithView(ScalarType(bits));
    return sign ? Emit<OpCode::SignExtend, Type::U64>(value) : Emit<OpCode::ZeroExtend64>(value);
}

Value PackedInsert(Value word, Value value, unsigned bits, unsigned lane) {
    const auto masked = And(value.WithView(Type::U32), Literal{u32((u64{1} << bits) - 1)});
    return Or(word, lane ? LslImm(masked, Literal{bits * lane}) : masked);
}

PackedResult PackedArithmetic(
        Value a, Value b, unsigned bits, bool sign, bool subtract, int exchange = 0) {
    auto result = Imm32(0), ge = Imm32(0);
    for (unsigned lane = 0; lane < 32 / bits; ++lane) {
        const auto first = PackedLane(a, bits, lane, sign);
        const auto second = PackedLane(b, bits, exchange ? lane ^ 1 : lane, sign);
        const bool sub = exchange ? ((lane == 0) == (exchange > 0)) : subtract;
        const auto wide = sub ? Sub(first, second) : Add(first, second);
        result = PackedInsert(result, wide, bits, lane);
        Value flag;
        if (sign || sub)
            flag = BoolNot(TestBit(wide, Literal{63u}));
        else
            flag = TestBit(wide, Literal{bits});
        const auto mask =
                bits == 16 ? Mul(flag.WithView(Type::U32), Literal{3u}) : flag.WithView(Type::U32);
        ge = Or(ge, lane ? LslImm(mask, Literal{lane * (bits / 8)}) : mask);
    }
    return {result, ge};
}

Value PackedHalving(Value a, Value b, unsigned bits, bool sign, bool subtract, int exchange = 0) {
    auto result = Imm32(0);
    for (unsigned lane = 0; lane < 32 / bits; ++lane) {
        const auto first = PackedLane(a, bits, lane, sign);
        const auto second = PackedLane(b, bits, exchange ? lane ^ 1 : lane, sign);
        const bool sub = exchange ? ((lane == 0) == (exchange > 0)) : subtract;
        const auto wide = sub ? Sub(first, second) : Add(first, second);
        result = PackedInsert(
                result, sign ? AsrImm(wide, Literal{1u}) : LsrImm(wide, Literal{1u}), bits, lane);
    }
    return result;
}

Value PackedSaturated(Value a, Value b, unsigned bits, bool sign, bool subtract) {
    auto result = Imm32(0);
    for (unsigned lane = 0; lane < 32 / bits; ++lane) {
        const auto first = PackedLane(a, bits, lane, sign),
                   second = PackedLane(b, bits, lane, sign);
        const auto wide = subtract ? Sub(first, second) : Add(first, second);
        result = PackedInsert(result, ClampWide(wide, bits, !sign).result, bits, lane);
    }
    return result;
}

#define ARM32_PACKED_BINARY(name, implementation, bits, sign, subtract)                            \
    auto name(Value a, Value b) { return implementation(a, b, bits, sign, subtract); }
ARM32_PACKED_BINARY(PackedAddS8, PackedArithmetic, 8, true, false)
ARM32_PACKED_BINARY(PackedAddS16, PackedArithmetic, 16, true, false)
ARM32_PACKED_BINARY(PackedAddU8, PackedArithmetic, 8, false, false)
ARM32_PACKED_BINARY(PackedAddU16, PackedArithmetic, 16, false, false)
ARM32_PACKED_BINARY(PackedSubS8, PackedArithmetic, 8, true, true)
ARM32_PACKED_BINARY(PackedSubS16, PackedArithmetic, 16, true, true)
ARM32_PACKED_BINARY(PackedSubU8, PackedArithmetic, 8, false, true)
ARM32_PACKED_BINARY(PackedSubU16, PackedArithmetic, 16, false, true)
ARM32_PACKED_BINARY(PackedHalvingAddS8, PackedHalving, 8, true, false)
ARM32_PACKED_BINARY(PackedHalvingAddS16, PackedHalving, 16, true, false)
ARM32_PACKED_BINARY(PackedHalvingAddU8, PackedHalving, 8, false, false)
ARM32_PACKED_BINARY(PackedHalvingAddU16, PackedHalving, 16, false, false)
ARM32_PACKED_BINARY(PackedHalvingSubS8, PackedHalving, 8, true, true)
ARM32_PACKED_BINARY(PackedHalvingSubS16, PackedHalving, 16, true, true)
ARM32_PACKED_BINARY(PackedHalvingSubU8, PackedHalving, 8, false, true)
ARM32_PACKED_BINARY(PackedHalvingSubU16, PackedHalving, 16, false, true)
ARM32_PACKED_BINARY(PackedSaturatedAddS8, PackedSaturated, 8, true, false)
ARM32_PACKED_BINARY(PackedSaturatedAddS16, PackedSaturated, 16, true, false)
ARM32_PACKED_BINARY(PackedSaturatedAddU8, PackedSaturated, 8, false, false)
ARM32_PACKED_BINARY(PackedSaturatedAddU16, PackedSaturated, 16, false, false)
ARM32_PACKED_BINARY(PackedSaturatedSubS8, PackedSaturated, 8, true, true)
ARM32_PACKED_BINARY(PackedSaturatedSubS16, PackedSaturated, 16, true, true)
ARM32_PACKED_BINARY(PackedSaturatedSubU8, PackedSaturated, 8, false, true)
ARM32_PACKED_BINARY(PackedSaturatedSubU16, PackedSaturated, 16, false, true)
#undef ARM32_PACKED_BINARY
#define ARM32_PACKED_EXCHANGE(name, implementation, sign, exchange)                                \
    auto name(Value a, Value b) { return implementation(a, b, 16, sign, false, exchange); }
ARM32_PACKED_EXCHANGE(PackedAddSubS16, PackedArithmetic, true, 1)
ARM32_PACKED_EXCHANGE(PackedAddSubU16, PackedArithmetic, false, 1)
ARM32_PACKED_EXCHANGE(PackedSubAddS16, PackedArithmetic, true, -1)
ARM32_PACKED_EXCHANGE(PackedSubAddU16, PackedArithmetic, false, -1)
ARM32_PACKED_EXCHANGE(PackedHalvingAddSubS16, PackedHalving, true, 1)
ARM32_PACKED_EXCHANGE(PackedHalvingAddSubU16, PackedHalving, false, 1)
ARM32_PACKED_EXCHANGE(PackedHalvingSubAddS16, PackedHalving, true, -1)
ARM32_PACKED_EXCHANGE(PackedHalvingSubAddU16, PackedHalving, false, -1)
#undef ARM32_PACKED_EXCHANGE

Value PackedSelect(Value ge, Value a, Value b) {
    auto result = Imm32(0);
    for (unsigned lane = 0; lane < 4; ++lane) {
        const auto selected = Select(TestBit(ge, Literal{lane}),
                                     PackedLane(a, 8, lane, false),
                                     PackedLane(b, 8, lane, false));
        result = PackedInsert(result, selected, 8, lane);
    }
    return result;
}

Value PackedAbsDiffSumU8(Value a, Value b) {
    auto sum = Imm32(0);
    for (unsigned lane = 0; lane < 4; ++lane) {
        const auto first = PackedLane(a, 8, lane, false), second = PackedLane(b, 8, lane, false);
        const auto difference = Sub(first, second);
        sum = Add(sum,
                  Select(TestBit(difference, Literal{63u}), Sub(second, first), difference)
                          .WithView(Type::U32));
    }
    return sum;
}

// Imported A32 instruction semantics.
// Adapted from dynarmic-interp 5a76f693 (MerryMage and contributors), SPDX-License-Identifier:
// 0BSD.

// parallel
/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

// SADD8<c> <Rd>, <Rn>, <Rm>
bool arm_SADD8(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedAddS8(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

// SADD16<c> <Rd>, <Rn>, <Rm>
bool arm_SADD16(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedAddS16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

// SASX<c> <Rd>, <Rn>, <Rm>
bool arm_SASX(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedAddSubS16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

// SSAX<c> <Rd>, <Rn>, <Rm>
bool arm_SSAX(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedSubAddS16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

// SSUB8<c> <Rd>, <Rn>, <Rm>
bool arm_SSUB8(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedSubS8(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

// SSUB16<c> <Rd>, <Rn>, <Rm>
bool arm_SSUB16(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedSubS16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

// UADD8<c> <Rd>, <Rn>, <Rm>
bool arm_UADD8(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedAddU8(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

// UADD16<c> <Rd>, <Rn>, <Rm>
bool arm_UADD16(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedAddU16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

// UASX<c> <Rd>, <Rn>, <Rm>
bool arm_UASX(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedAddSubU16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

// USAX<c> <Rd>, <Rn>, <Rm>
bool arm_USAX(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedSubAddU16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

// USAD8<c> <Rd>, <Rn>, <Rm>
bool arm_USAD8(Cond cond, Reg d, Reg m, Reg n) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedAbsDiffSumU8(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// USADA8<c> <Rd>, <Rn>, <Rm>, <Ra>
bool arm_USADA8(Cond cond, Reg d, Reg a, Reg m, Reg n) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto tmp = ir.PackedAbsDiffSumU8(ir.GetRegister(n), ir.GetRegister(m));
    const auto result = ir.AddWithCarry(ir.GetRegister(a), tmp, ir.Imm1(0));
    ir.SetRegister(d, result);
    return true;
}

// USUB8<c> <Rd>, <Rn>, <Rm>
bool arm_USUB8(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedSubU8(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

// USUB16<c> <Rd>, <Rn>, <Rm>
bool arm_USUB16(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedSubU16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

// Parallel Add/Subtract (Saturating) instructions

// QADD8<c> <Rd>, <Rn>, <Rm>
bool arm_QADD8(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedSaturatedAddS8(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// QADD16<c> <Rd>, <Rn>, <Rm>
bool arm_QADD16(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedSaturatedAddS16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// QSUB8<c> <Rd>, <Rn>, <Rm>
bool arm_QSUB8(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedSaturatedSubS8(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// QSUB16<c> <Rd>, <Rn>, <Rm>
bool arm_QSUB16(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedSaturatedSubS16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// UQADD8<c> <Rd>, <Rn>, <Rm>
bool arm_UQADD8(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedSaturatedAddU8(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// UQADD16<c> <Rd>, <Rn>, <Rm>
bool arm_UQADD16(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedSaturatedAddU16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// UQSUB8<c> <Rd>, <Rn>, <Rm>
bool arm_UQSUB8(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedSaturatedSubU8(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// UQSUB16<c> <Rd>, <Rn>, <Rm>
bool arm_UQSUB16(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedSaturatedSubU16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// Parallel Add/Subtract (Halving) instructions

// SHADD8<c> <Rd>, <Rn>, <Rm>
bool arm_SHADD8(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedHalvingAddS8(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// SHADD16<c> <Rd>, <Rn>, <Rm>
bool arm_SHADD16(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedHalvingAddS16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// SHASX<c> <Rd>, <Rn>, <Rm>
bool arm_SHASX(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedHalvingAddSubS16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// SHSAX<c> <Rd>, <Rn>, <Rm>
bool arm_SHSAX(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedHalvingSubAddS16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// SHSUB8<c> <Rd>, <Rn>, <Rm>
bool arm_SHSUB8(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedHalvingSubS8(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// SHSUB16<c> <Rd>, <Rn>, <Rm>
bool arm_SHSUB16(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedHalvingSubS16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// UHADD8<c> <Rd>, <Rn>, <Rm>
bool arm_UHADD8(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedHalvingAddU8(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// UHADD16<c> <Rd>, <Rn>, <Rm>
bool arm_UHADD16(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedHalvingAddU16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// UHASX<c> <Rd>, <Rn>, <Rm>
bool arm_UHASX(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedHalvingAddSubU16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// UHSAX<c> <Rd>, <Rn>, <Rm>
bool arm_UHSAX(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedHalvingSubAddU16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// UHSUB8<c> <Rd>, <Rn>, <Rm>
bool arm_UHSUB8(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedHalvingSubU8(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// UHSUB16<c> <Rd>, <Rn>, <Rm>
bool arm_UHSUB16(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.PackedHalvingSubU16(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// saturated
/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

static Value saturated_Pack2x16To1x32(Semantics& ir, Value lo, Value hi) {
    return ir.Or(ir.And(lo, ir.Imm32(0xFFFF)),
                 ir.LogicalShiftLeft(hi, ir.Imm8(16), ir.Imm1(0)).result);
}

static Value saturated_MostSignificantHalf(Semantics& ir, Value value) {
    return ir.LeastSignificantHalf(ir.LogicalShiftRight(value, ir.Imm8(16), ir.Imm1(0)).result);
}

// Saturation instructions

// SSAT<c> <Rd>, #<imm>, <Rn>{, <shift>}
bool arm_SSAT(Cond cond, Imm<5> sat_imm, Reg d, Imm<5> imm5, bool sh, Reg n) {
    if (d == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto saturate_to = static_cast<size_t>(sat_imm.ZeroExtend()) + 1;
    const auto shift = !sh ? ShiftType::LSL : ShiftType::ASR;
    const auto operand = EmitImmShift(ir.GetRegister(n), shift, imm5, ir.GetCFlag());
    const auto result = ir.SignedSaturation(operand.result, saturate_to);

    ir.SetRegister(d, result.result);
    ir.OrQFlag(result.overflow);
    return true;
}

// SSAT16<c> <Rd>, #<imm>, <Rn>
bool arm_SSAT16(Cond cond, Imm<4> sat_imm, Reg d, Reg n) {
    if (d == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto saturate_to = static_cast<size_t>(sat_imm.ZeroExtend()) + 1;
    const auto lo_operand = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(ir.GetRegister(n)));
    const auto hi_operand =
            ir.SignExtendHalfToWord(saturated_MostSignificantHalf(ir, ir.GetRegister(n)));
    const auto lo_result = ir.SignedSaturation(lo_operand, saturate_to);
    const auto hi_result = ir.SignedSaturation(hi_operand, saturate_to);

    ir.SetRegister(d, saturated_Pack2x16To1x32(ir, lo_result.result, hi_result.result));
    ir.OrQFlag(lo_result.overflow);
    ir.OrQFlag(hi_result.overflow);
    return true;
}

// USAT<c> <Rd>, #<imm5>, <Rn>{, <shift>}
bool arm_USAT(Cond cond, Imm<5> sat_imm, Reg d, Imm<5> imm5, bool sh, Reg n) {
    if (d == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto saturate_to = static_cast<size_t>(sat_imm.ZeroExtend());
    const auto shift = !sh ? ShiftType::LSL : ShiftType::ASR;
    const auto operand = EmitImmShift(ir.GetRegister(n), shift, imm5, ir.GetCFlag());
    const auto result = ir.UnsignedSaturation(operand.result, saturate_to);

    ir.SetRegister(d, result.result);
    ir.OrQFlag(result.overflow);
    return true;
}

// USAT16<c> <Rd>, #<imm4>, <Rn>
bool arm_USAT16(Cond cond, Imm<4> sat_imm, Reg d, Reg n) {
    if (d == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    // UnsignedSaturation takes a *signed* value as input, hence sign extension is required.
    const auto saturate_to = static_cast<size_t>(sat_imm.ZeroExtend());
    const auto lo_operand = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(ir.GetRegister(n)));
    const auto hi_operand =
            ir.SignExtendHalfToWord(saturated_MostSignificantHalf(ir, ir.GetRegister(n)));
    const auto lo_result = ir.UnsignedSaturation(lo_operand, saturate_to);
    const auto hi_result = ir.UnsignedSaturation(hi_operand, saturate_to);

    ir.SetRegister(d, saturated_Pack2x16To1x32(ir, lo_result.result, hi_result.result));
    ir.OrQFlag(lo_result.overflow);
    ir.OrQFlag(hi_result.overflow);
    return true;
}

// Saturated Add/Subtract instructions

// QADD<c> <Rd>, <Rm>, <Rn>
bool arm_QADD(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto a = ir.GetRegister(m);
    const auto b = ir.GetRegister(n);
    const auto result = ir.SignedSaturatedAddWithFlag(a, b);

    ir.SetRegister(d, result.result);
    ir.OrQFlag(result.overflow);
    return true;
}

// QSUB<c> <Rd>, <Rm>, <Rn>
bool arm_QSUB(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto a = ir.GetRegister(m);
    const auto b = ir.GetRegister(n);
    const auto result = ir.SignedSaturatedSubWithFlag(a, b);

    ir.SetRegister(d, result.result);
    ir.OrQFlag(result.overflow);
    return true;
}

// QDADD<c> <Rd>, <Rm>, <Rn>
bool arm_QDADD(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto a = ir.GetRegister(m);
    const auto b = ir.GetRegister(n);
    const auto doubled = ir.SignedSaturatedAddWithFlag(b, b);
    ir.OrQFlag(doubled.overflow);

    const auto result = ir.SignedSaturatedAddWithFlag(a, doubled.result);
    ir.SetRegister(d, result.result);
    ir.OrQFlag(result.overflow);
    return true;
}

// QDSUB<c> <Rd>, <Rm>, <Rn>
bool arm_QDSUB(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto a = ir.GetRegister(m);
    const auto b = ir.GetRegister(n);
    const auto doubled = ir.SignedSaturatedAddWithFlag(b, b);
    ir.OrQFlag(doubled.overflow);

    const auto result = ir.SignedSaturatedSubWithFlag(a, doubled.result);
    ir.SetRegister(d, result.result);
    ir.OrQFlag(result.overflow);
    return true;
}

// Parallel saturated instructions

// QASX<c> <Rd>, <Rn>, <Rm>
bool arm_QASX(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto Rn = ir.GetRegister(n);
    const auto Rm = ir.GetRegister(m);
    const auto Rn_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(Rn));
    const auto Rn_hi = ir.SignExtendHalfToWord(saturated_MostSignificantHalf(ir, Rn));
    const auto Rm_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(Rm));
    const auto Rm_hi = ir.SignExtendHalfToWord(saturated_MostSignificantHalf(ir, Rm));
    const auto diff = ir.SignedSaturation(ir.Sub(Rn_lo, Rm_hi), 16).result;
    const auto sum = ir.SignedSaturation(ir.Add(Rn_hi, Rm_lo), 16).result;
    const auto result = saturated_Pack2x16To1x32(ir, diff, sum);

    ir.SetRegister(d, result);
    return true;
}

// QSAX<c> <Rd>, <Rn>, <Rm>
bool arm_QSAX(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto Rn = ir.GetRegister(n);
    const auto Rm = ir.GetRegister(m);
    const auto Rn_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(Rn));
    const auto Rn_hi = ir.SignExtendHalfToWord(saturated_MostSignificantHalf(ir, Rn));
    const auto Rm_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(Rm));
    const auto Rm_hi = ir.SignExtendHalfToWord(saturated_MostSignificantHalf(ir, Rm));
    const auto sum = ir.SignedSaturation(ir.Add(Rn_lo, Rm_hi), 16).result;
    const auto diff = ir.SignedSaturation(ir.Sub(Rn_hi, Rm_lo), 16).result;
    const auto result = saturated_Pack2x16To1x32(ir, sum, diff);

    ir.SetRegister(d, result);
    return true;
}

// UQASX<c> <Rd>, <Rn>, <Rm>
bool arm_UQASX(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto Rn = ir.GetRegister(n);
    const auto Rm = ir.GetRegister(m);
    const auto Rn_lo = ir.ZeroExtendHalfToWord(ir.LeastSignificantHalf(Rn));
    const auto Rn_hi = ir.ZeroExtendHalfToWord(saturated_MostSignificantHalf(ir, Rn));
    const auto Rm_lo = ir.ZeroExtendHalfToWord(ir.LeastSignificantHalf(Rm));
    const auto Rm_hi = ir.ZeroExtendHalfToWord(saturated_MostSignificantHalf(ir, Rm));
    const auto diff = ir.UnsignedSaturation(ir.Sub(Rn_lo, Rm_hi), 16).result;
    const auto sum = ir.UnsignedSaturation(ir.Add(Rn_hi, Rm_lo), 16).result;
    const auto result = saturated_Pack2x16To1x32(ir, diff, sum);

    ir.SetRegister(d, result);
    return true;
}

// UQSAX<c> <Rd>, <Rn>, <Rm>
bool arm_UQSAX(Cond cond, Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto Rn = ir.GetRegister(n);
    const auto Rm = ir.GetRegister(m);
    const auto Rn_lo = ir.ZeroExtendHalfToWord(ir.LeastSignificantHalf(Rn));
    const auto Rn_hi = ir.ZeroExtendHalfToWord(saturated_MostSignificantHalf(ir, Rn));
    const auto Rm_lo = ir.ZeroExtendHalfToWord(ir.LeastSignificantHalf(Rm));
    const auto Rm_hi = ir.ZeroExtendHalfToWord(saturated_MostSignificantHalf(ir, Rm));
    const auto sum = ir.UnsignedSaturation(ir.Add(Rn_lo, Rm_hi), 16).result;
    const auto diff = ir.UnsignedSaturation(ir.Sub(Rn_hi, Rm_lo), 16).result;
    const auto result = saturated_Pack2x16To1x32(ir, sum, diff);

    ir.SetRegister(d, result);
    return true;
}

// thumb32_parallel
/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

static Value thumb32_parallel_Pack2x16To1x32(Semantics& ir, Value lo, Value hi) {
    return ir.Or(ir.And(lo, ir.Imm32(0xFFFF)),
                 ir.LogicalShiftLeft(hi, ir.Imm8(16), ir.Imm1(0)).result);
}

static Value thumb32_parallel_MostSignificantHalf(Semantics& ir, Value value) {
    return ir.LeastSignificantHalf(ir.LogicalShiftRight(value, ir.Imm8(16), ir.Imm1(0)).result);
}

bool thumb32_SADD8(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedAddS8(reg_n, reg_m);

    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

bool thumb32_SADD16(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedAddS16(reg_n, reg_m);

    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

bool thumb32_SASX(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedAddSubS16(reg_n, reg_m);

    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

bool thumb32_SSAX(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedSubAddS16(reg_n, reg_m);

    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

bool thumb32_SSUB8(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedSubS8(reg_n, reg_m);

    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

bool thumb32_SSUB16(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedSubS16(reg_n, reg_m);

    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

bool thumb32_UADD8(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedAddU8(reg_n, reg_m);

    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

bool thumb32_UADD16(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedAddU16(reg_n, reg_m);

    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

bool thumb32_UASX(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedAddSubU16(reg_n, reg_m);

    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

bool thumb32_USAX(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedSubAddU16(reg_n, reg_m);

    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

bool thumb32_USUB8(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedSubU8(reg_n, reg_m);

    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

bool thumb32_USUB16(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedSubU16(reg_n, reg_m);

    ir.SetRegister(d, result.result);
    ir.SetGEFlags(result.ge);
    return true;
}

bool thumb32_QADD8(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedSaturatedAddS8(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_QADD16(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedSaturatedAddS16(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_QASX(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto Rn = ir.GetRegister(n);
    const auto Rm = ir.GetRegister(m);
    const auto Rn_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(Rn));
    const auto Rn_hi = ir.SignExtendHalfToWord(thumb32_parallel_MostSignificantHalf(ir, Rn));
    const auto Rm_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(Rm));
    const auto Rm_hi = ir.SignExtendHalfToWord(thumb32_parallel_MostSignificantHalf(ir, Rm));
    const auto diff = ir.SignedSaturation(ir.Sub(Rn_lo, Rm_hi), 16).result;
    const auto sum = ir.SignedSaturation(ir.Add(Rn_hi, Rm_lo), 16).result;
    const auto result = thumb32_parallel_Pack2x16To1x32(ir, diff, sum);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_QSAX(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto Rn = ir.GetRegister(n);
    const auto Rm = ir.GetRegister(m);
    const auto Rn_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(Rn));
    const auto Rn_hi = ir.SignExtendHalfToWord(thumb32_parallel_MostSignificantHalf(ir, Rn));
    const auto Rm_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(Rm));
    const auto Rm_hi = ir.SignExtendHalfToWord(thumb32_parallel_MostSignificantHalf(ir, Rm));
    const auto sum = ir.SignedSaturation(ir.Add(Rn_lo, Rm_hi), 16).result;
    const auto diff = ir.SignedSaturation(ir.Sub(Rn_hi, Rm_lo), 16).result;
    const auto result = thumb32_parallel_Pack2x16To1x32(ir, sum, diff);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_QSUB8(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedSaturatedSubS8(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_QSUB16(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedSaturatedSubS16(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UQADD8(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedSaturatedAddU8(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UQADD16(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedSaturatedAddU16(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UQASX(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto Rn = ir.GetRegister(n);
    const auto Rm = ir.GetRegister(m);
    const auto Rn_lo = ir.ZeroExtendHalfToWord(ir.LeastSignificantHalf(Rn));
    const auto Rn_hi = ir.ZeroExtendHalfToWord(thumb32_parallel_MostSignificantHalf(ir, Rn));
    const auto Rm_lo = ir.ZeroExtendHalfToWord(ir.LeastSignificantHalf(Rm));
    const auto Rm_hi = ir.ZeroExtendHalfToWord(thumb32_parallel_MostSignificantHalf(ir, Rm));
    const auto diff = ir.UnsignedSaturation(ir.Sub(Rn_lo, Rm_hi), 16).result;
    const auto sum = ir.UnsignedSaturation(ir.Add(Rn_hi, Rm_lo), 16).result;
    const auto result = thumb32_parallel_Pack2x16To1x32(ir, diff, sum);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UQSAX(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto Rn = ir.GetRegister(n);
    const auto Rm = ir.GetRegister(m);
    const auto Rn_lo = ir.ZeroExtendHalfToWord(ir.LeastSignificantHalf(Rn));
    const auto Rn_hi = ir.ZeroExtendHalfToWord(thumb32_parallel_MostSignificantHalf(ir, Rn));
    const auto Rm_lo = ir.ZeroExtendHalfToWord(ir.LeastSignificantHalf(Rm));
    const auto Rm_hi = ir.ZeroExtendHalfToWord(thumb32_parallel_MostSignificantHalf(ir, Rm));
    const auto sum = ir.UnsignedSaturation(ir.Add(Rn_lo, Rm_hi), 16).result;
    const auto diff = ir.UnsignedSaturation(ir.Sub(Rn_hi, Rm_lo), 16).result;
    const auto result = thumb32_parallel_Pack2x16To1x32(ir, sum, diff);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UQSUB8(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedSaturatedSubU8(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UQSUB16(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedSaturatedSubU16(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_SHADD8(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedHalvingAddS8(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_SHADD16(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedHalvingAddS16(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_SHASX(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedHalvingAddSubS16(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_SHSAX(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedHalvingSubAddS16(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_SHSUB8(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedHalvingSubS8(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_SHSUB16(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedHalvingSubS16(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UHADD8(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedHalvingAddU8(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UHADD16(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedHalvingAddU16(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UHASX(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedHalvingAddSubU16(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UHSAX(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedHalvingSubAddU16(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UHSUB8(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedHalvingSubU8(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UHSUB16(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedHalvingSubU16(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}
