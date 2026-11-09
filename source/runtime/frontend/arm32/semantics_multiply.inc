// Imported A32 instruction semantics.
// Adapted from dynarmic-interp 5a76f693 (MerryMage and contributors), SPDX-License-Identifier:
// 0BSD.

// multiply
/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

// MLA{S}<c> <Rd>, <Rn>, <Rm>, <Ra>
bool arm_MLA(Cond cond, bool S, Reg d, Reg a, Reg m, Reg n) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC || a == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.Add(ir.Mul(ir.GetRegister(n), ir.GetRegister(m)), ir.GetRegister(a));
    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZ(ir.NZFrom(result));
    }

    return true;
}

// MLS<c> <Rd>, <Rn>, <Rm>, <Ra>
bool arm_MLS(Cond cond, Reg d, Reg a, Reg m, Reg n) {
    if (d == Reg::PC || a == Reg::PC || m == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Value operand1 = ir.GetRegister(n);
    const Value operand2 = ir.GetRegister(m);
    const Value operand3 = ir.GetRegister(a);
    const Value result = ir.Sub(operand3, ir.Mul(operand1, operand2));

    ir.SetRegister(d, result);
    return true;
}

// MUL{S}<c> <Rd>, <Rn>, <Rm>
bool arm_MUL(Cond cond, bool S, Reg d, Reg m, Reg n) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.Mul(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZ(ir.NZFrom(result));
    }

    return true;
}

