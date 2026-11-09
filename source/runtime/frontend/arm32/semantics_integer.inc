Value UnsignedDiv(const Value& a, const Value& b) {
    const auto zero = TestZero(b);
    return Select(zero, Imm32(0), Emit<OpCode::Div>(a, Select(zero, Imm32(1), b)));
}

Value SignedDiv(const Value& a, const Value& b) {
    const auto zero = TestZero(b);
    const auto left = SignExtendWordToLong(a),
               right = SignExtendWordToLong(Select(zero, Imm32(1), b));
    return Select(zero, Imm32(0), Emit<OpCode::SignedDiv64>(left, right).WithView(Type::U32));
}

Value CRC32ISO(Value crc, Value data, unsigned bits) {
    crc = Xor(crc, data.WithView(bits == 8 ? Type::U8 : bits == 16 ? Type::U16 : Type::U32));
    for (unsigned bit = 0; bit < bits; ++bit)
        crc = Xor(LsrImm(crc, Literal{1u}),
                  Select(TestBit(crc, Literal{0u}), Imm32(0xedb88320), Imm32(0)));
    return crc;
}

Value CRC32ISO8(Value crc, Value data) { return CRC32ISO(crc, ZeroExtendByteToWord(data), 8); }

Value CRC32ISO16(Value crc, Value data) { return CRC32ISO(crc, ZeroExtendHalfToWord(data), 16); }

Value CRC32ISO32(Value crc, Value data) { return CRC32ISO(crc, data, 32); }

Value CRC32Castagnoli8(Value crc, Value data) {
    return Emit<OpCode::Crc32c>(crc, data, Literal{8u});
}

Value CRC32Castagnoli16(Value crc, Value data) {
    return Emit<OpCode::Crc32c>(crc, data, Literal{16u});
}

Value CRC32Castagnoli32(Value crc, Value data) {
    return Emit<OpCode::Crc32c>(crc, data, Literal{32u});
}

// Imported A32 instruction semantics.
// Adapted from dynarmic-interp 5a76f693 (MerryMage and contributors), SPDX-License-Identifier:
// 0BSD.

// data_processing
/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

// ADC{S}<c> <Rd>, <Rn>, #<imm>
bool arm_ADC_imm(Cond cond, bool S, Reg n, Reg d, int rotate, Imm<8> imm8) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = ArmExpandImm(rotate, imm8);
    const auto result = ir.AddWithCarry(ir.GetRegister(n), ir.Imm32(imm32), ir.GetCFlag());
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

// ADC{S}<c> <Rd>, <Rn>, <Rm>{, <shift>}
bool arm_ADC_reg(Cond cond, bool S, Reg n, Reg d, Imm<5> imm5, ShiftType shift, Reg m) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), shift, imm5, ir.GetCFlag());
    const auto result = ir.AddWithCarry(ir.GetRegister(n), shifted.result, ir.GetCFlag());
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }

    return true;
}

// ADC{S}<c> <Rd>, <Rn>, <Rm>, <type> <Rs>
bool arm_ADC_rsr(Cond cond, bool S, Reg n, Reg d, Reg s, ShiftType shift, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC || s == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(s));
    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitRegShift(ir.GetRegister(m), shift, shift_n, carry_in);
    const auto result = ir.AddWithCarry(ir.GetRegister(n), shifted.result, ir.GetCFlag());

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }

    return true;
}

// ADD{S}<c> <Rd>, <Rn>, #<const>
bool arm_ADD_imm(Cond cond, bool S, Reg n, Reg d, int rotate, Imm<8> imm8) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = ArmExpandImm(rotate, imm8);
    const auto result = ir.AddWithCarry(ir.GetRegister(n), ir.Imm32(imm32), ir.Imm1(0));
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }

    return true;
}

// ADD{S}<c> <Rd>, <Rn>, <Rm>{, <shift>}
bool arm_ADD_reg(Cond cond, bool S, Reg n, Reg d, Imm<5> imm5, ShiftType shift, Reg m) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), shift, imm5, ir.GetCFlag());
    const auto result = ir.AddWithCarry(ir.GetRegister(n), shifted.result, ir.Imm1(0));
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }

    return true;
}

// ADD{S}<c> <Rd>, <Rn>, <Rm>, <type> <Rs>
bool arm_ADD_rsr(Cond cond, bool S, Reg n, Reg d, Reg s, ShiftType shift, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC || s == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(s));
    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitRegShift(ir.GetRegister(m), shift, shift_n, carry_in);
    const auto result = ir.AddWithCarry(ir.GetRegister(n), shifted.result, ir.Imm1(0));

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }

    return true;
}

// AND{S}<c> <Rd>, <Rn>, #<const>
bool arm_AND_imm(Cond cond, bool S, Reg n, Reg d, int rotate, Imm<8> imm8) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto imm_carry = ArmExpandImm_C(rotate, imm8, ir.GetCFlag());
    const auto result = ir.And(ir.GetRegister(n), ir.Imm32(imm_carry.imm32));
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), imm_carry.carry);
    }

    return true;
}

// AND{S}<c> <Rd>, <Rn>, <Rm>{, <shift>}
bool arm_AND_reg(Cond cond, bool S, Reg n, Reg d, Imm<5> imm5, ShiftType shift, Reg m) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitImmShift(ir.GetRegister(m), shift, imm5, carry_in);
    const auto result = ir.And(ir.GetRegister(n), shifted.result);
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }
    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }

    return true;
}

// AND{S}<c> <Rd>, <Rn>, <Rm>, <type> <Rs>
bool arm_AND_rsr(Cond cond, bool S, Reg n, Reg d, Reg s, ShiftType shift, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC || s == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(s));
    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitRegShift(ir.GetRegister(m), shift, shift_n, carry_in);
    const auto result = ir.And(ir.GetRegister(n), shifted.result);

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }

    return true;
}

// BIC{S}<c> <Rd>, <Rn>, #<const>
bool arm_BIC_imm(Cond cond, bool S, Reg n, Reg d, int rotate, Imm<8> imm8) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto imm_carry = ArmExpandImm_C(rotate, imm8, ir.GetCFlag());
    const auto result = ir.AndNot(ir.GetRegister(n), ir.Imm32(imm_carry.imm32));
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), imm_carry.carry);
    }

    return true;
}

// BIC{S}<c> <Rd>, <Rn>, <Rm>{, <shift>}
bool arm_BIC_reg(Cond cond, bool S, Reg n, Reg d, Imm<5> imm5, ShiftType shift, Reg m) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitImmShift(ir.GetRegister(m), shift, imm5, carry_in);
    const auto result = ir.AndNot(ir.GetRegister(n), shifted.result);
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }

    return true;
}

// BIC{S}<c> <Rd>, <Rn>, <Rm>, <type> <Rs>
bool arm_BIC_rsr(Cond cond, bool S, Reg n, Reg d, Reg s, ShiftType shift, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC || s == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(s));
    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitRegShift(ir.GetRegister(m), shift, shift_n, carry_in);
    const auto result = ir.AndNot(ir.GetRegister(n), shifted.result);

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }

    return true;
}

// CMN<c> <Rn>, #<const>
bool arm_CMN_imm(Cond cond, Reg n, int rotate, Imm<8> imm8) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = ArmExpandImm(rotate, imm8);
    const auto result = ir.AddWithCarry(ir.GetRegister(n), ir.Imm32(imm32), ir.Imm1(0));

    ir.SetCpsrNZCV(ir.NZCVFrom(result));
    return true;
}

// CMN<c> <Rn>, <Rm>{, <shift>}
bool arm_CMN_reg(Cond cond, Reg n, Imm<5> imm5, ShiftType shift, Reg m) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), shift, imm5, ir.GetCFlag());
    const auto result = ir.AddWithCarry(ir.GetRegister(n), shifted.result, ir.Imm1(0));

    ir.SetCpsrNZCV(ir.NZCVFrom(result));
    return true;
}

// CMN<c> <Rn>, <Rm>, <type> <Rs>
bool arm_CMN_rsr(Cond cond, Reg n, Reg s, ShiftType shift, Reg m) {
    if (n == Reg::PC || m == Reg::PC || s == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(s));
    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitRegShift(ir.GetRegister(m), shift, shift_n, carry_in);
    const auto result = ir.AddWithCarry(ir.GetRegister(n), shifted.result, ir.Imm1(0));

    ir.SetCpsrNZCV(ir.NZCVFrom(result));
    return true;
}

// CMP<c> <Rn>, #<imm>
bool arm_CMP_imm(Cond cond, Reg n, int rotate, Imm<8> imm8) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = ArmExpandImm(rotate, imm8);
    const auto result = ir.SubWithCarry(ir.GetRegister(n), ir.Imm32(imm32), ir.Imm1(1));

    ir.SetCpsrNZCV(ir.NZCVFrom(result));
    return true;
}

// CMP<c> <Rn>, <Rm>{, <shift>}
bool arm_CMP_reg(Cond cond, Reg n, Imm<5> imm5, ShiftType shift, Reg m) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), shift, imm5, ir.GetCFlag());
    const auto result = ir.SubWithCarry(ir.GetRegister(n), shifted.result, ir.Imm1(1));

    ir.SetCpsrNZCV(ir.NZCVFrom(result));
    return true;
}

// CMP<c> <Rn>, <Rm>, <type> <Rs>
bool arm_CMP_rsr(Cond cond, Reg n, Reg s, ShiftType shift, Reg m) {
    if (n == Reg::PC || m == Reg::PC || s == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(s));
    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitRegShift(ir.GetRegister(m), shift, shift_n, carry_in);
    const auto result = ir.SubWithCarry(ir.GetRegister(n), shifted.result, ir.Imm1(1));

    ir.SetCpsrNZCV(ir.NZCVFrom(result));
    return true;
}

// EOR{S}<c> <Rd>, <Rn>, #<const>
bool arm_EOR_imm(Cond cond, bool S, Reg n, Reg d, int rotate, Imm<8> imm8) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto imm_carry = ArmExpandImm_C(rotate, imm8, ir.GetCFlag());
    const auto result = ir.Eor(ir.GetRegister(n), ir.Imm32(imm_carry.imm32));
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), imm_carry.carry);
    }

    return true;
}

// EOR{S}<c> <Rd>, <Rn>, <Rm>{, <shift>}
bool arm_EOR_reg(Cond cond, bool S, Reg n, Reg d, Imm<5> imm5, ShiftType shift, Reg m) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitImmShift(ir.GetRegister(m), shift, imm5, carry_in);
    const auto result = ir.Eor(ir.GetRegister(n), shifted.result);
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }

    return true;
}

// EOR{S}<c> <Rd>, <Rn>, <Rm>, <type> <Rs>
bool arm_EOR_rsr(Cond cond, bool S, Reg n, Reg d, Reg s, ShiftType shift, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC || s == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(s));
    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitRegShift(ir.GetRegister(m), shift, shift_n, carry_in);
    const auto result = ir.Eor(ir.GetRegister(n), shifted.result);

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }

    return true;
}

// MOV{S}<c> <Rd>, #<const>
bool arm_MOV_imm(Cond cond, bool S, Reg d, int rotate, Imm<8> imm8) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto imm_carry = ArmExpandImm_C(rotate, imm8, ir.GetCFlag());
    const auto result = ir.Imm32(imm_carry.imm32);
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), imm_carry.carry);
    }

    return true;
}

// MOV{S}<c> <Rd>, <Rm>
bool arm_MOV_reg(Cond cond, bool S, Reg d, Imm<5> imm5, ShiftType shift, Reg m) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitImmShift(ir.GetRegister(m), shift, imm5, carry_in);
    const auto result = shifted.result;
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }

    return true;
}

bool arm_MOV_rsr(Cond cond, bool S, Reg d, Reg s, ShiftType shift, Reg m) {
    if (d == Reg::PC || m == Reg::PC || s == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(s));
    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitRegShift(ir.GetRegister(m), shift, shift_n, carry_in);
    const auto result = shifted.result;
    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }

    return true;
}

// MVN{S}<c> <Rd>, #<const>
bool arm_MVN_imm(Cond cond, bool S, Reg d, int rotate, Imm<8> imm8) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto imm_carry = ArmExpandImm_C(rotate, imm8, ir.GetCFlag());
    const auto result = ir.Not(ir.Imm32(imm_carry.imm32));
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), imm_carry.carry);
    }

    return true;
}

// MVN{S}<c> <Rd>, <Rm>{, <shift>}
bool arm_MVN_reg(Cond cond, bool S, Reg d, Imm<5> imm5, ShiftType shift, Reg m) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitImmShift(ir.GetRegister(m), shift, imm5, carry_in);
    const auto result = ir.Not(shifted.result);
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }

    return true;
}

// MVN{S}<c> <Rd>, <Rm>, <type> <Rs>
bool arm_MVN_rsr(Cond cond, bool S, Reg d, Reg s, ShiftType shift, Reg m) {
    if (d == Reg::PC || m == Reg::PC || s == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(s));
    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitRegShift(ir.GetRegister(m), shift, shift_n, carry_in);
    const auto result = ir.Not(shifted.result);

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }

    return true;
}

// ORR{S}<c> <Rd>, <Rn>, #<const>
bool arm_ORR_imm(Cond cond, bool S, Reg n, Reg d, int rotate, Imm<8> imm8) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto imm_carry = ArmExpandImm_C(rotate, imm8, ir.GetCFlag());
    const auto result = ir.Or(ir.GetRegister(n), ir.Imm32(imm_carry.imm32));
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), imm_carry.carry);
    }

    return true;
}

// ORR{S}<c> <Rd>, <Rn>, <Rm>{, <shift>}
bool arm_ORR_reg(Cond cond, bool S, Reg n, Reg d, Imm<5> imm5, ShiftType shift, Reg m) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitImmShift(ir.GetRegister(m), shift, imm5, carry_in);
    const auto result = ir.Or(ir.GetRegister(n), shifted.result);
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }

    return true;
}

// ORR{S}<c> <Rd>, <Rn>, <Rm>, <type> <Rs>
bool arm_ORR_rsr(Cond cond, bool S, Reg n, Reg d, Reg s, ShiftType shift, Reg m) {
    if (n == Reg::PC || m == Reg::PC || s == Reg::PC || d == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(s));
    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitRegShift(ir.GetRegister(m), shift, shift_n, carry_in);
    const auto result = ir.Or(ir.GetRegister(n), shifted.result);

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }

    return true;
}

// RSB{S}<c> <Rd>, <Rn>, #<const>
bool arm_RSB_imm(Cond cond, bool S, Reg n, Reg d, int rotate, Imm<8> imm8) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = ArmExpandImm(rotate, imm8);
    const auto result = ir.SubWithCarry(ir.Imm32(imm32), ir.GetRegister(n), ir.Imm1(1));
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }

    return true;
}

// RSB{S}<c> <Rd>, <Rn>, <Rm>{, <shift>}
bool arm_RSB_reg(Cond cond, bool S, Reg n, Reg d, Imm<5> imm5, ShiftType shift, Reg m) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), shift, imm5, ir.GetCFlag());
    const auto result = ir.SubWithCarry(shifted.result, ir.GetRegister(n), ir.Imm1(1));
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }

    return true;
}

// RSB{S}<c> <Rd>, <Rn>, <Rm>, <type> <Rs>
bool arm_RSB_rsr(Cond cond, bool S, Reg n, Reg d, Reg s, ShiftType shift, Reg m) {
    if (n == Reg::PC || m == Reg::PC || s == Reg::PC || d == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(s));
    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitRegShift(ir.GetRegister(m), shift, shift_n, carry_in);
    const auto result = ir.SubWithCarry(shifted.result, ir.GetRegister(n), ir.Imm1(1));

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }

    return true;
}

// RSC{S}<c> <Rd>, <Rn>, #<const>
bool arm_RSC_imm(Cond cond, bool S, Reg n, Reg d, int rotate, Imm<8> imm8) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = ArmExpandImm(rotate, imm8);
    const auto result = ir.SubWithCarry(ir.Imm32(imm32), ir.GetRegister(n), ir.GetCFlag());
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }

    return true;
}

bool arm_RSC_reg(Cond cond, bool S, Reg n, Reg d, Imm<5> imm5, ShiftType shift, Reg m) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), shift, imm5, ir.GetCFlag());
    const auto result = ir.SubWithCarry(shifted.result, ir.GetRegister(n), ir.GetCFlag());
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }

    return true;
}

// RSC{S}<c> <Rd>, <Rn>, <Rm>{, <shift>}
bool arm_RSC_rsr(Cond cond, bool S, Reg n, Reg d, Reg s, ShiftType shift, Reg m) {
    if (n == Reg::PC || m == Reg::PC || s == Reg::PC || d == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(s));
    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitRegShift(ir.GetRegister(m), shift, shift_n, carry_in);
    const auto result = ir.SubWithCarry(shifted.result, ir.GetRegister(n), ir.GetCFlag());

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }

    return true;
}

// SBC{S}<c> <Rd>, <Rn>, #<const>
bool arm_SBC_imm(Cond cond, bool S, Reg n, Reg d, int rotate, Imm<8> imm8) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = ArmExpandImm(rotate, imm8);
    const auto result = ir.SubWithCarry(ir.GetRegister(n), ir.Imm32(imm32), ir.GetCFlag());
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }

    return true;
}

// SBC{S}<c> <Rd>, <Rn>, <Rm>{, <shift>}
bool arm_SBC_reg(Cond cond, bool S, Reg n, Reg d, Imm<5> imm5, ShiftType shift, Reg m) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), shift, imm5, ir.GetCFlag());
    const auto result = ir.SubWithCarry(ir.GetRegister(n), shifted.result, ir.GetCFlag());
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }

    return true;
}

// SBC{S}<c> <Rd>, <Rn>, <Rm>, <type> <Rs>
bool arm_SBC_rsr(Cond cond, bool S, Reg n, Reg d, Reg s, ShiftType shift, Reg m) {
    if (n == Reg::PC || m == Reg::PC || s == Reg::PC || d == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(s));
    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitRegShift(ir.GetRegister(m), shift, shift_n, carry_in);
    const auto result = ir.SubWithCarry(ir.GetRegister(n), shifted.result, ir.GetCFlag());

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }

    return true;
}

// SUB{S}<c> <Rd>, <Rn>, #<const>
bool arm_SUB_imm(Cond cond, bool S, Reg n, Reg d, int rotate, Imm<8> imm8) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = ArmExpandImm(rotate, imm8);
    const auto result = ir.SubWithCarry(ir.GetRegister(n), ir.Imm32(imm32), ir.Imm1(1));
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }

    return true;
}

// SUB{S}<c> <Rd>, <Rn>, <Rm>{, <shift>}
bool arm_SUB_reg(Cond cond, bool S, Reg n, Reg d, Imm<5> imm5, ShiftType shift, Reg m) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), shift, imm5, ir.GetCFlag());
    const auto result = ir.SubWithCarry(ir.GetRegister(n), shifted.result, ir.Imm1(1));
    if (d == Reg::PC) {
        if (S) {
            // This is UNPREDICTABLE when in user-mode.
            return UnpredictableInstruction();
        }

        ir.ALUWritePC(result);
        ir.SetTerm(Term::ReturnToDispatch{});
        return false;
    }

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }

    return true;
}

bool arm_SUB_rsr(Cond cond, bool S, Reg n, Reg d, Reg s, ShiftType shift, Reg m) {
    if (n == Reg::PC || m == Reg::PC || s == Reg::PC || d == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(s));
    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitRegShift(ir.GetRegister(m), shift, shift_n, carry_in);
    const auto result = ir.SubWithCarry(ir.GetRegister(n), shifted.result, ir.Imm1(1));

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }

    return true;
}

// TEQ<c> <Rn>, #<const>
bool arm_TEQ_imm(Cond cond, Reg n, int rotate, Imm<8> imm8) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto imm_carry = ArmExpandImm_C(rotate, imm8, ir.GetCFlag());
    const auto result = ir.Eor(ir.GetRegister(n), ir.Imm32(imm_carry.imm32));

    ir.SetCpsrNZC(ir.NZFrom(result), imm_carry.carry);
    return true;
}

// TEQ<c> <Rn>, <Rm>{, <shift>}
bool arm_TEQ_reg(Cond cond, Reg n, Imm<5> imm5, ShiftType shift, Reg m) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitImmShift(ir.GetRegister(m), shift, imm5, carry_in);
    const auto result = ir.Eor(ir.GetRegister(n), shifted.result);

    ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    return true;
}

// TEQ<c> <Rn>, <Rm>, <type> <Rs>
bool arm_TEQ_rsr(Cond cond, Reg n, Reg s, ShiftType shift, Reg m) {
    if (n == Reg::PC || m == Reg::PC || s == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(s));
    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitRegShift(ir.GetRegister(m), shift, shift_n, carry_in);
    const auto result = ir.Eor(ir.GetRegister(n), shifted.result);

    ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    return true;
}

// TST<c> <Rn>, #<const>
bool arm_TST_imm(Cond cond, Reg n, int rotate, Imm<8> imm8) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto imm_carry = ArmExpandImm_C(rotate, imm8, ir.GetCFlag());
    const auto result = ir.And(ir.GetRegister(n), ir.Imm32(imm_carry.imm32));

    ir.SetCpsrNZC(ir.NZFrom(result), imm_carry.carry);
    return true;
}