// SMLAL{S}<c> <RdLo>, <RdHi>, <Rn>, <Rm>
bool arm_SMLAL(Cond cond, bool S, Reg dHi, Reg dLo, Reg m, Reg n) {
    if (dLo == Reg::PC || dHi == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (dLo == dHi) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto n64 = ir.SignExtendWordToLong(ir.GetRegister(n));
    const auto m64 = ir.SignExtendWordToLong(ir.GetRegister(m));
    const auto product = ir.Mul(n64, m64);
    const auto addend = ir.Pack2x32To1x64(ir.GetRegister(dLo), ir.GetRegister(dHi));
    const auto result = ir.Add(product, addend);
    const auto lo = ir.LeastSignificantWord(result);
    const auto hi = ir.MostSignificantWord(result).result;

    ir.SetRegister(dLo, lo);
    ir.SetRegister(dHi, hi);
    if (S) {
        ir.SetCpsrNZ(ir.NZFrom(result));
    }

    return true;
}

// SMULL{S}<c> <RdLo>, <RdHi>, <Rn>, <Rm>
bool arm_SMULL(Cond cond, bool S, Reg dHi, Reg dLo, Reg m, Reg n) {
    if (dLo == Reg::PC || dHi == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (dLo == dHi) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto n64 = ir.SignExtendWordToLong(ir.GetRegister(n));
    const auto m64 = ir.SignExtendWordToLong(ir.GetRegister(m));
    const auto result = ir.Mul(n64, m64);
    const auto lo = ir.LeastSignificantWord(result);
    const auto hi = ir.MostSignificantWord(result).result;

    ir.SetRegister(dLo, lo);
    ir.SetRegister(dHi, hi);
    if (S) {
        ir.SetCpsrNZ(ir.NZFrom(result));
    }

    return true;
}

// UMAAL<c> <RdLo>, <RdHi>, <Rn>, <Rm>
bool arm_UMAAL(Cond cond, Reg dHi, Reg dLo, Reg m, Reg n) {
    if (dLo == Reg::PC || dHi == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (dLo == dHi) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto lo64 = ir.ZeroExtendWordToLong(ir.GetRegister(dLo));
    const auto hi64 = ir.ZeroExtendWordToLong(ir.GetRegister(dHi));
    const auto n64 = ir.ZeroExtendWordToLong(ir.GetRegister(n));
    const auto m64 = ir.ZeroExtendWordToLong(ir.GetRegister(m));
    const auto result = ir.Add(ir.Add(ir.Mul(n64, m64), hi64), lo64);

    ir.SetRegister(dLo, ir.LeastSignificantWord(result));
    ir.SetRegister(dHi, ir.MostSignificantWord(result).result);
    return true;
}

// UMLAL{S}<c> <RdLo>, <RdHi>, <Rn>, <Rm>
bool arm_UMLAL(Cond cond, bool S, Reg dHi, Reg dLo, Reg m, Reg n) {
    if (dLo == Reg::PC || dHi == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (dLo == dHi) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto addend = ir.Pack2x32To1x64(ir.GetRegister(dLo), ir.GetRegister(dHi));
    const auto n64 = ir.ZeroExtendWordToLong(ir.GetRegister(n));
    const auto m64 = ir.ZeroExtendWordToLong(ir.GetRegister(m));
    const auto result = ir.Add(ir.Mul(n64, m64), addend);
    const auto lo = ir.LeastSignificantWord(result);
    const auto hi = ir.MostSignificantWord(result).result;

    ir.SetRegister(dLo, lo);
    ir.SetRegister(dHi, hi);
    if (S) {
        ir.SetCpsrNZ(ir.NZFrom(result));
    }

    return true;
}

// UMULL{S}<c> <RdLo>, <RdHi>, <Rn>, <Rm>
bool arm_UMULL(Cond cond, bool S, Reg dHi, Reg dLo, Reg m, Reg n) {
    if (dLo == Reg::PC || dHi == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (dLo == dHi) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto n64 = ir.ZeroExtendWordToLong(ir.GetRegister(n));
    const auto m64 = ir.ZeroExtendWordToLong(ir.GetRegister(m));
    const auto result = ir.Mul(n64, m64);
    const auto lo = ir.LeastSignificantWord(result);
    const auto hi = ir.MostSignificantWord(result).result;

    ir.SetRegister(dLo, lo);
    ir.SetRegister(dHi, hi);
    if (S) {
        ir.SetCpsrNZ(ir.NZFrom(result));
    }

    return true;
}

// SMLAL<x><y><c> <RdLo>, <RdHi>, <Rn>, <Rm>
bool arm_SMLALxy(Cond cond, Reg dHi, Reg dLo, Reg m, bool M, bool N, Reg n) {
    if (dLo == Reg::PC || dHi == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (dLo == dHi) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Value n32 = ir.GetRegister(n);
    const Value m32 = ir.GetRegister(m);
    const Value n16 = N ? ir.ArithmeticShiftRight(n32, ir.Imm8(16), ir.Imm1(0)).result
                        : ir.SignExtendHalfToWord(ir.LeastSignificantHalf(n32));
    const Value m16 = M ? ir.ArithmeticShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result
                        : ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32));
    const Value product = ir.SignExtendWordToLong(ir.Mul(n16, m16));
    const auto addend = ir.Pack2x32To1x64(ir.GetRegister(dLo), ir.GetRegister(dHi));
    const auto result = ir.Add(product, addend);

    ir.SetRegister(dLo, ir.LeastSignificantWord(result));
    ir.SetRegister(dHi, ir.MostSignificantWord(result).result);
    return true;
}

// SMLA<x><y><c> <Rd>, <Rn>, <Rm>, <Ra>
bool arm_SMLAxy(Cond cond, Reg d, Reg a, Reg m, bool M, bool N, Reg n) {
    Value arithmetic_overflow;
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC || a == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Value n32 = ir.GetRegister(n);
    const Value m32 = ir.GetRegister(m);
    const Value n16 = N ? ir.ArithmeticShiftRight(n32, ir.Imm8(16), ir.Imm1(0)).result
                        : ir.SignExtendHalfToWord(ir.LeastSignificantHalf(n32));
    const Value m16 = M ? ir.ArithmeticShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result
                        : ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32));
    const Value product = ir.Mul(n16, m16);
    const auto result =
            ir.AddWithOverflow(product, ir.GetRegister(a), ir.Imm1(0), arithmetic_overflow);

    ir.SetRegister(d, result);
    ir.OrQFlag(arithmetic_overflow);
    return true;
}

// SMUL<x><y><c> <Rd>, <Rn>, <Rm>
bool arm_SMULxy(Cond cond, Reg d, Reg m, bool M, bool N, Reg n) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Value n32 = ir.GetRegister(n);
    const Value m32 = ir.GetRegister(m);
    const Value n16 = N ? ir.ArithmeticShiftRight(n32, ir.Imm8(16), ir.Imm1(0)).result
                        : ir.SignExtendHalfToWord(ir.LeastSignificantHalf(n32));
    const Value m16 = M ? ir.ArithmeticShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result
                        : ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32));
    const Value result = ir.Mul(n16, m16);

    ir.SetRegister(d, result);
    return true;
}

// SMLAW<y><c> <Rd>, <Rn>, <Rm>, <Ra>
bool arm_SMLAWy(Cond cond, Reg d, Reg a, Reg m, bool M, Reg n) {
    Value arithmetic_overflow;
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC || a == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Value n32 = ir.SignExtendWordToLong(ir.GetRegister(n));
    Value m32 = ir.GetRegister(m);
    if (M) {
        m32 = ir.LogicalShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result;
    }
    const Value m16 =
            ir.SignExtendWordToLong(ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32)));
    const auto product =
            ir.LeastSignificantWord(ir.LogicalShiftRight(ir.Mul(n32, m16), ir.Imm8(16)));
    const auto result =
            ir.AddWithOverflow(product, ir.GetRegister(a), ir.Imm1(0), arithmetic_overflow);

    ir.SetRegister(d, result);
    ir.OrQFlag(arithmetic_overflow);
    return true;
}

// SMULW<y><c> <Rd>, <Rn>, <Rm>
bool arm_SMULWy(Cond cond, Reg d, Reg m, bool M, Reg n) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Value n32 = ir.SignExtendWordToLong(ir.GetRegister(n));
    Value m32 = ir.GetRegister(m);
    if (M) {
        m32 = ir.LogicalShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result;
    }
    const Value m16 =
            ir.SignExtendWordToLong(ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32)));
    const auto result = ir.LogicalShiftRight(ir.Mul(n32, m16), ir.Imm8(16));

    ir.SetRegister(d, ir.LeastSignificantWord(result));
    return true;
}

// SMMLA{R}<c> <Rd>, <Rn>, <Rm>, <Ra>
bool arm_SMMLA(Cond cond, Reg d, Reg a, Reg m, bool R, Reg n) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC /* no check for a */) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto n64 = ir.SignExtendWordToLong(ir.GetRegister(n));
    const auto m64 = ir.SignExtendWordToLong(ir.GetRegister(m));
    const auto a64 = ir.Pack2x32To1x64(ir.Imm32(0), ir.GetRegister(a));
    const auto temp = ir.Add(a64, ir.Mul(n64, m64));
    const auto result_carry = ir.MostSignificantWord(temp);
    auto result = result_carry.result;
    if (R) {
        result = ir.AddWithCarry(result, ir.Imm32(0), result_carry.carry);
    }

    ir.SetRegister(d, result);
    return true;
}

// SMMLS{R}<c> <Rd>, <Rn>, <Rm>, <Ra>
bool arm_SMMLS(Cond cond, Reg d, Reg a, Reg m, bool R, Reg n) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC || a == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto n64 = ir.SignExtendWordToLong(ir.GetRegister(n));
    const auto m64 = ir.SignExtendWordToLong(ir.GetRegister(m));
    const auto a64 = ir.Pack2x32To1x64(ir.Imm32(0), ir.GetRegister(a));
    const auto temp = ir.Sub(a64, ir.Mul(n64, m64));
    const auto result_carry = ir.MostSignificantWord(temp);
    auto result = result_carry.result;
    if (R) {
        result = ir.AddWithCarry(result, ir.Imm32(0), result_carry.carry);
    }

    ir.SetRegister(d, result);
    return true;
}

// SMMUL{R}<c> <Rd>, <Rn>, <Rm>
bool arm_SMMUL(Cond cond, Reg d, Reg m, bool R, Reg n) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto n64 = ir.SignExtendWordToLong(ir.GetRegister(n));
    const auto m64 = ir.SignExtendWordToLong(ir.GetRegister(m));
    const auto product = ir.Mul(n64, m64);
    const auto result_carry = ir.MostSignificantWord(product);
    auto result = result_carry.result;
    if (R) {
        result = ir.AddWithCarry(result, ir.Imm32(0), result_carry.carry);
    }

    ir.SetRegister(d, result);
    return true;
}

// SMLAD{X}<c> <Rd>, <Rn>, <Rm>, <Ra>
bool arm_SMLAD(Cond cond, Reg d, Reg a, Reg m, bool M, Reg n) {
    Value arithmetic_overflow;
    if (a == Reg::PC) {
        return arm_SMUAD(cond, d, m, M, n);
    }

    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Value n32 = ir.GetRegister(n);
    const Value m32 = ir.GetRegister(m);
    const Value n_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(n32));
    const Value n_hi = ir.ArithmeticShiftRight(n32, ir.Imm8(16), ir.Imm1(0)).result;

    Value m_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32));
    Value m_hi = ir.ArithmeticShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result;
    if (M) {
        std::swap(m_lo, m_hi);
    }

    const Value product_lo = ir.Mul(n_lo, m_lo);
    const Value product_hi = ir.Mul(n_hi, m_hi);
    const Value addend = ir.GetRegister(a);

    auto result = ir.AddWithOverflow(product_lo, product_hi, ir.Imm1(0), arithmetic_overflow);
    ir.OrQFlag(arithmetic_overflow);
    result = ir.AddWithOverflow(result, addend, ir.Imm1(0), arithmetic_overflow);
    ir.SetRegister(d, result);
    ir.OrQFlag(arithmetic_overflow);
    return true;
}