// TST<c> <Rn>, <Rm>{, <shift>}
bool arm_TST_reg(Cond cond, Reg n, Imm<5> imm5, ShiftType shift, Reg m) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitImmShift(ir.GetRegister(m), shift, imm5, carry_in);
    const auto result = ir.And(ir.GetRegister(n), shifted.result);

    ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    return true;
}

// TST<c> <Rn>, <Rm>, <type> <Rs>
bool arm_TST_rsr(Cond cond, Reg n, Reg s, ShiftType shift, Reg m) {
    if (n == Reg::PC || m == Reg::PC || s == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(s));
    const auto carry_in = ir.GetCFlag();
    const auto shifted = EmitRegShift(ir.GetRegister(m), shift, shift_n, carry_in);
    const auto result = ir.And(ir.GetRegister(n), shifted.result);

    ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    return true;
}

// extension
/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

static Value extension_Rotate(Semantics& ir, Reg m, SignExtendRotation rotate) {
    const u8 rotate_by = static_cast<u8>(static_cast<size_t>(rotate) * 8);
    return ir.RotateRight(ir.GetRegister(m), ir.Imm8(rotate_by), ir.Imm1(0)).result;
}

// SXTAB<c> <Rd>, <Rn>, <Rm>{, <rotation>}
bool arm_SXTAB(Cond cond, Reg n, Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto rotated = extension_Rotate(ir, m, rotate);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.Add(reg_n, ir.SignExtendByteToWord(ir.LeastSignificantByte(rotated)));

    ir.SetRegister(d, result);
    return true;
}

// SXTAB16<c> <Rd>, <Rn>, <Rm>{, <rotation>}
bool arm_SXTAB16(Cond cond, Reg n, Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto rotated = extension_Rotate(ir, m, rotate);
    const auto low_byte = ir.And(rotated, ir.Imm32(0x00FF00FF));
    const auto sign_bit = ir.And(rotated, ir.Imm32(0x00800080));
    const auto addend = ir.Or(low_byte, ir.Mul(sign_bit, ir.Imm32(0x1FE)));
    const auto result = ir.PackedAddU16(addend, ir.GetRegister(n)).result;

    ir.SetRegister(d, result);
    return true;
}

// SXTAH<c> <Rd>, <Rn>, <Rm>{, <rotation>}
bool arm_SXTAH(Cond cond, Reg n, Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto rotated = extension_Rotate(ir, m, rotate);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.Add(reg_n, ir.SignExtendHalfToWord(ir.LeastSignificantHalf(rotated)));

    ir.SetRegister(d, result);
    return true;
}

// SXTB<c> <Rd>, <Rm>{, <rotation>}
bool arm_SXTB(Cond cond, Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto rotated = extension_Rotate(ir, m, rotate);
    const auto result = ir.SignExtendByteToWord(ir.LeastSignificantByte(rotated));

    ir.SetRegister(d, result);
    return true;
}

// SXTB16<c> <Rd>, <Rm>{, <rotation>}
bool arm_SXTB16(Cond cond, Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto rotated = extension_Rotate(ir, m, rotate);
    const auto low_byte = ir.And(rotated, ir.Imm32(0x00FF00FF));
    const auto sign_bit = ir.And(rotated, ir.Imm32(0x00800080));
    const auto result = ir.Or(low_byte, ir.Mul(sign_bit, ir.Imm32(0x1FE)));

    ir.SetRegister(d, result);
    return true;
}

// SXTH<c> <Rd>, <Rm>{, <rotation>}
bool arm_SXTH(Cond cond, Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto rotated = extension_Rotate(ir, m, rotate);
    const auto result = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(rotated));

    ir.SetRegister(d, result);
    return true;
}

// UXTAB<c> <Rd>, <Rn>, <Rm>{, <rotation>}
bool arm_UXTAB(Cond cond, Reg n, Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto rotated = extension_Rotate(ir, m, rotate);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.Add(reg_n, ir.ZeroExtendByteToWord(ir.LeastSignificantByte(rotated)));

    ir.SetRegister(d, result);
    return true;
}

// UXTAB16<c> <Rd>, <Rn>, <Rm>{, <rotation>}
bool arm_UXTAB16(Cond cond, Reg n, Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto rotated = extension_Rotate(ir, m, rotate);
    auto result = ir.And(rotated, ir.Imm32(0x00FF00FF));
    const auto reg_n = ir.GetRegister(n);
    result = ir.PackedAddU16(reg_n, result).result;

    ir.SetRegister(d, result);
    return true;
}

// UXTAH<c> <Rd>, <Rn>, <Rm>{, <rotation>}
bool arm_UXTAH(Cond cond, Reg n, Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto rotated = extension_Rotate(ir, m, rotate);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.Add(reg_n, ir.ZeroExtendHalfToWord(ir.LeastSignificantHalf(rotated)));

    ir.SetRegister(d, result);
    return true;
}

// UXTB<c> <Rd>, <Rm>{, <rotation>}
bool arm_UXTB(Cond cond, Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto rotated = extension_Rotate(ir, m, rotate);
    const auto result = ir.ZeroExtendByteToWord(ir.LeastSignificantByte(rotated));
    ir.SetRegister(d, result);
    return true;
}

// UXTB16<c> <Rd>, <Rm>{, <rotation>}
bool arm_UXTB16(Cond cond, Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto rotated = extension_Rotate(ir, m, rotate);
    const auto result = ir.And(rotated, ir.Imm32(0x00FF00FF));

    ir.SetRegister(d, result);
    return true;
}

// UXTH<c> <Rd>, <Rm>{, <rotation>}
bool arm_UXTH(Cond cond, Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto rotated = extension_Rotate(ir, m, rotate);
    const auto result = ir.ZeroExtendHalfToWord(ir.LeastSignificantHalf(rotated));

    ir.SetRegister(d, result);
    return true;
}

// packing
/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