// SMLALD{X}<c> <RdLo>, <RdHi>, <Rn>, <Rm>
bool arm_SMLALD(Cond cond, Reg dHi, Reg dLo, Reg m, bool M, Reg n) {
    if (dLo == Reg::PC || dHi == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (dLo == dHi) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Value n32 = ir.GetRegister(n);
    const Value m32 = ir.GetRegister(m);
    const Value n_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(n32));
    const Value n_hi = ir.ArithmeticShiftRight(n32, ir.Imm8(16), ir.Imm1(0)).result;

    Value m_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32));
    Value m_hi = ir.ArithmeticShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result;
    if (M) {
        std::swap(m_lo, m_hi);
    }

    const Value product_lo = ir.SignExtendWordToLong(ir.Mul(n_lo, m_lo));
    const Value product_hi = ir.SignExtendWordToLong(ir.Mul(n_hi, m_hi));
    const auto addend = ir.Pack2x32To1x64(ir.GetRegister(dLo), ir.GetRegister(dHi));
    const auto result = ir.Add(ir.Add(product_lo, product_hi), addend);

    ir.SetRegister(dLo, ir.LeastSignificantWord(result));
    ir.SetRegister(dHi, ir.MostSignificantWord(result).result);
    return true;
}

// SMLSD{X}<c> <Rd>, <Rn>, <Rm>, <Ra>
bool arm_SMLSD(Cond cond, Reg d, Reg a, Reg m, bool M, Reg n) {
    Value arithmetic_overflow;
    if (a == Reg::PC) {
        return arm_SMUSD(cond, d, m, M, n);
    }

    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Value n32 = ir.GetRegister(n);
    const Value m32 = ir.GetRegister(m);
    const Value n_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(n32));
    const Value n_hi = ir.ArithmeticShiftRight(n32, ir.Imm8(16), ir.Imm1(0)).result;

    Value m_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32));
    Value m_hi = ir.ArithmeticShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result;
    if (M) {
        std::swap(m_lo, m_hi);
    }

    const Value product_lo = ir.Mul(n_lo, m_lo);
    const Value product_hi = ir.Mul(n_hi, m_hi);
    const Value addend = ir.GetRegister(a);
    const Value product = ir.Sub(product_lo, product_hi);
    auto result = ir.AddWithOverflow(product, addend, ir.Imm1(0), arithmetic_overflow);

    ir.SetRegister(d, result);
    ir.OrQFlag(arithmetic_overflow);
    return true;
}

// SMLSLD{X}<c> <RdLo>, <RdHi>, <Rn>, <Rm>
bool arm_SMLSLD(Cond cond, Reg dHi, Reg dLo, Reg m, bool M, Reg n) {
    if (dLo == Reg::PC || dHi == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (dLo == dHi) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Value n32 = ir.GetRegister(n);
    const Value m32 = ir.GetRegister(m);
    const Value n_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(n32));
    const Value n_hi = ir.ArithmeticShiftRight(n32, ir.Imm8(16), ir.Imm1(0)).result;

    Value m_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32));
    Value m_hi = ir.ArithmeticShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result;
    if (M) {
        std::swap(m_lo, m_hi);
    }

    const Value product_lo = ir.SignExtendWordToLong(ir.Mul(n_lo, m_lo));
    const Value product_hi = ir.SignExtendWordToLong(ir.Mul(n_hi, m_hi));
    const auto addend = ir.Pack2x32To1x64(ir.GetRegister(dLo), ir.GetRegister(dHi));
    const auto result = ir.Add(ir.Sub(product_lo, product_hi), addend);

    ir.SetRegister(dLo, ir.LeastSignificantWord(result));
    ir.SetRegister(dHi, ir.MostSignificantWord(result).result);
    return true;
}

// SMUAD{X}<c> <Rd>, <Rn>, <Rm>
bool arm_SMUAD(Cond cond, Reg d, Reg m, bool M, Reg n) {
    Value arithmetic_overflow;
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Value n32 = ir.GetRegister(n);
    const Value m32 = ir.GetRegister(m);
    const Value n_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(n32));
    const Value n_hi = ir.ArithmeticShiftRight(n32, ir.Imm8(16), ir.Imm1(0)).result;

    Value m_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32));
    Value m_hi = ir.ArithmeticShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result;
    if (M) {
        std::swap(m_lo, m_hi);
    }

    const Value product_lo = ir.Mul(n_lo, m_lo);
    const Value product_hi = ir.Mul(n_hi, m_hi);
    const auto result = ir.AddWithOverflow(product_lo, product_hi, ir.Imm1(0), arithmetic_overflow);

    ir.SetRegister(d, result);
    ir.OrQFlag(arithmetic_overflow);
    return true;
}

// SMUSD{X}<c> <Rd>, <Rn>, <Rm>
bool arm_SMUSD(Cond cond, Reg d, Reg m, bool M, Reg n) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Value n32 = ir.GetRegister(n);
    const Value m32 = ir.GetRegister(m);
    const Value n_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(n32));
    const Value n_hi = ir.ArithmeticShiftRight(n32, ir.Imm8(16), ir.Imm1(0)).result;

    Value m_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32));
    Value m_hi = ir.ArithmeticShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result;
    if (M) {
        std::swap(m_lo, m_hi);
    }

    const Value product_lo = ir.Mul(n_lo, m_lo);
    const Value product_hi = ir.Mul(n_hi, m_hi);
    auto result = ir.Sub(product_lo, product_hi);
    ir.SetRegister(d, result);
    return true;
}

// divide
/* This file is part of the dynarmic project.
 * Copyright (c) 2019 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

using divide_DivideFunction = Value (Semantics::*)(const Value&, const Value&);

bool divide_DivideOperation(
        Semantics& v, Cond cond, Reg d, Reg m, Reg n, divide_DivideFunction fn) {
    if (d == Reg::PC || m == Reg::PC || n == Reg::PC) {
        return v.UnpredictableInstruction();
    }

    if (!v.ArmConditionPassed(cond)) {
        return true;
    }

    const Value operand1 = v.ir.GetRegister(n);
    const Value operand2 = v.ir.GetRegister(m);
    const Value result = (v.ir.*fn)(operand1, operand2);

    v.ir.SetRegister(d, result);
    return true;
}

// SDIV<c> <Rd>, <Rn>, <Rm>
bool arm_SDIV(Cond cond, Reg d, Reg m, Reg n) {
    return divide_DivideOperation(*this, cond, d, m, n, &Semantics::SignedDiv);
}

// UDIV<c> <Rd>, <Rn>, <Rm>
bool arm_UDIV(Cond cond, Reg d, Reg m, Reg n) {
    return divide_DivideOperation(*this, cond, d, m, n, &Semantics::UnsignedDiv);
}

// thumb32_long_multiply
/* This file is part of the dynarmic project.
 * Copyright (c) 2021 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

using thumb32_long_multiply_DivideFunction = Value (Semantics::*)(const Value&, const Value&);

bool thumb32_long_multiply_DivideOperation(
        Semantics& v, Reg d, Reg m, Reg n, thumb32_long_multiply_DivideFunction fn) {
    if (d == Reg::PC || m == Reg::PC || n == Reg::PC) {
        return v.UnpredictableInstruction();
    }

    const Value operand1 = v.ir.GetRegister(n);
    const Value operand2 = v.ir.GetRegister(m);
    const Value result = (v.ir.*fn)(operand1, operand2);

    v.ir.SetRegister(d, result);
    return true;
}

bool thumb32_SDIV(Reg n, Reg d, Reg m) {
    return thumb32_long_multiply_DivideOperation(*this, d, m, n, &Semantics::SignedDiv);
}

bool thumb32_SMLAL(Reg n, Reg dLo, Reg dHi, Reg m) {
    if (dLo == Reg::PC || dHi == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (dHi == dLo) {
        return UnpredictableInstruction();
    }

    const auto n64 = ir.SignExtendWordToLong(ir.GetRegister(n));
    const auto m64 = ir.SignExtendWordToLong(ir.GetRegister(m));
    const auto product = ir.Mul(n64, m64);
    const auto addend = ir.Pack2x32To1x64(ir.GetRegister(dLo), ir.GetRegister(dHi));
    const auto result = ir.Add(product, addend);
    const auto lo = ir.LeastSignificantWord(result);
    const auto hi = ir.MostSignificantWord(result).result;

    ir.SetRegister(dLo, lo);
    ir.SetRegister(dHi, hi);
    return true;
}

bool thumb32_SMLALD(Reg n, Reg dLo, Reg dHi, bool M, Reg m) {
    if (dLo == Reg::PC || dHi == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (dHi == dLo) {
        return UnpredictableInstruction();
    }

    const Value n32 = ir.GetRegister(n);
    const Value m32 = ir.GetRegister(m);
    const Value n_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(n32));
    const Value n_hi = ir.ArithmeticShiftRight(n32, ir.Imm8(16), ir.Imm1(0)).result;

    Value m_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32));
    Value m_hi = ir.ArithmeticShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result;
    if (M) {
        std::swap(m_lo, m_hi);
    }

    const Value product_lo = ir.SignExtendWordToLong(ir.Mul(n_lo, m_lo));
    const Value product_hi = ir.SignExtendWordToLong(ir.Mul(n_hi, m_hi));
    const auto addend = ir.Pack2x32To1x64(ir.GetRegister(dLo), ir.GetRegister(dHi));
    const auto result = ir.Add(ir.Add(product_lo, product_hi), addend);

    ir.SetRegister(dLo, ir.LeastSignificantWord(result));
    ir.SetRegister(dHi, ir.MostSignificantWord(result).result);
    return true;
}

bool thumb32_SMLALXY(Reg n, Reg dLo, Reg dHi, bool N, bool M, Reg m) {
    if (dLo == Reg::PC || dHi == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (dHi == dLo) {
        return UnpredictableInstruction();
    }

    const Value n32 = ir.GetRegister(n);
    const Value m32 = ir.GetRegister(m);
    const Value n16 = N ? ir.ArithmeticShiftRight(n32, ir.Imm8(16), ir.Imm1(0)).result
                        : ir.SignExtendHalfToWord(ir.LeastSignificantHalf(n32));
    const Value m16 = M ? ir.ArithmeticShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result
                        : ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32));
    const Value product = ir.SignExtendWordToLong(ir.Mul(n16, m16));
    const auto addend = ir.Pack2x32To1x64(ir.GetRegister(dLo), ir.GetRegister(dHi));
    const auto result = ir.Add(product, addend);

    ir.SetRegister(dLo, ir.LeastSignificantWord(result));
    ir.SetRegister(dHi, ir.MostSignificantWord(result).result);
    return true;
}

bool thumb32_SMLSLD(Reg n, Reg dLo, Reg dHi, bool M, Reg m) {
    if (dLo == Reg::PC || dHi == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (dHi == dLo) {
        return UnpredictableInstruction();
    }

    const Value n32 = ir.GetRegister(n);
    const Value m32 = ir.GetRegister(m);
    const Value n_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(n32));
    const Value n_hi = ir.ArithmeticShiftRight(n32, ir.Imm8(16), ir.Imm1(0)).result;

    Value m_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32));
    Value m_hi = ir.ArithmeticShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result;
    if (M) {
        std::swap(m_lo, m_hi);
    }

    const Value product_lo = ir.SignExtendWordToLong(ir.Mul(n_lo, m_lo));
    const Value product_hi = ir.SignExtendWordToLong(ir.Mul(n_hi, m_hi));
    const auto addend = ir.Pack2x32To1x64(ir.GetRegister(dLo), ir.GetRegister(dHi));
    const auto result = ir.Add(ir.Sub(product_lo, product_hi), addend);

    ir.SetRegister(dLo, ir.LeastSignificantWord(result));
    ir.SetRegister(dHi, ir.MostSignificantWord(result).result);
    return true;
}

bool thumb32_SMULL(Reg n, Reg dLo, Reg dHi, Reg m) {
    if (dLo == Reg::PC || dHi == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (dHi == dLo) {
        return UnpredictableInstruction();
    }

    const auto n64 = ir.SignExtendWordToLong(ir.GetRegister(n));
    const auto m64 = ir.SignExtendWordToLong(ir.GetRegister(m));
    const auto result = ir.Mul(n64, m64);
    const auto lo = ir.LeastSignificantWord(result);
    const auto hi = ir.MostSignificantWord(result).result;

    ir.SetRegister(dLo, lo);
    ir.SetRegister(dHi, hi);
    return true;
}

bool thumb32_UDIV(Reg n, Reg d, Reg m) {
    return thumb32_long_multiply_DivideOperation(*this, d, m, n, &Semantics::UnsignedDiv);
}

bool thumb32_UMLAL(Reg n, Reg dLo, Reg dHi, Reg m) {
    if (dLo == Reg::PC || dHi == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (dHi == dLo) {
        return UnpredictableInstruction();
    }

    const auto n64 = ir.ZeroExtendWordToLong(ir.GetRegister(n));
    const auto m64 = ir.ZeroExtendWordToLong(ir.GetRegister(m));
    const auto product = ir.Mul(n64, m64);
    const auto addend = ir.Pack2x32To1x64(ir.GetRegister(dLo), ir.GetRegister(dHi));
    const auto result = ir.Add(product, addend);
    const auto lo = ir.LeastSignificantWord(result);
    const auto hi = ir.MostSignificantWord(result).result;

    ir.SetRegister(dLo, lo);
    ir.SetRegister(dHi, hi);
    return true;
}

bool thumb32_UMULL(Reg n, Reg dLo, Reg dHi, Reg m) {
    if (dLo == Reg::PC || dHi == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (dHi == dLo) {
        return UnpredictableInstruction();
    }

    const auto n64 = ir.ZeroExtendWordToLong(ir.GetRegister(n));
    const auto m64 = ir.ZeroExtendWordToLong(ir.GetRegister(m));
    const auto result = ir.Mul(n64, m64);
    const auto lo = ir.LeastSignificantWord(result);
    const auto hi = ir.MostSignificantWord(result).result;

    ir.SetRegister(dLo, lo);
    ir.SetRegister(dHi, hi);
    return true;
}

bool thumb32_UMAAL(Reg n, Reg dLo, Reg dHi, Reg m) {
    if (dLo == Reg::PC || dHi == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (dHi == dLo) {
        return UnpredictableInstruction();
    }

    const auto lo64 = ir.ZeroExtendWordToLong(ir.GetRegister(dLo));
    const auto hi64 = ir.ZeroExtendWordToLong(ir.GetRegister(dHi));
    const auto n64 = ir.ZeroExtendWordToLong(ir.GetRegister(n));
    const auto m64 = ir.ZeroExtendWordToLong(ir.GetRegister(m));
    const auto result = ir.Add(ir.Add(ir.Mul(n64, m64), hi64), lo64);

    ir.SetRegister(dLo, ir.LeastSignificantWord(result));
    ir.SetRegister(dHi, ir.MostSignificantWord(result).result);
    return true;
}

// thumb32_multiply
/* This file is part of the dynarmic project.
 * Copyright (c) 2021 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

bool thumb32_MLA(Reg n, Reg a, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC || a == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_a = ir.GetRegister(a);
    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.Add(ir.Mul(reg_n, reg_m), reg_a);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_MLS(Reg n, Reg a, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC || a == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_a = ir.GetRegister(a);
    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.Sub(reg_a, ir.Mul(reg_n, reg_m));

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_MUL(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.Mul(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_SMLAD(Reg n, Reg a, Reg d, bool X, Reg m) {
    Value arithmetic_overflow;
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC || a == Reg::PC) {
        return UnpredictableInstruction();
    }

    const Value n32 = ir.GetRegister(n);
    const Value m32 = ir.GetRegister(m);
    const Value n_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(n32));
    const Value n_hi = ir.ArithmeticShiftRight(n32, ir.Imm8(16), ir.Imm1(0)).result;

    Value m_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32));
    Value m_hi = ir.ArithmeticShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result;
    if (X) {
        std::swap(m_lo, m_hi);
    }

    const Value product_lo = ir.Mul(n_lo, m_lo);
    const Value product_hi = ir.Mul(n_hi, m_hi);
    const Value addend = ir.GetRegister(a);

    auto result = ir.AddWithOverflow(product_lo, product_hi, ir.Imm1(0), arithmetic_overflow);
    ir.OrQFlag(arithmetic_overflow);
    result = ir.AddWithOverflow(result, addend, ir.Imm1(0), arithmetic_overflow);

    ir.SetRegister(d, result);
    ir.OrQFlag(arithmetic_overflow);
    return true;
}

bool thumb32_SMLSD(Reg n, Reg a, Reg d, bool X, Reg m) {
    Value arithmetic_overflow;
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC || a == Reg::PC) {
        return UnpredictableInstruction();
    }

    const Value n32 = ir.GetRegister(n);
    const Value m32 = ir.GetRegister(m);
    const Value n_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(n32));
    const Value n_hi = ir.ArithmeticShiftRight(n32, ir.Imm8(16), ir.Imm1(0)).result;

    Value m_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32));
    Value m_hi = ir.ArithmeticShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result;
    if (X) {
        std::swap(m_lo, m_hi);
    }

    const Value product_lo = ir.Mul(n_lo, m_lo);
    const Value product_hi = ir.Mul(n_hi, m_hi);
    const Value addend = ir.GetRegister(a);
    const Value product = ir.Sub(product_lo, product_hi);
    auto result = ir.AddWithOverflow(product, addend, ir.Imm1(0), arithmetic_overflow);

    ir.SetRegister(d, result);
    ir.OrQFlag(arithmetic_overflow);
    return true;
}

bool thumb32_SMLAXY(Reg n, Reg a, Reg d, bool N, bool M, Reg m) {
    Value arithmetic_overflow;
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC || a == Reg::PC) {
        return UnpredictableInstruction();
    }

    const Value n32 = ir.GetRegister(n);
    const Value m32 = ir.GetRegister(m);
    const Value n16 = N ? ir.ArithmeticShiftRight(n32, ir.Imm8(16), ir.Imm1(0)).result
                        : ir.SignExtendHalfToWord(ir.LeastSignificantHalf(n32));
    const Value m16 = M ? ir.ArithmeticShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result
                        : ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32));
    const Value product = ir.Mul(n16, m16);
    const auto result =
            ir.AddWithOverflow(product, ir.GetRegister(a), ir.Imm1(0), arithmetic_overflow);

    ir.SetRegister(d, result);
    ir.OrQFlag(arithmetic_overflow);
    return true;
}

bool thumb32_SMMLA(Reg n, Reg a, Reg d, bool R, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC || a == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto n64 = ir.SignExtendWordToLong(ir.GetRegister(n));
    const auto m64 = ir.SignExtendWordToLong(ir.GetRegister(m));
    const auto a64 = ir.Pack2x32To1x64(ir.Imm32(0), ir.GetRegister(a));
    const auto temp = ir.Add(a64, ir.Mul(n64, m64));
    const auto result_carry = ir.MostSignificantWord(temp);
    auto result = result_carry.result;
    if (R) {
        result = ir.AddWithCarry(result, ir.Imm32(0), result_carry.carry);
    }

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_SMMLS(Reg n, Reg a, Reg d, bool R, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC || a == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto n64 = ir.SignExtendWordToLong(ir.GetRegister(n));
    const auto m64 = ir.SignExtendWordToLong(ir.GetRegister(m));
    const auto a64 = ir.Pack2x32To1x64(ir.Imm32(0), ir.GetRegister(a));
    const auto temp = ir.Sub(a64, ir.Mul(n64, m64));
    const auto result_carry = ir.MostSignificantWord(temp);
    auto result = result_carry.result;
    if (R) {
        result = ir.AddWithCarry(result, ir.Imm32(0), result_carry.carry);
    }

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_SMMUL(Reg n, Reg d, bool R, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto n64 = ir.SignExtendWordToLong(ir.GetRegister(n));
    const auto m64 = ir.SignExtendWordToLong(ir.GetRegister(m));
    const auto product = ir.Mul(n64, m64);
    const auto result_carry = ir.MostSignificantWord(product);
    auto result = result_carry.result;
    if (R) {
        result = ir.AddWithCarry(result, ir.Imm32(0), result_carry.carry);
    }

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_SMUAD(Reg n, Reg d, bool M, Reg m) {
    Value arithmetic_overflow;
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const Value n32 = ir.GetRegister(n);
    const Value m32 = ir.GetRegister(m);
    const Value n_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(n32));
    const Value n_hi = ir.ArithmeticShiftRight(n32, ir.Imm8(16), ir.Imm1(0)).result;

    Value m_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32));
    Value m_hi = ir.ArithmeticShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result;
    if (M) {
        std::swap(m_lo, m_hi);
    }

    const Value product_lo = ir.Mul(n_lo, m_lo);
    const Value product_hi = ir.Mul(n_hi, m_hi);
    const auto result = ir.AddWithOverflow(product_lo, product_hi, ir.Imm1(0), arithmetic_overflow);

    ir.SetRegister(d, result);
    ir.OrQFlag(arithmetic_overflow);
    return true;
}

bool thumb32_SMUSD(Reg n, Reg d, bool M, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const Value n32 = ir.GetRegister(n);
    const Value m32 = ir.GetRegister(m);
    const Value n_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(n32));
    const Value n_hi = ir.ArithmeticShiftRight(n32, ir.Imm8(16), ir.Imm1(0)).result;

    Value m_lo = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32));
    Value m_hi = ir.ArithmeticShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result;
    if (M) {
        std::swap(m_lo, m_hi);
    }

    const Value product_lo = ir.Mul(n_lo, m_lo);
    const Value product_hi = ir.Mul(n_hi, m_hi);
    const Value result = ir.Sub(product_lo, product_hi);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_SMULXY(Reg n, Reg d, bool N, bool M, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto n32 = ir.GetRegister(n);
    const auto m32 = ir.GetRegister(m);
    const auto n16 = N ? ir.ArithmeticShiftRight(n32, ir.Imm8(16), ir.Imm1(0)).result
                       : ir.SignExtendHalfToWord(ir.LeastSignificantHalf(n32));
    const auto m16 = M ? ir.ArithmeticShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result
                       : ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32));
    const auto result = ir.Mul(n16, m16);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_SMLAWY(Reg n, Reg a, Reg d, bool M, Reg m) {
    Value arithmetic_overflow;
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC || a == Reg::PC) {
        return UnpredictableInstruction();
    }

    const Value n32 = ir.SignExtendWordToLong(ir.GetRegister(n));
    Value m32 = ir.GetRegister(m);
    if (M) {
        m32 = ir.LogicalShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result;
    }
    const Value m16 =
            ir.SignExtendWordToLong(ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32)));
    const auto product =
            ir.LeastSignificantWord(ir.LogicalShiftRight(ir.Mul(n32, m16), ir.Imm8(16)));
    const auto result =
            ir.AddWithOverflow(product, ir.GetRegister(a), ir.Imm1(0), arithmetic_overflow);

    ir.SetRegister(d, result);
    ir.OrQFlag(arithmetic_overflow);
    return true;
}

bool thumb32_SMULWY(Reg n, Reg d, bool M, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const Value n32 = ir.SignExtendWordToLong(ir.GetRegister(n));
    Value m32 = ir.GetRegister(m);
    if (M) {
        m32 = ir.LogicalShiftRight(m32, ir.Imm8(16), ir.Imm1(0)).result;
    }
    const Value m16 =
            ir.SignExtendWordToLong(ir.SignExtendHalfToWord(ir.LeastSignificantHalf(m32)));
    const auto result = ir.LogicalShiftRight(ir.Mul(n32, m16), ir.Imm8(16));

    ir.SetRegister(d, ir.LeastSignificantWord(result));
    return true;
}

bool thumb32_USAD8(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedAbsDiffSumU8(reg_n, reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_USADA8(Reg n, Reg a, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC || a == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_a = ir.GetRegister(a);
    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto tmp = ir.PackedAbsDiffSumU8(reg_n, reg_m);
    const auto result = ir.AddWithCarry(reg_a, tmp, ir.Imm1(0));

    ir.SetRegister(d, result);
    return true;
}