// PKHBT<c> <Rd>, <Rn>, <Rm>{, LSL #<imm>}
bool arm_PKHBT(Cond cond, Reg n, Reg d, Imm<5> imm5, Reg m) {
    if (n == Reg::PC || d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shifted =
            EmitImmShift(ir.GetRegister(m), ShiftType::LSL, imm5, ir.Imm1(false)).result;
    const auto lower_half = ir.And(ir.GetRegister(n), ir.Imm32(0x0000FFFF));
    const auto upper_half = ir.And(shifted, ir.Imm32(0xFFFF0000));

    ir.SetRegister(d, ir.Or(lower_half, upper_half));
    return true;
}

// PKHTB<c> <Rd>, <Rn>, <Rm>{, ASR #<imm>}
bool arm_PKHTB(Cond cond, Reg n, Reg d, Imm<5> imm5, Reg m) {
    if (n == Reg::PC || d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto shifted =
            EmitImmShift(ir.GetRegister(m), ShiftType::ASR, imm5, ir.Imm1(false)).result;
    const auto lower_half = ir.And(shifted, ir.Imm32(0x0000FFFF));
    const auto upper_half = ir.And(ir.GetRegister(n), ir.Imm32(0xFFFF0000));

    ir.SetRegister(d, ir.Or(lower_half, upper_half));
    return true;
}

// misc
/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

// BFC<c> <Rd>, #<lsb>, #<width>
bool arm_BFC(Cond cond, Imm<5> msb, Reg d, Imm<5> lsb) {
    if (d == Reg::PC) {
        return UnpredictableInstruction();
    }
    if (msb < lsb) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 lsb_value = lsb.ZeroExtend();
    const u32 msb_value = msb.ZeroExtend();
    const u32 mask = ~(bits::ones<u32>(msb_value - lsb_value + 1) << lsb_value);
    const Value operand = ir.GetRegister(d);
    const Value result = ir.And(operand, ir.Imm32(mask));

    ir.SetRegister(d, result);
    return true;
}

// BFI<c> <Rd>, <Rn>, #<lsb>, #<width>
bool arm_BFI(Cond cond, Imm<5> msb, Reg d, Imm<5> lsb, Reg n) {
    if (d == Reg::PC) {
        return UnpredictableInstruction();
    }
    if (msb < lsb) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 lsb_value = lsb.ZeroExtend();
    const u32 msb_value = msb.ZeroExtend();
    const u32 inclusion_mask = bits::ones<u32>(msb_value - lsb_value + 1) << lsb_value;
    const u32 exclusion_mask = ~inclusion_mask;
    const Value operand1 = ir.And(ir.GetRegister(d), ir.Imm32(exclusion_mask));
    const Value operand2 = ir.And(ir.LogicalShiftLeft(ir.GetRegister(n), ir.Imm8(u8(lsb_value))),
                                  ir.Imm32(inclusion_mask));
    const Value result = ir.Or(operand1, operand2);

    ir.SetRegister(d, result);
    return true;
}

// CLZ<c> <Rd>, <Rm>
bool arm_CLZ(Cond cond, Reg d, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    ir.SetRegister(d, ir.CountLeadingZeros(ir.GetRegister(m)));
    return true;
}

// MOVT<c> <Rd>, #<imm16>
bool arm_MOVT(Cond cond, Imm<4> imm4, Reg d, Imm<12> imm12) {
    if (d == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Value imm16 = ir.Imm32(concatenate(imm4, imm12).ZeroExtend() << 16);
    const Value operand = ir.GetRegister(d);
    const Value result = ir.Or(ir.And(operand, ir.Imm32(0x0000FFFFU)), imm16);

    ir.SetRegister(d, result);
    return true;
}

bool arm_MOVW(Cond cond, Imm<4> imm4, Reg d, Imm<12> imm12) {
    if (d == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Value imm = ir.Imm32(concatenate(imm4, imm12).ZeroExtend());

    ir.SetRegister(d, imm);
    return true;
}

// SBFX<c> <Rd>, <Rn>, #<lsb>, #<width>
bool arm_SBFX(Cond cond, Imm<5> widthm1, Reg d, Imm<5> lsb, Reg n) {
    if (d == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const u32 lsb_value = lsb.ZeroExtend();
    const u32 widthm1_value = widthm1.ZeroExtend();
    const u32 msb = lsb_value + widthm1_value;
    if (msb >= (sizeof(u32) * 8)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    constexpr size_t max_width = (sizeof(u32) * 8);
    const u32 width = widthm1_value + 1;
    const u8 left_shift_amount = static_cast<u8>(max_width - width - lsb_value);
    const u8 right_shift_amount = static_cast<u8>(max_width - width);
    const Value operand = ir.GetRegister(n);
    const Value tmp = ir.LogicalShiftLeft(operand, ir.Imm8(left_shift_amount));
    const Value result = ir.ArithmeticShiftRight(tmp, ir.Imm8(right_shift_amount));

    ir.SetRegister(d, result);
    return true;
}

// SEL<c> <Rd>, <Rn>, <Rm>
bool arm_SEL(Cond cond, Reg n, Reg d, Reg m) {
    if (n == Reg::PC || d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto to = ir.GetRegister(m);
    const auto from = ir.GetRegister(n);
    const auto result = ir.PackedSelect(ir.GetGEFlags(), to, from);

    ir.SetRegister(d, result);
    return true;
}

// UBFX<c> <Rd>, <Rn>, #<lsb>, #<width>
bool arm_UBFX(Cond cond, Imm<5> widthm1, Reg d, Imm<5> lsb, Reg n) {
    if (d == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const u32 lsb_value = lsb.ZeroExtend();
    const u32 widthm1_value = widthm1.ZeroExtend();
    const u32 msb = lsb_value + widthm1_value;
    if (msb >= (sizeof(u32) * 8)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Value operand = ir.GetRegister(n);
    const Value mask = ir.Imm32(bits::ones<u32>(widthm1_value + 1));
    const Value result = ir.And(ir.LogicalShiftRight(operand, ir.Imm8(u8(lsb_value))), mask);

    ir.SetRegister(d, result);
    return true;
}

// reversal
/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

// RBIT<c> <Rd>, <Rm>
bool arm_RBIT(Cond cond, Reg d, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Value swapped = ir.ByteReverseWord(ir.GetRegister(m));

    // ((x & 0xF0F0F0F0) >> 4) | ((x & 0x0F0F0F0F) << 4)
    const Value first_lsr = ir.LogicalShiftRight(ir.And(swapped, ir.Imm32(0xF0F0F0F0)), ir.Imm8(4));
    const Value first_lsl = ir.LogicalShiftLeft(ir.And(swapped, ir.Imm32(0x0F0F0F0F)), ir.Imm8(4));
    const Value corrected = ir.Or(first_lsl, first_lsr);

    // ((x & 0x88888888) >> 3) | ((x & 0x44444444) >> 1) |
    // ((x & 0x22222222) << 1) | ((x & 0x11111111) << 3)
    const Value second_lsr =
            ir.LogicalShiftRight(ir.And(corrected, ir.Imm32(0x88888888)), ir.Imm8(3));
    const Value third_lsr =
            ir.LogicalShiftRight(ir.And(corrected, ir.Imm32(0x44444444)), ir.Imm8(1));
    const Value second_lsl =
            ir.LogicalShiftLeft(ir.And(corrected, ir.Imm32(0x22222222)), ir.Imm8(1));
    const Value third_lsl =
            ir.LogicalShiftLeft(ir.And(corrected, ir.Imm32(0x11111111)), ir.Imm8(3));

    const Value result = ir.Or(ir.Or(ir.Or(second_lsr, third_lsr), second_lsl), third_lsl);

    ir.SetRegister(d, result);
    return true;
}

// REV<c> <Rd>, <Rm>
bool arm_REV(Cond cond, Reg d, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto result = ir.ByteReverseWord(ir.GetRegister(m));
    ir.SetRegister(d, result);
    return true;
}

// REV16<c> <Rd>, <Rm>
bool arm_REV16(Cond cond, Reg d, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto reg_m = ir.GetRegister(m);
    const auto lo = ir.And(ir.LogicalShiftRight(reg_m, ir.Imm8(8), ir.Imm1(0)).result,
                           ir.Imm32(0x00FF00FF));
    const auto hi =
            ir.And(ir.LogicalShiftLeft(reg_m, ir.Imm8(8), ir.Imm1(0)).result, ir.Imm32(0xFF00FF00));
    const auto result = ir.Or(lo, hi);

    ir.SetRegister(d, result);
    return true;
}

// REVSH<c> <Rd>, <Rm>
bool arm_REVSH(Cond cond, Reg d, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto rev_half = ir.ByteReverseHalf(ir.LeastSignificantHalf(ir.GetRegister(m)));
    ir.SetRegister(d, ir.SignExtendHalfToWord(rev_half));
    return true;
}

// a32_crc32
/* This file is part of the dynarmic project.
 * Copyright (c) 2019 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

// It's considered constrained UNPREDICTABLE behavior if either
// CRC32 instruction variant is executed with a condition code
// that is *not* 0xE (Always execute). ARM defines one of the following
// as being a requirement in this case. Either:
//
// 1. The instruction is undefined.
// 2. The instruction executes as a NOP.
// 3. The instruction executes unconditionally.
// 4. The instruction executes conditionally.
//
// It's also considered constrained UNPREDICTABLE behavior if
// either CRC32 instruction variant is executed with a size specifier
// of 64-bit (sz -> 0b11)
//
// In this case, either:
//
// 1. The instruction is undefined
// 2. The instruction executes as a NOP.
// 3. The instruction executes with the additional decode: size = 32.
//
// In both cases, we treat as unpredictable, to allow
// library users to provide their own intended behavior
// in the unpredictable exception handler.

enum class CRCType {
    Castagnoli,
    ISO,
};

bool a32_crc32_CRC32Variant(Semantics& v, Cond cond, Imm<2> sz, Reg n, Reg d, Reg m, CRCType type) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return v.UnpredictableInstruction();
    }

    if (sz == 0b11) {
        return v.UnpredictableInstruction();
    }

    if (cond != Cond::AL) {
        return v.UnpredictableInstruction();
    }

    const Value result = [m, n, sz, type, &v] {
        const Value accumulator = v.ir.GetRegister(n);
        const Value data = v.ir.GetRegister(m);

        if (type == CRCType::ISO) {
            switch (sz.ZeroExtend()) {
                case 0b00:
                    return v.ir.CRC32ISO8(accumulator, data);
                case 0b01:
                    return v.ir.CRC32ISO16(accumulator, data);
                case 0b10:
                    return v.ir.CRC32ISO32(accumulator, data);
            }
        } else {
            switch (sz.ZeroExtend()) {
                case 0b00:
                    return v.ir.CRC32Castagnoli8(accumulator, data);
                case 0b01:
                    return v.ir.CRC32Castagnoli16(accumulator, data);
                case 0b10:
                    return v.ir.CRC32Castagnoli32(accumulator, data);
            }
        }

        __builtin_unreachable();
    }();

    v.ir.SetRegister(d, result);
    return true;
}

// CRC32{B,H,W}{<q>} <Rd>, <Rn>, <Rm>
bool arm_CRC32(Cond cond, Imm<2> sz, Reg n, Reg d, Reg m) {
    return a32_crc32_CRC32Variant(*this, cond, sz, n, d, m, CRCType::ISO);
}

// CRC32C{B,H,W}{<q>} <Rd>, <Rn>, <Rm>
bool arm_CRC32C(Cond cond, Imm<2> sz, Reg n, Reg d, Reg m) {
    return a32_crc32_CRC32Variant(*this, cond, sz, n, d, m, CRCType::Castagnoli);
}

// thumb32_data_processing_modified_immediate
/* This file is part of the dynarmic project.
 * Copyright (c) 2021 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

bool thumb32_TST_imm(Imm<1> i, Reg n, Imm<3> imm3, Imm<8> imm8) {
    if (n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto imm_carry = ThumbExpandImm_C(i, imm3, imm8, ir.GetCFlag());
    const auto result = ir.And(ir.GetRegister(n), ir.Imm32(imm_carry.imm32));

    ir.SetCpsrNZC(ir.NZFrom(result), imm_carry.carry);
    return true;
}

bool thumb32_AND_imm(Imm<1> i, bool S, Reg n, Imm<3> imm3, Reg d, Imm<8> imm8) {
    assert(!(d == Reg::PC && S));
    if ((d == Reg::PC && !S) || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto imm_carry = ThumbExpandImm_C(i, imm3, imm8, ir.GetCFlag());
    const auto result = ir.And(ir.GetRegister(n), ir.Imm32(imm_carry.imm32));

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), imm_carry.carry);
    }
    return true;
}

bool thumb32_BIC_imm(Imm<1> i, bool S, Reg n, Imm<3> imm3, Reg d, Imm<8> imm8) {
    if (d == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto imm_carry = ThumbExpandImm_C(i, imm3, imm8, ir.GetCFlag());
    const auto result = ir.AndNot(ir.GetRegister(n), ir.Imm32(imm_carry.imm32));

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), imm_carry.carry);
    }
    return true;
}

bool thumb32_MOV_imm(Imm<1> i, bool S, Imm<3> imm3, Reg d, Imm<8> imm8) {
    if (d == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto imm_carry = ThumbExpandImm_C(i, imm3, imm8, ir.GetCFlag());
    const auto result = ir.Imm32(imm_carry.imm32);

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), imm_carry.carry);
    }
    return true;
}

bool thumb32_ORR_imm(Imm<1> i, bool S, Reg n, Imm<3> imm3, Reg d, Imm<8> imm8) {
    assert(n != Reg::PC);
    if (d == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto imm_carry = ThumbExpandImm_C(i, imm3, imm8, ir.GetCFlag());
    const auto result = ir.Or(ir.GetRegister(n), ir.Imm32(imm_carry.imm32));

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), imm_carry.carry);
    }
    return true;
}

bool thumb32_MVN_imm(Imm<1> i, bool S, Imm<3> imm3, Reg d, Imm<8> imm8) {
    if (d == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto imm_carry = ThumbExpandImm_C(i, imm3, imm8, ir.GetCFlag());
    const auto result = ir.Imm32(~imm_carry.imm32);

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), imm_carry.carry);
    }
    return true;
}

bool thumb32_ORN_imm(Imm<1> i, bool S, Reg n, Imm<3> imm3, Reg d, Imm<8> imm8) {
    assert(n != Reg::PC);
    if (d == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto imm_carry = ThumbExpandImm_C(i, imm3, imm8, ir.GetCFlag());
    const auto result = ir.Or(ir.GetRegister(n), ir.Imm32(~imm_carry.imm32));

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), imm_carry.carry);
    }
    return true;
}

bool thumb32_TEQ_imm(Imm<1> i, Reg n, Imm<3> imm3, Imm<8> imm8) {
    if (n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto imm_carry = ThumbExpandImm_C(i, imm3, imm8, ir.GetCFlag());
    const auto result = ir.Eor(ir.GetRegister(n), ir.Imm32(imm_carry.imm32));

    ir.SetCpsrNZC(ir.NZFrom(result), imm_carry.carry);
    return true;
}

bool thumb32_EOR_imm(Imm<1> i, bool S, Reg n, Imm<3> imm3, Reg d, Imm<8> imm8) {
    assert(!(d == Reg::PC && S));
    if ((d == Reg::PC && !S) || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto imm_carry = ThumbExpandImm_C(i, imm3, imm8, ir.GetCFlag());
    const auto result = ir.Eor(ir.GetRegister(n), ir.Imm32(imm_carry.imm32));

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), imm_carry.carry);
    }
    return true;
}

bool thumb32_CMN_imm(Imm<1> i, Reg n, Imm<3> imm3, Imm<8> imm8) {
    if (n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto imm32 = ThumbExpandImm(i, imm3, imm8);
    const auto result = ir.AddWithCarry(ir.GetRegister(n), ir.Imm32(imm32), ir.Imm1(0));

    ir.SetCpsrNZCV(ir.NZCVFrom(result));
    return true;
}

bool thumb32_ADD_imm_1(Imm<1> i, bool S, Reg n, Imm<3> imm3, Reg d, Imm<8> imm8) {
    assert(!(d == Reg::PC && S));
    if ((d == Reg::PC && !S) || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto imm32 = ThumbExpandImm(i, imm3, imm8);
    const auto result = ir.AddWithCarry(ir.GetRegister(n), ir.Imm32(imm32), ir.Imm1(0));

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

bool thumb32_ADC_imm(Imm<1> i, bool S, Reg n, Imm<3> imm3, Reg d, Imm<8> imm8) {
    if (d == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto imm32 = ThumbExpandImm(i, imm3, imm8);
    const auto result = ir.AddWithCarry(ir.GetRegister(n), ir.Imm32(imm32), ir.GetCFlag());

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

bool thumb32_SBC_imm(Imm<1> i, bool S, Reg n, Imm<3> imm3, Reg d, Imm<8> imm8) {
    if (d == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto imm32 = ThumbExpandImm(i, imm3, imm8);
    const auto result = ir.SubWithCarry(ir.GetRegister(n), ir.Imm32(imm32), ir.GetCFlag());

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

bool thumb32_CMP_imm(Imm<1> i, Reg n, Imm<3> imm3, Imm<8> imm8) {
    if (n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto imm32 = ThumbExpandImm(i, imm3, imm8);
    const auto result = ir.SubWithCarry(ir.GetRegister(n), ir.Imm32(imm32), ir.Imm1(1));

    ir.SetCpsrNZCV(ir.NZCVFrom(result));
    return true;
}

bool thumb32_SUB_imm_1(Imm<1> i, bool S, Reg n, Imm<3> imm3, Reg d, Imm<8> imm8) {
    assert(!(d == Reg::PC && S));
    if ((d == Reg::PC && !S) || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto imm32 = ThumbExpandImm(i, imm3, imm8);
    const auto result = ir.SubWithCarry(ir.GetRegister(n), ir.Imm32(imm32), ir.Imm1(1));

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

bool thumb32_RSB_imm(Imm<1> i, bool S, Reg n, Imm<3> imm3, Reg d, Imm<8> imm8) {
    if (d == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto imm32 = ThumbExpandImm(i, imm3, imm8);
    const auto result = ir.SubWithCarry(ir.Imm32(imm32), ir.GetRegister(n), ir.Imm1(1));

    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

// thumb32_data_processing_plain_binary_immediate
/* This file is part of the dynarmic project.
 * Copyright (c) 2021 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

static Value thumb32_data_processing_plain_binary_immediate_Pack2x16To1x32(Semantics& ir,
                                                                           Value lo,
                                                                           Value hi) {
    return ir.Or(ir.And(lo, ir.Imm32(0xFFFF)),
                 ir.LogicalShiftLeft(hi, ir.Imm8(16), ir.Imm1(0)).result);
}

static Value thumb32_data_processing_plain_binary_immediate_MostSignificantHalf(Semantics& ir,
                                                                                Value value) {
    return ir.LeastSignificantHalf(ir.LogicalShiftRight(value, ir.Imm8(16), ir.Imm1(0)).result);
}

using thumb32_data_processing_plain_binary_immediate_SaturationFunction =
        OverflowResult (Semantics::*)(const Value&, size_t);

static bool thumb32_data_processing_plain_binary_immediate_Saturation(
        Semantics& v,
        bool sh,
        Reg n,
        Reg d,
        Imm<5> shift_amount,
        size_t saturate_to,
        thumb32_data_processing_plain_binary_immediate_SaturationFunction sat_fn) {
    assert(!(sh && shift_amount == 0));

    if (d == Reg::PC || n == Reg::PC) {
        return v.UnpredictableInstruction();
    }

    const auto shift = sh ? ShiftType::ASR : ShiftType::LSL;
    const auto operand = v.EmitImmShift(v.ir.GetRegister(n), shift, shift_amount, v.ir.GetCFlag());
    const auto result = (v.ir.*sat_fn)(operand.result, saturate_to);

    v.ir.SetRegister(d, result.result);
    v.ir.OrQFlag(result.overflow);
    return true;
}

static bool thumb32_data_processing_plain_binary_immediate_Saturation16(
        Semantics& v,
        Reg n,
        Reg d,
        size_t saturate_to,
        thumb32_data_processing_plain_binary_immediate_SaturationFunction sat_fn) {
    if (d == Reg::PC || n == Reg::PC) {
        return v.UnpredictableInstruction();
    }

    const auto reg_n = v.ir.GetRegister(n);

    const auto lo_operand = v.ir.SignExtendHalfToWord(v.ir.LeastSignificantHalf(reg_n));
    const auto hi_operand = v.ir.SignExtendHalfToWord(
            thumb32_data_processing_plain_binary_immediate_MostSignificantHalf(v.ir, reg_n));
    const auto lo_result = (v.ir.*sat_fn)(lo_operand, saturate_to);
    const auto hi_result = (v.ir.*sat_fn)(hi_operand, saturate_to);

    v.ir.SetRegister(d,
                     thumb32_data_processing_plain_binary_immediate_Pack2x16To1x32(
                             v.ir, lo_result.result, hi_result.result));
    v.ir.OrQFlag(lo_result.overflow);
    v.ir.OrQFlag(hi_result.overflow);
    return true;
}

bool thumb32_ADR_t2(Imm<1> imm1, Imm<3> imm3, Reg d, Imm<8> imm8) {
    if (d == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto imm32 = concatenate(imm1, imm3, imm8).ZeroExtend();
    const auto result = ir.AlignPC(4) - imm32;

    ir.SetRegister(d, ir.Imm32(result));
    return true;
}

bool thumb32_ADR_t3(Imm<1> imm1, Imm<3> imm3, Reg d, Imm<8> imm8) {
    if (d == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto imm32 = concatenate(imm1, imm3, imm8).ZeroExtend();
    const auto result = ir.AlignPC(4) + imm32;

    ir.SetRegister(d, ir.Imm32(result));
    return true;
}

bool thumb32_ADD_imm_2(Imm<1> imm1, Reg n, Imm<3> imm3, Reg d, Imm<8> imm8) {
    if (d == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const u32 imm = concatenate(imm1, imm3, imm8).ZeroExtend();
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.AddWithCarry(reg_n, ir.Imm32(imm), ir.Imm1(0));

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_BFC(Imm<3> imm3, Reg d, Imm<2> imm2, Imm<5> msb) {
    if (d == Reg::PC) {
        return UnpredictableInstruction();
    }

    const u32 lsbit = concatenate(imm3, imm2).ZeroExtend();
    const u32 msbit = msb.ZeroExtend();

    if (msbit < lsbit) {
        return UnpredictableInstruction();
    }

    const u32 mask = ~(bits::ones<u32>(msbit - lsbit + 1) << lsbit);
    const auto reg_d = ir.GetRegister(d);
    const auto result = ir.And(reg_d, ir.Imm32(mask));

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_BFI(Reg n, Imm<3> imm3, Reg d, Imm<2> imm2, Imm<5> msb) {
    if (d == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const u32 lsbit = concatenate(imm3, imm2).ZeroExtend();
    const u32 msbit = msb.ZeroExtend();

    if (msbit < lsbit) {
        return UnpredictableInstruction();
    }

    const u32 inclusion_mask = bits::ones<u32>(msbit - lsbit + 1) << lsbit;
    const u32 exclusion_mask = ~inclusion_mask;
    const Value operand1 = ir.And(ir.GetRegister(d), ir.Imm32(exclusion_mask));
    const Value operand2 = ir.And(ir.LogicalShiftLeft(ir.GetRegister(n), ir.Imm8(u8(lsbit))),
                                  ir.Imm32(inclusion_mask));
    const Value result = ir.Or(operand1, operand2);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_MOVT(Imm<1> imm1, Imm<4> imm4, Imm<3> imm3, Reg d, Imm<8> imm8) {
    if (d == Reg::PC) {
        return UnpredictableInstruction();
    }

    const Value imm16 = ir.Imm32(concatenate(imm4, imm1, imm3, imm8).ZeroExtend() << 16);
    const Value operand = ir.GetRegister(d);
    const Value result = ir.Or(ir.And(operand, ir.Imm32(0x0000FFFFU)), imm16);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_MOVW_imm(Imm<1> imm1, Imm<4> imm4, Imm<3> imm3, Reg d, Imm<8> imm8) {
    if (d == Reg::PC) {
        return UnpredictableInstruction();
    }

    const Value imm = ir.Imm32(concatenate(imm4, imm1, imm3, imm8).ZeroExtend());

    ir.SetRegister(d, imm);
    return true;
}

bool thumb32_SBFX(Reg n, Imm<3> imm3, Reg d, Imm<2> imm2, Imm<5> widthm1) {
    if (d == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const u32 lsbit = concatenate(imm3, imm2).ZeroExtend();
    const u32 widthm1_value = widthm1.ZeroExtend();
    const u32 msb = lsbit + widthm1_value;
    if (msb >= (sizeof(u32) * 8)) {
        return UnpredictableInstruction();
    }

    constexpr size_t max_width = (sizeof(u32) * 8);
    const auto width = widthm1_value + 1;
    const auto left_shift_amount = static_cast<u8>(max_width - width - lsbit);
    const auto right_shift_amount = static_cast<u8>(max_width - width);
    const auto operand = ir.GetRegister(n);
    const auto tmp = ir.LogicalShiftLeft(operand, ir.Imm8(left_shift_amount));
    const auto result = ir.ArithmeticShiftRight(tmp, ir.Imm8(right_shift_amount));

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_SSAT(bool sh, Reg n, Imm<3> imm3, Reg d, Imm<2> imm2, Imm<5> sat_imm) {
    return thumb32_data_processing_plain_binary_immediate_Saturation(*this,
                                                                     sh,
                                                                     n,
                                                                     d,
                                                                     concatenate(imm3, imm2),
                                                                     sat_imm.ZeroExtend() + 1,
                                                                     &Semantics::SignedSaturation);
}

bool thumb32_SSAT16(Reg n, Reg d, Imm<4> sat_imm) {
    return thumb32_data_processing_plain_binary_immediate_Saturation16(
            *this, n, d, sat_imm.ZeroExtend() + 1, &Semantics::SignedSaturation);
}

bool thumb32_SUB_imm_2(Imm<1> imm1, Reg n, Imm<3> imm3, Reg d, Imm<8> imm8) {
    if (d == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const u32 imm = concatenate(imm1, imm3, imm8).ZeroExtend();
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.SubWithCarry(reg_n, ir.Imm32(imm), ir.Imm1(1));

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UBFX(Reg n, Imm<3> imm3, Reg d, Imm<2> imm2, Imm<5> widthm1) {
    if (d == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const u32 lsbit = concatenate(imm3, imm2).ZeroExtend();
    const u32 widthm1_value = widthm1.ZeroExtend();
    const u32 msb = lsbit + widthm1_value;
    if (msb >= (sizeof(u32) * 8)) {
        return UnpredictableInstruction();
    }

    const auto operand = ir.GetRegister(n);
    const auto mask = ir.Imm32(bits::ones<u32>(widthm1_value + 1));
    const auto result = ir.And(ir.LogicalShiftRight(operand, ir.Imm8(u8(lsbit))), mask);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_USAT(bool sh, Reg n, Imm<3> imm3, Reg d, Imm<2> imm2, Imm<5> sat_imm) {
    return thumb32_data_processing_plain_binary_immediate_Saturation(
            *this,
            sh,
            n,
            d,
            concatenate(imm3, imm2),
            sat_imm.ZeroExtend(),
            &Semantics::UnsignedSaturation);
}

bool thumb32_USAT16(Reg n, Reg d, Imm<4> sat_imm) {
    return thumb32_data_processing_plain_binary_immediate_Saturation16(
            *this, n, d, sat_imm.ZeroExtend(), &Semantics::UnsignedSaturation);
}

// thumb32_data_processing_register
/* This file is part of the dynarmic project.
 * Copyright (c) 2021 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

Value thumb32_data_processing_register_Rotate(Semantics& ir, Reg m, SignExtendRotation rotate) {
    const u8 rotate_by = static_cast<u8>(static_cast<size_t>(rotate) * 8);
    return ir.RotateRight(ir.GetRegister(m), ir.Imm8(rotate_by), ir.Imm1(0)).result;
}

using thumb32_data_processing_register_ShiftFunction = ShiftResult (Semantics::*)(const Value&,
                                                                                  const Value&,
                                                                                  const Value&);

bool thumb32_data_processing_register_ShiftInstruction(
        Semantics& v,
        Reg m,
        Reg d,
        Reg s,
        bool S,
        thumb32_data_processing_register_ShiftFunction shift_fn) {
    if (d == Reg::PC || m == Reg::PC || s == Reg::PC) {
        return v.UnpredictableInstruction();
    }

    const auto shift_s = v.ir.LeastSignificantByte(v.ir.GetRegister(s));
    const auto apsr_c = v.ir.GetCFlag();
    const auto result_carry = (v.ir.*shift_fn)(v.ir.GetRegister(m), shift_s, apsr_c);

    if (S) {
        v.ir.SetCpsrNZC(v.ir.NZFrom(result_carry.result), result_carry.carry);
    }

    v.ir.SetRegister(d, result_carry.result);
    return true;
}

bool thumb32_ASR_reg(bool S, Reg m, Reg d, Reg s) {
    return thumb32_data_processing_register_ShiftInstruction(
            *this, m, d, s, S, &Semantics::ArithmeticShiftRight);
}

bool thumb32_LSL_reg(bool S, Reg m, Reg d, Reg s) {
    return thumb32_data_processing_register_ShiftInstruction(
            *this, m, d, s, S, &Semantics::LogicalShiftLeft);
}

bool thumb32_LSR_reg(bool S, Reg m, Reg d, Reg s) {
    return thumb32_data_processing_register_ShiftInstruction(
            *this, m, d, s, S, &Semantics::LogicalShiftRight);
}

bool thumb32_ROR_reg(bool S, Reg m, Reg d, Reg s) {
    return thumb32_data_processing_register_ShiftInstruction(
            *this, m, d, s, S, &Semantics::RotateRight);
}

bool thumb32_SXTB(Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto rotated = thumb32_data_processing_register_Rotate(ir, m, rotate);
    const auto result = ir.SignExtendByteToWord(ir.LeastSignificantByte(rotated));

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_SXTB16(Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto rotated = thumb32_data_processing_register_Rotate(ir, m, rotate);
    const auto low_byte = ir.And(rotated, ir.Imm32(0x00FF00FF));
    const auto sign_bit = ir.And(rotated, ir.Imm32(0x00800080));
    const auto result = ir.Or(low_byte, ir.Mul(sign_bit, ir.Imm32(0x1FE)));

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_SXTAB(Reg n, Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto rotated = thumb32_data_processing_register_Rotate(ir, m, rotate);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.Add(reg_n, ir.SignExtendByteToWord(ir.LeastSignificantByte(rotated)));

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_SXTAB16(Reg n, Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto rotated = thumb32_data_processing_register_Rotate(ir, m, rotate);
    const auto low_byte = ir.And(rotated, ir.Imm32(0x00FF00FF));
    const auto sign_bit = ir.And(rotated, ir.Imm32(0x00800080));
    const auto addend = ir.Or(low_byte, ir.Mul(sign_bit, ir.Imm32(0x1FE)));
    const auto result = ir.PackedAddU16(addend, ir.GetRegister(n)).result;

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_SXTH(Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto rotated = thumb32_data_processing_register_Rotate(ir, m, rotate);
    const auto result = ir.SignExtendHalfToWord(ir.LeastSignificantHalf(rotated));

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_SXTAH(Reg n, Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto rotated = thumb32_data_processing_register_Rotate(ir, m, rotate);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.Add(reg_n, ir.SignExtendHalfToWord(ir.LeastSignificantHalf(rotated)));

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UXTB(Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto rotated = thumb32_data_processing_register_Rotate(ir, m, rotate);
    const auto result = ir.ZeroExtendByteToWord(ir.LeastSignificantByte(rotated));

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UXTB16(Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto rotated = thumb32_data_processing_register_Rotate(ir, m, rotate);
    const auto result = ir.And(rotated, ir.Imm32(0x00FF00FF));

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UXTAB(Reg n, Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto rotated = thumb32_data_processing_register_Rotate(ir, m, rotate);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.Add(reg_n, ir.ZeroExtendByteToWord(ir.LeastSignificantByte(rotated)));

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UXTAB16(Reg n, Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto rotated = thumb32_data_processing_register_Rotate(ir, m, rotate);
    auto result = ir.And(rotated, ir.Imm32(0x00FF00FF));
    const auto reg_n = ir.GetRegister(n);
    result = ir.PackedAddU16(reg_n, result).result;

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UXTH(Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto rotated = thumb32_data_processing_register_Rotate(ir, m, rotate);
    const auto result = ir.ZeroExtendHalfToWord(ir.LeastSignificantHalf(rotated));

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_UXTAH(Reg n, Reg d, SignExtendRotation rotate, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto rotated = thumb32_data_processing_register_Rotate(ir, m, rotate);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.Add(reg_n, ir.ZeroExtendHalfToWord(ir.LeastSignificantHalf(rotated)));

    ir.SetRegister(d, result);
    return true;
}

// thumb32_data_processing_shifted_register
/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

bool thumb32_TST_reg(Reg n, Imm<3> imm3, Imm<2> imm2, ShiftType type, Reg m) {
    if (n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), type, imm3, imm2, ir.GetCFlag());
    const auto result = ir.And(ir.GetRegister(n), shifted.result);

    ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    return true;
}

bool thumb32_AND_reg(bool S, Reg n, Imm<3> imm3, Reg d, Imm<2> imm2, ShiftType type, Reg m) {
    assert(!(d == Reg::PC && S));

    if ((d == Reg::PC && !S) || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), type, imm3, imm2, ir.GetCFlag());
    const auto result = ir.And(ir.GetRegister(n), shifted.result);
    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }
    return true;
}

bool thumb32_BIC_reg(bool S, Reg n, Imm<3> imm3, Reg d, Imm<2> imm2, ShiftType type, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), type, imm3, imm2, ir.GetCFlag());
    const auto result = ir.AndNot(ir.GetRegister(n), shifted.result);
    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }
    return true;
}

bool thumb32_MOV_reg(bool S, Imm<3> imm3, Reg d, Imm<2> imm2, ShiftType type, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), type, imm3, imm2, ir.GetCFlag());
    const auto result = shifted.result;
    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }
    return true;
}

bool thumb32_ORR_reg(bool S, Reg n, Imm<3> imm3, Reg d, Imm<2> imm2, ShiftType type, Reg m) {
    assert(n != Reg::PC);

    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), type, imm3, imm2, ir.GetCFlag());
    const auto result = ir.Or(ir.GetRegister(n), shifted.result);
    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }
    return true;
}

bool thumb32_MVN_reg(bool S, Imm<3> imm3, Reg d, Imm<2> imm2, ShiftType type, Reg m) {
    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), type, imm3, imm2, ir.GetCFlag());
    const auto result = ir.Not(shifted.result);
    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }
    return true;
}

bool thumb32_ORN_reg(bool S, Reg n, Imm<3> imm3, Reg d, Imm<2> imm2, ShiftType type, Reg m) {
    assert(n != Reg::PC);

    if (d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), type, imm3, imm2, ir.GetCFlag());
    const auto result = ir.Or(ir.GetRegister(n), ir.Not(shifted.result));
    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }
    return true;
}

bool thumb32_TEQ_reg(Reg n, Imm<3> imm3, Imm<2> imm2, ShiftType type, Reg m) {
    if (n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), type, imm3, imm2, ir.GetCFlag());
    const auto result = ir.Eor(ir.GetRegister(n), shifted.result);

    ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    return true;
}

bool thumb32_EOR_reg(bool S, Reg n, Imm<3> imm3, Reg d, Imm<2> imm2, ShiftType type, Reg m) {
    assert(!(d == Reg::PC && S));

    if ((d == Reg::PC && !S) || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), type, imm3, imm2, ir.GetCFlag());
    const auto result = ir.Eor(ir.GetRegister(n), shifted.result);
    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZC(ir.NZFrom(result), shifted.carry);
    }
    return true;
}

bool thumb32_PKH(Reg n, Imm<3> imm3, Reg d, Imm<2> imm2, Imm<1> tb, Reg m) {
    const ShiftType type = concatenate(tb, Imm<1>{0}).ZeroExtend<ShiftType>();

    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto operand2 = EmitImmShift(ir.GetRegister(m), type, imm3, imm2, ir.GetCFlag()).result;
    const auto lower = ir.And((tb == 1) ? operand2 : ir.GetRegister(n), ir.Imm32(0x0000FFFF));
    const auto upper = ir.And((tb == 1) ? ir.GetRegister(n) : operand2, ir.Imm32(0xFFFF0000));
    const auto result = ir.Or(upper, lower);
    ir.SetRegister(d, result);
    return true;
}

bool thumb32_CMN_reg(Reg n, Imm<3> imm3, Imm<2> imm2, ShiftType type, Reg m) {
    if (n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), type, imm3, imm2, ir.GetCFlag());
    const auto result = ir.AddWithCarry(ir.GetRegister(n), shifted.result, ir.Imm1(0));

    ir.SetCpsrNZCV(ir.NZCVFrom(result));
    return true;
}

bool thumb32_ADD_reg(bool S, Reg n, Imm<3> imm3, Reg d, Imm<2> imm2, ShiftType type, Reg m) {
    assert(!(d == Reg::PC && S));

    if ((d == Reg::PC && !S) || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), type, imm3, imm2, ir.GetCFlag());
    const auto result = ir.AddWithCarry(ir.GetRegister(n), shifted.result, ir.Imm1(0));
    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

bool thumb32_ADC_reg(bool S, Reg n, Imm<3> imm3, Reg d, Imm<2> imm2, ShiftType type, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), type, imm3, imm2, ir.GetCFlag());
    const auto result = ir.AddWithCarry(ir.GetRegister(n), shifted.result, ir.GetCFlag());
    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

bool thumb32_SBC_reg(bool S, Reg n, Imm<3> imm3, Reg d, Imm<2> imm2, ShiftType type, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), type, imm3, imm2, ir.GetCFlag());
    const auto result = ir.SubWithCarry(ir.GetRegister(n), shifted.result, ir.GetCFlag());
    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

bool thumb32_CMP_reg(Reg n, Imm<3> imm3, Imm<2> imm2, ShiftType type, Reg m) {
    if (n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), type, imm3, imm2, ir.GetCFlag());
    const auto result = ir.SubWithCarry(ir.GetRegister(n), shifted.result, ir.Imm1(1));

    ir.SetCpsrNZCV(ir.NZCVFrom(result));
    return true;
}

bool thumb32_SUB_reg(bool S, Reg n, Imm<3> imm3, Reg d, Imm<2> imm2, ShiftType type, Reg m) {
    assert(!(d == Reg::PC && S));

    if ((d == Reg::PC && !S) || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), type, imm3, imm2, ir.GetCFlag());
    const auto result = ir.SubWithCarry(ir.GetRegister(n), shifted.result, ir.Imm1(1));
    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

bool thumb32_RSB_reg(bool S, Reg n, Imm<3> imm3, Reg d, Imm<2> imm2, ShiftType type, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto shifted = EmitImmShift(ir.GetRegister(m), type, imm3, imm2, ir.GetCFlag());
    const auto result = ir.SubWithCarry(shifted.result, ir.GetRegister(n), ir.Imm1(1));
    ir.SetRegister(d, result);
    if (S) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

// thumb32_misc
/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

bool thumb32_CLZ(Reg n, Reg d, Reg m) {
    if (m != n || d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto result = ir.CountLeadingZeros(reg_m);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_QADD(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.SignedSaturatedAddWithFlag(reg_m, reg_n);

    ir.SetRegister(d, result.result);
    ir.OrQFlag(result.overflow);
    return true;
}

bool thumb32_QDADD(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto doubled_n = ir.SignedSaturatedAddWithFlag(reg_n, reg_n);
    ir.OrQFlag(doubled_n.overflow);

    const auto result = ir.SignedSaturatedAddWithFlag(reg_m, doubled_n.result);
    ir.SetRegister(d, result.result);
    ir.OrQFlag(result.overflow);
    return true;
}

bool thumb32_QDSUB(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto doubled_n = ir.SignedSaturatedAddWithFlag(reg_n, reg_n);
    ir.OrQFlag(doubled_n.overflow);

    const auto result = ir.SignedSaturatedSubWithFlag(reg_m, doubled_n.result);
    ir.SetRegister(d, result.result);
    ir.OrQFlag(result.overflow);
    return true;
}

bool thumb32_QSUB(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.SignedSaturatedSubWithFlag(reg_m, reg_n);

    ir.SetRegister(d, result.result);
    ir.OrQFlag(result.overflow);
    return true;
}

bool thumb32_RBIT(Reg n, Reg d, Reg m) {
    if (m != n || d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const Value swapped = ir.ByteReverseWord(ir.GetRegister(m));

    // ((x & 0xF0F0F0F0) >> 4) | ((x & 0x0F0F0F0F) << 4)
    const Value first_lsr = ir.LogicalShiftRight(ir.And(swapped, ir.Imm32(0xF0F0F0F0)), ir.Imm8(4));
    const Value first_lsl = ir.LogicalShiftLeft(ir.And(swapped, ir.Imm32(0x0F0F0F0F)), ir.Imm8(4));
    const Value corrected = ir.Or(first_lsl, first_lsr);

    // ((x & 0x88888888) >> 3) | ((x & 0x44444444) >> 1) |
    // ((x & 0x22222222) << 1) | ((x & 0x11111111) << 3)
    const Value second_lsr =
            ir.LogicalShiftRight(ir.And(corrected, ir.Imm32(0x88888888)), ir.Imm8(3));
    const Value third_lsr =
            ir.LogicalShiftRight(ir.And(corrected, ir.Imm32(0x44444444)), ir.Imm8(1));
    const Value second_lsl =
            ir.LogicalShiftLeft(ir.And(corrected, ir.Imm32(0x22222222)), ir.Imm8(1));
    const Value third_lsl =
            ir.LogicalShiftLeft(ir.And(corrected, ir.Imm32(0x11111111)), ir.Imm8(3));

    const Value result = ir.Or(ir.Or(ir.Or(second_lsr, third_lsr), second_lsl), third_lsl);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_REV(Reg n, Reg d, Reg m) {
    if (m != n || d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto result = ir.ByteReverseWord(ir.GetRegister(m));

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_REV16(Reg n, Reg d, Reg m) {
    if (m != n || d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto lo = ir.And(ir.LogicalShiftRight(reg_m, ir.Imm8(8), ir.Imm1(0)).result,
                           ir.Imm32(0x00FF00FF));
    const auto hi =
            ir.And(ir.LogicalShiftLeft(reg_m, ir.Imm8(8), ir.Imm1(0)).result, ir.Imm32(0xFF00FF00));
    const auto result = ir.Or(lo, hi);

    ir.SetRegister(d, result);
    return true;
}

bool thumb32_REVSH(Reg n, Reg d, Reg m) {
    if (m != n || d == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto rev_half = ir.ByteReverseHalf(ir.LeastSignificantHalf(reg_m));

    ir.SetRegister(d, ir.SignExtendHalfToWord(rev_half));
    return true;
}

bool thumb32_SEL(Reg n, Reg d, Reg m) {
    if (d == Reg::PC || n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto result = ir.PackedSelect(ir.GetGEFlags(), reg_m, reg_n);

    ir.SetRegister(d, result);
    return true;
}

bool thumb16_LSL_imm(Imm<5> imm5, Reg m, Reg d) {
    const u8 shift_n = imm5.ZeroExtend<u8>();
    if (shift_n == 0 && ir.current_location.IT().IsInITBlock()) {
        return UnpredictableInstruction();
    }

    const auto cpsr_c = ir.GetCFlag();
    const auto result = ir.LogicalShiftLeft(ir.GetRegister(m), ir.Imm8(shift_n), cpsr_c);

    ir.SetRegister(d, result.result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZC(ir.NZFrom(result.result), result.carry);
    }
    return true;
}

bool thumb16_LSR_imm(Imm<5> imm5, Reg m, Reg d) {
    const u8 shift_n = imm5 != 0 ? imm5.ZeroExtend<u8>() : u8(32);
    const auto cpsr_c = ir.GetCFlag();
    const auto result = ir.LogicalShiftRight(ir.GetRegister(m), ir.Imm8(shift_n), cpsr_c);

    ir.SetRegister(d, result.result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZC(ir.NZFrom(result.result), result.carry);
    }
    return true;
}

bool thumb16_ASR_imm(Imm<5> imm5, Reg m, Reg d) {
    const u8 shift_n = imm5 != 0 ? imm5.ZeroExtend<u8>() : u8(32);
    const auto cpsr_c = ir.GetCFlag();
    const auto result = ir.ArithmeticShiftRight(ir.GetRegister(m), ir.Imm8(shift_n), cpsr_c);

    ir.SetRegister(d, result.result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZC(ir.NZFrom(result.result), result.carry);
    }
    return true;
}

bool thumb16_ADD_reg_t1(Reg m, Reg n, Reg d) {
    const auto result = ir.AddWithCarry(ir.GetRegister(n), ir.GetRegister(m), ir.Imm1(0));

    ir.SetRegister(d, result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

bool thumb16_SUB_reg(Reg m, Reg n, Reg d) {
    const auto result = ir.SubWithCarry(ir.GetRegister(n), ir.GetRegister(m), ir.Imm1(1));

    ir.SetRegister(d, result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

bool thumb16_ADD_imm_t1(Imm<3> imm3, Reg n, Reg d) {
    const u32 imm32 = imm3.ZeroExtend();
    const auto result = ir.AddWithCarry(ir.GetRegister(n), ir.Imm32(imm32), ir.Imm1(0));

    ir.SetRegister(d, result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

bool thumb16_SUB_imm_t1(Imm<3> imm3, Reg n, Reg d) {
    const u32 imm32 = imm3.ZeroExtend();
    const auto result = ir.SubWithCarry(ir.GetRegister(n), ir.Imm32(imm32), ir.Imm1(1));

    ir.SetRegister(d, result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

bool thumb16_MOV_imm(Reg d, Imm<8> imm8) {
    const u32 imm32 = imm8.ZeroExtend();
    const auto result = ir.Imm32(imm32);

    ir.SetRegister(d, result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZ(ir.NZFrom(result));
    }
    return true;
}

bool thumb16_CMP_imm(Reg n, Imm<8> imm8) {
    const u32 imm32 = imm8.ZeroExtend();
    const auto result = ir.SubWithCarry(ir.GetRegister(n), ir.Imm32(imm32), ir.Imm1(1));

    ir.SetCpsrNZCV(ir.NZCVFrom(result));
    return true;
}

bool thumb16_ADD_imm_t2(Reg d_n, Imm<8> imm8) {
    const u32 imm32 = imm8.ZeroExtend();
    const Reg d = d_n;
    const Reg n = d_n;
    const auto result = ir.AddWithCarry(ir.GetRegister(n), ir.Imm32(imm32), ir.Imm1(0));

    ir.SetRegister(d, result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

bool thumb16_SUB_imm_t2(Reg d_n, Imm<8> imm8) {
    const u32 imm32 = imm8.ZeroExtend();
    const Reg d = d_n;
    const Reg n = d_n;
    const auto result = ir.SubWithCarry(ir.GetRegister(n), ir.Imm32(imm32), ir.Imm1(1));

    ir.SetRegister(d, result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

bool thumb16_AND_reg(Reg m, Reg d_n) {
    const Reg d = d_n;
    const Reg n = d_n;
    const auto result = ir.And(ir.GetRegister(n), ir.GetRegister(m));

    ir.SetRegister(d, result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZ(ir.NZFrom(result));
    }
    return true;
}

bool thumb16_EOR_reg(Reg m, Reg d_n) {
    const Reg d = d_n;
    const Reg n = d_n;
    const auto result = ir.Eor(ir.GetRegister(n), ir.GetRegister(m));

    ir.SetRegister(d, result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZ(ir.NZFrom(result));
    }
    return true;
}

bool thumb16_LSL_reg(Reg m, Reg d_n) {
    const Reg d = d_n;
    const Reg n = d_n;
    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(m));
    const auto apsr_c = ir.GetCFlag();
    const auto result_carry = ir.LogicalShiftLeft(ir.GetRegister(n), shift_n, apsr_c);

    ir.SetRegister(d, result_carry.result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZC(ir.NZFrom(result_carry.result), result_carry.carry);
    }
    return true;
}

bool thumb16_LSR_reg(Reg m, Reg d_n) {
    const Reg d = d_n;
    const Reg n = d_n;
    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(m));
    const auto cpsr_c = ir.GetCFlag();
    const auto result = ir.LogicalShiftRight(ir.GetRegister(n), shift_n, cpsr_c);

    ir.SetRegister(d, result.result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZC(ir.NZFrom(result.result), result.carry);
    }
    return true;
}

bool thumb16_ASR_reg(Reg m, Reg d_n) {
    const Reg d = d_n;
    const Reg n = d_n;
    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(m));
    const auto cpsr_c = ir.GetCFlag();
    const auto result = ir.ArithmeticShiftRight(ir.GetRegister(n), shift_n, cpsr_c);

    ir.SetRegister(d, result.result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZC(ir.NZFrom(result.result), result.carry);
    }
    return true;
}

bool thumb16_ADC_reg(Reg m, Reg d_n) {
    const Reg d = d_n;
    const Reg n = d_n;
    const auto aspr_c = ir.GetCFlag();
    const auto result = ir.AddWithCarry(ir.GetRegister(n), ir.GetRegister(m), aspr_c);

    ir.SetRegister(d, result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

bool thumb16_SBC_reg(Reg m, Reg d_n) {
    const Reg d = d_n;
    const Reg n = d_n;
    const auto aspr_c = ir.GetCFlag();
    const auto result = ir.SubWithCarry(ir.GetRegister(n), ir.GetRegister(m), aspr_c);

    ir.SetRegister(d, result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

bool thumb16_ROR_reg(Reg m, Reg d_n) {
    const Reg d = d_n;
    const Reg n = d_n;
    const auto shift_n = ir.LeastSignificantByte(ir.GetRegister(m));
    const auto cpsr_c = ir.GetCFlag();
    const auto result = ir.RotateRight(ir.GetRegister(n), shift_n, cpsr_c);

    ir.SetRegister(d, result.result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZC(ir.NZFrom(result.result), result.carry);
    }
    return true;
}

bool thumb16_TST_reg(Reg m, Reg n) {
    const auto result = ir.And(ir.GetRegister(n), ir.GetRegister(m));
    ir.SetCpsrNZ(ir.NZFrom(result));
    return true;
}

bool thumb16_RSB_imm(Reg n, Reg d) {
    const auto result = ir.SubWithCarry(ir.Imm32(0), ir.GetRegister(n), ir.Imm1(1));
    ir.SetRegister(d, result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZCV(ir.NZCVFrom(result));
    }
    return true;
}

bool thumb16_CMP_reg_t1(Reg m, Reg n) {
    const auto result = ir.SubWithCarry(ir.GetRegister(n), ir.GetRegister(m), ir.Imm1(1));
    ir.SetCpsrNZCV(ir.NZCVFrom(result));
    return true;
}

bool thumb16_CMN_reg(Reg m, Reg n) {
    const auto result = ir.AddWithCarry(ir.GetRegister(n), ir.GetRegister(m), ir.Imm1(0));
    ir.SetCpsrNZCV(ir.NZCVFrom(result));
    return true;
}

bool thumb16_ORR_reg(Reg m, Reg d_n) {
    const Reg d = d_n;
    const Reg n = d_n;
    const auto result = ir.Or(ir.GetRegister(m), ir.GetRegister(n));

    ir.SetRegister(d, result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZ(ir.NZFrom(result));
    }
    return true;
}

bool thumb16_MUL_reg(Reg n, Reg d_m) {
    const Reg d = d_m;
    const Reg m = d_m;
    const auto result = ir.Mul(ir.GetRegister(m), ir.GetRegister(n));

    ir.SetRegister(d, result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZ(ir.NZFrom(result));
    }
    return true;
}

bool thumb16_BIC_reg(Reg m, Reg d_n) {
    const Reg d = d_n;
    const Reg n = d_n;
    const auto result = ir.AndNot(ir.GetRegister(n), ir.GetRegister(m));

    ir.SetRegister(d, result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZ(ir.NZFrom(result));
    }
    return true;
}

bool thumb16_MVN_reg(Reg m, Reg d) {
    const auto result = ir.Not(ir.GetRegister(m));

    ir.SetRegister(d, result);
    if (!ir.current_location.IT().IsInITBlock()) {
        ir.SetCpsrNZ(ir.NZFrom(result));
    }
    return true;
}

bool thumb16_ADD_reg_t2(bool d_n_hi, Reg m, Reg d_n_lo) {
    const Reg d_n = d_n_hi ? (d_n_lo + 8) : d_n_lo;
    const Reg n = d_n;
    const Reg d = d_n;
    if (n == Reg::PC && m == Reg::PC) {
        return UnpredictableInstruction();
    }
    if (d == Reg::PC && ir.current_location.IT().IsInITBlock() &&
        !ir.current_location.IT().IsLastInITBlock()) {
        return UnpredictableInstruction();
    }

    const auto result = ir.AddWithCarry(ir.GetRegister(n), ir.GetRegister(m), ir.Imm1(0));
    if (d == Reg::PC) {
        ir.UpdateUpperLocationDescriptor();
        ir.ALUWritePC(result);
        // Return to dispatch as we can't predict what PC is going to be. Stop compilation.
        ir.SetTerm(Term::FastDispatchHint{});
        return false;
    } else {
        ir.SetRegister(d, result);
        return true;
    }
}

bool thumb16_CMP_reg_t2(bool n_hi, Reg m, Reg n_lo) {
    const Reg n = n_hi ? (n_lo + 8) : n_lo;
    if (n < Reg::R8 && m < Reg::R8) {
        return UnpredictableInstruction();
    }
    if (n == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto result = ir.SubWithCarry(ir.GetRegister(n), ir.GetRegister(m), ir.Imm1(1));
    ir.SetCpsrNZCV(ir.NZCVFrom(result));
    return true;
}

bool thumb16_MOV_reg(bool d_hi, Reg m, Reg d_lo) {
    const Reg d = d_hi ? (d_lo + 8) : d_lo;
    if (d == Reg::PC && ir.current_location.IT().IsInITBlock() &&
        !ir.current_location.IT().IsLastInITBlock()) {
        return UnpredictableInstruction();
    }

    const auto result = ir.GetRegister(m);

    if (d == Reg::PC) {
        ir.UpdateUpperLocationDescriptor();
        ir.ALUWritePC(result);
        ir.SetTerm(Term::FastDispatchHint{});
        return false;
    } else {
        ir.SetRegister(d, result);
        return true;
    }
}

bool thumb16_ADR(Reg d, Imm<8> imm8) {
    const u32 imm32 = imm8.ZeroExtend() << 2;
    const auto result = ir.Imm32(ir.AlignPC(4) + imm32);

    ir.SetRegister(d, result);
    return true;
}

bool thumb16_ADD_sp_t1(Reg d, Imm<8> imm8) {
    const u32 imm32 = imm8.ZeroExtend() << 2;
    const auto result = ir.AddWithCarry(ir.GetRegister(Reg::SP), ir.Imm32(imm32), ir.Imm1(0));

    ir.SetRegister(d, result);
    return true;
}

bool thumb16_ADD_sp_t2(Imm<7> imm7) {
    const u32 imm32 = imm7.ZeroExtend() << 2;
    const Reg d = Reg::SP;
    const auto result = ir.AddWithCarry(ir.GetRegister(Reg::SP), ir.Imm32(imm32), ir.Imm1(0));

    ir.SetRegister(d, result);
    return true;
}

bool thumb16_SUB_sp(Imm<7> imm7) {
    const u32 imm32 = imm7.ZeroExtend() << 2;
    const Reg d = Reg::SP;
    const auto result = ir.SubWithCarry(ir.GetRegister(Reg::SP), ir.Imm32(imm32), ir.Imm1(1));

    ir.SetRegister(d, result);
    return true;
}

bool thumb16_SXTH(Reg m, Reg d) {
    const auto half = ir.LeastSignificantHalf(ir.GetRegister(m));
    ir.SetRegister(d, ir.SignExtendHalfToWord(half));
    return true;
}

bool thumb16_SXTB(Reg m, Reg d) {
    const auto byte = ir.LeastSignificantByte(ir.GetRegister(m));
    ir.SetRegister(d, ir.SignExtendByteToWord(byte));
    return true;
}

bool thumb16_UXTH(Reg m, Reg d) {
    const auto half = ir.LeastSignificantHalf(ir.GetRegister(m));
    ir.SetRegister(d, ir.ZeroExtendHalfToWord(half));
    return true;
}

bool thumb16_UXTB(Reg m, Reg d) {
    const auto byte = ir.LeastSignificantByte(ir.GetRegister(m));
    ir.SetRegister(d, ir.ZeroExtendByteToWord(byte));
    return true;
}

bool thumb16_REV(Reg m, Reg d) {
    ir.SetRegister(d, ir.ByteReverseWord(ir.GetRegister(m)));
    return true;
}

bool thumb16_REV16(Reg m, Reg d) {
    const auto Rm = ir.GetRegister(m);
    const auto upper_half =
            ir.LeastSignificantHalf(ir.LogicalShiftRight(Rm, ir.Imm8(16), ir.Imm1(0)).result);
    const auto lower_half = ir.LeastSignificantHalf(Rm);
    const auto rev_upper_half = ir.ZeroExtendHalfToWord(ir.ByteReverseHalf(upper_half));
    const auto rev_lower_half = ir.ZeroExtendHalfToWord(ir.ByteReverseHalf(lower_half));
    const auto result = ir.Or(ir.LogicalShiftLeft(rev_upper_half, ir.Imm8(16), ir.Imm1(0)).result,
                              rev_lower_half);

    ir.SetRegister(d, result);
    return true;
}

bool thumb16_REVSH(Reg m, Reg d) {
    const auto rev_half = ir.ByteReverseHalf(ir.LeastSignificantHalf(ir.GetRegister(m)));
    ir.SetRegister(d, ir.SignExtendHalfToWord(rev_half));
    return true;
}
