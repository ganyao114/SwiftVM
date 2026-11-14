bool UnprivilegedTransfer(unsigned bits, bool load, bool sign, bool half_encoding) {
    const Reg n = static_cast<Reg>((current_word >> 16) & 15),
              t = static_cast<Reg>((current_word >> 12) & 15);
    if (n == Reg::PC || t == Reg::PC || (load && n == t)) return UnpredictableInstruction();
    Value offset;
    if (half_encoding) {
        offset = (current_word & (1u << 22))
                         ? Imm32(((current_word >> 4) & 0xf0) | (current_word & 15))
                         : GetRegister(static_cast<Reg>(current_word & 15));
    } else if (current_word & (1u << 25)) {
        const Reg m = static_cast<Reg>(current_word & 15);
        if (m == Reg::PC) return UnpredictableInstruction();
        offset = EmitImmShift(GetRegister(m),
                              static_cast<ShiftType>((current_word >> 5) & 3),
                              Imm<5>{(current_word >> 7) & 31},
                              GetCFlag())
                         .result;
    } else
        offset = Imm32(current_word & 0xfff);
    const auto address = GetRegister(n);
    const auto updated = (current_word & (1u << 23)) ? Add(address, offset) : Sub(address, offset);
    if (load) {
        auto value = ReadMemory(bits, address, AccType::UNPRIVILEGED);
        value = sign ? SignExtendToWord(value) : ZeroExtendToWord(value);
        SetRegister(n, updated);
        SetRegister(t, value);
    } else {
        WriteMemory(bits, address, GetRegister(t), AccType::UNPRIVILEGED);
        SetRegister(n, updated);
    }
    return true;
}

Value TypedLoad(Type type, Value address, bool ordered = false) {
    const auto result = ordered ? sink.template Emit<OpCode::LoadMemoryTSO>(type, {Arg(address)})
                                : sink.template Emit<OpCode::LoadMemory>(type, {Arg(address)});
    if (!result) throw Rejected{BuildError::IRRejected, result.error};
    return {result.GetValue(), type, ordered ? OpCode::LoadMemoryTSO : OpCode::LoadMemory};
}

Value ReadMemory(size_t bits, Value address, AccType access = AccType::NORMAL) {
    if (access == AccType::ATOMIC && bits > 8)
        Emit<OpCode::CheckMemoryAlignment>(address, Literal{u32(bits == 64 ? 3 : bits / 8 - 1)});
    auto value = TypedLoad(ScalarType(bits), address, access == AccType::ORDERED);
    if (current_location.EFlag() && bits > 8)
        value = Emit<OpCode::ByteSwap>(value, Literal{u32(bits)});
    return value;
}

Value ReadMemory8(Value address, AccType access = AccType::NORMAL) {
    return ReadMemory(8, address, access);
}

Value ReadMemory16(Value address, AccType access = AccType::NORMAL) {
    return ReadMemory(16, address, access);
}

Value ReadMemory32(Value address, AccType access = AccType::NORMAL) {
    return ReadMemory(32, address, access);
}

Value ReadMemory64(Value address, AccType access = AccType::NORMAL) {
    return ReadMemory(64, address, access);
}

// Preserve the aligned doubleword transfer. At a word-aligned address with
// bit 2 set, LDRD/STRD instead perform two ordered word accesses (DDI0406C.d,
// A8.8.73 and A8.8.211). Each word owns its faults and successful commits.
void LoadDual(Value address, Reg t, Reg t2) {
    Emit<OpCode::CheckMemoryAlignment>(address, Literal{u32{3}});
    const auto split = Goto(TestBit(address, Literal{2u}));
    const auto data = ReadMemory64(address);
    SetRegister(t,
                current_location.EFlag() ? MostSignificantWord(data).result
                                         : LeastSignificantWord(data));
    SetRegister(t2,
                current_location.EFlag() ? LeastSignificantWord(data)
                                         : MostSignificantWord(data).result);
    const auto done = Goto(Imm1(true));
    BindLabel(split);
    const auto second_address = Add(address, Imm32(4));
    SetRegister(t, ReadMemory32(address));
    SetRegister(t2, ReadMemory32(second_address));
    BindLabel(done);
}

void StoreDual(Value address, Reg t, Reg t2) {
    Emit<OpCode::CheckMemoryAlignment>(address, Literal{u32{3}});
    const auto first = GetRegister(t), second = GetRegister(t2);
    const auto split = Goto(TestBit(address, Literal{2u}));
    WriteMemory64(address,
                  current_location.EFlag() ? Pack2x32To1x64(second, first)
                                           : Pack2x32To1x64(first, second));
    const auto done = Goto(Imm1(true));
    BindLabel(split);
    const auto second_address = Add(address, Imm32(4));
    WriteMemory32(address, first);
    WriteMemory32(second_address, second);
    BindLabel(done);
}

void ClearReservation() {
    StoreUniform(Uniform{layout.reservation_valid_offset, Type::U32}, Imm32(0));
}

void ClearExclusive() { ClearReservation(); }

void WriteMemory(size_t bits, Value address, Value value, AccType access = AccType::NORMAL) {
    if (access == AccType::ATOMIC && bits > 8)
        Emit<OpCode::CheckMemoryAlignment>(address, Literal{u32(bits == 64 ? 3 : bits / 8 - 1)});
    value = value.WithView(ScalarType(bits));
    if (current_location.EFlag() && bits > 8)
        value = Emit<OpCode::ByteSwap>(value, Literal{u32(bits)});
    if (access == AccType::ORDERED)
        Emit<OpCode::StoreMemoryTSO>(address, value);
    else
        Emit<OpCode::StoreMemory>(address, value);
    ClearReservation();
}

void WriteMemory8(Value address, Value value, AccType access = AccType::NORMAL) {
    WriteMemory(8, address, value, access);
}

void WriteMemory16(Value address, Value value, AccType access = AccType::NORMAL) {
    WriteMemory(16, address, value, access);
}

void WriteMemory32(Value address, Value value, AccType access = AccType::NORMAL) {
    WriteMemory(32, address, value, access);
}

void WriteMemory64(Value address, Value value, AccType access = AccType::NORMAL) {
    WriteMemory(64, address, value, access);
}

Value AtomicSwap(unsigned bits, Value address, Value value) {
    const auto type = ScalarType(bits);
    if (bits > 8)
        Emit<OpCode::CheckMemoryAlignment>(address, Literal{u32(bits == 64 ? 3 : bits / 8 - 1)});
    value = value.WithView(type);
    if (current_location.EFlag() && bits > 8) value = Emit<OpCode::ByteSwap>(value, Literal{bits});
    const std::array args{Arg(address), Arg(value)};
    const auto result =
            sink.template Emit<OpCode::AtomicExchange>(type, std::span<const Operand>{args});
    if (!result) throw Rejected{BuildError::IRRejected, result.error};
    Value out{result.GetValue(), type, OpCode::AtomicExchange};
    if (current_location.EFlag() && bits > 8) out = Emit<OpCode::ByteSwap>(out, Literal{bits});
    ClearReservation();
    return ZeroExtendToWord(out);
}

void RequestStateService(runtime::arm32::StateServiceRequest request) {
    request.instruction_bytes = current_instruction_size;
    StoreUniform(Uniform{layout.service_word_offset, Type::U32},
                 Imm32(runtime::arm32::EncodeStateService(request)));
    StoreUniform(Uniform{layout.service_pc_offset, Type::U64}, Imm64(current_pc));
    control.kind = Control::Kind::Service;
    control.yes = current_pc;
    control.service = runtime::ExitValue(runtime::NativeExit::StateService);
    end_decode_ = true;
}

bool ExclusiveLoad(
        unsigned size, Reg n, Reg t, Reg t2 = Reg::R0, unsigned offset = 0, bool ordered = false) {
    if (n == Reg::PC || t == Reg::PC || (size == 3 && (t2 == Reg::PC || t == t2)))
        return UnpredictableInstruction();
    RequestStateService({runtime::arm32::StateServiceKind::LoadExclusive,
                         u8(size),
                         u8(n),
                         u8(t),
                         u8(t2),
                         0,
                         u8(current_instruction_size),
                         u8(offset),
                         ordered});
    return false;
}

bool ExclusiveStore(unsigned size,
                    Reg n,
                    Reg d,
                    Reg t,
                    Reg t2 = Reg::R0,
                    unsigned offset = 0,
                    bool ordered = false) {
    if (n == Reg::PC || d == Reg::PC || t == Reg::PC || d == n || d == t ||
        (size == 3 && (t2 == Reg::PC || t == t2 || d == t2)))
        return UnpredictableInstruction();
    RequestStateService({runtime::arm32::StateServiceKind::StoreExclusive,
                         u8(size),
                         u8(n),
                         u8(t),
                         u8(t2),
                         u8(d),
                         u8(current_instruction_size),
                         u8(offset),
                         ordered});
    return false;
}

void DataMemoryBarrier() { Emit<OpCode::MemoryBarrierTSO>(); }

void DataSynchronizationBarrier() { Emit<OpCode::MemoryBarrierTSO>(); }

void InstructionSynchronizationBarrier() { Emit<OpCode::MemoryBarrierTSO>(); }

// Imported A32 instruction semantics.
// Adapted from dynarmic-interp 5a76f693 (MerryMage and contributors), SPDX-License-Identifier:
// 0BSD.

// load_store
/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

bool arm_LDRBT() { return UnprivilegedTransfer(8, true, false, false); }

bool arm_LDRHT() { return UnprivilegedTransfer(16, true, false, true); }

bool arm_LDRSBT() { return UnprivilegedTransfer(8, true, true, true); }

bool arm_LDRSHT() { return UnprivilegedTransfer(16, true, true, true); }

bool arm_LDRT() { return UnprivilegedTransfer(32, true, false, false); }

bool arm_STRBT() { return UnprivilegedTransfer(8, false, false, false); }

bool arm_STRHT() { return UnprivilegedTransfer(16, false, false, true); }

bool arm_STRT() { return UnprivilegedTransfer(32, false, false, false); }

static Value load_store_GetAddress(Semantics& ir, bool P, bool U, bool W, Reg n, Value offset) {
    const bool index = P;
    const bool add = U;
    const bool wback = !P || W;

    const Value offset_addr =
            add ? ir.Add(ir.GetRegister(n), offset) : ir.Sub(ir.GetRegister(n), offset);
    const Value address = index ? offset_addr : ir.GetRegister(n);

    if (wback) ir.pending_writeback = RegisterWriteback{n, offset_addr};
    return address;
}

// LDR <Rt>, [PC, #+/-<imm>]
bool arm_LDR_lit(Cond cond, bool U, Reg t, Imm<12> imm12) {
    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const bool add = U;
    const u32 base = ir.AlignPC(4);
    const u32 address = add ? (base + imm12.ZeroExtend()) : (base - imm12.ZeroExtend());
    const auto data = ir.ReadMemory32(ir.Imm32(address), AccType::NORMAL);

    if (t == Reg::PC) {
        ir.LoadWritePC(data);
        ir.SetTerm(Term::FastDispatchHint{});
        return false;
    }

    ir.SetRegister(t, data);
    return true;
}

// LDR <Rt>, [<Rn>, #+/-<imm>]{!}
// LDR <Rt>, [<Rn>], #+/-<imm>
bool arm_LDR_imm(Cond cond, bool P, bool U, bool W, Reg n, Reg t, Imm<12> imm12) {
    if (n == Reg::PC) {
        return UnpredictableInstruction();
    }

    assert(!(!P && W));
    if ((!P || W) && n == t) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = imm12.ZeroExtend();
    const auto offset = ir.Imm32(imm32);
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);
    const auto data = ir.ReadMemory32(address, AccType::NORMAL);

    if (t == Reg::PC) {
        ir.LoadWritePC(data);

        if (!P && W && n == Reg::R13) {
            ir.SetTerm(Term::PopRSBHint{});
        } else {
            ir.SetTerm(Term::FastDispatchHint{});
        }

        return false;
    }

    ir.SetRegister(t, data);
    return true;
}

// LDR <Rt>, [<Rn>, #+/-<Rm>]{!}
// LDR <Rt>, [<Rn>], #+/-<Rm>
bool arm_LDR_reg(
        Cond cond, bool P, bool U, bool W, Reg n, Reg t, Imm<5> imm5, ShiftType shift, Reg m) {
    assert(!(!P && W));
    if (m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if ((!P || W) && (n == Reg::PC || n == t)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto offset = EmitImmShift(ir.GetRegister(m), shift, imm5, ir.GetCFlag()).result;
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);
    const auto data = ir.ReadMemory32(address, AccType::NORMAL);

    if (t == Reg::PC) {
        ir.LoadWritePC(data);
        ir.SetTerm(Term::FastDispatchHint{});
        return false;
    }

    ir.SetRegister(t, data);
    return true;
}

// LDRB <Rt>, [PC, #+/-<imm>]
bool arm_LDRB_lit(Cond cond, bool U, Reg t, Imm<12> imm12) {
    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = imm12.ZeroExtend();
    const bool add = U;
    const u32 base = ir.AlignPC(4);
    const u32 address = add ? (base + imm32) : (base - imm32);
    const auto data = ir.ZeroExtendByteToWord(ir.ReadMemory8(ir.Imm32(address), AccType::NORMAL));

    ir.SetRegister(t, data);
    return true;
}

// LDRB <Rt>, [<Rn>, #+/-<imm>]{!}
// LDRB <Rt>, [<Rn>], #+/-<imm>
bool arm_LDRB_imm(Cond cond, bool P, bool U, bool W, Reg n, Reg t, Imm<12> imm12) {
    if (n == Reg::PC) {
        return UnpredictableInstruction();
    }

    assert(!(!P && W));
    if ((!P || W) && n == t) {
        return UnpredictableInstruction();
    }

    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = imm12.ZeroExtend();
    const auto offset = ir.Imm32(imm32);
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);
    const auto data = ir.ZeroExtendByteToWord(ir.ReadMemory8(address, AccType::NORMAL));

    ir.SetRegister(t, data);
    return true;
}

// LDRB <Rt>, [<Rn>, #+/-<Rm>]{!}
// LDRB <Rt>, [<Rn>], #+/-<Rm>
bool arm_LDRB_reg(
        Cond cond, bool P, bool U, bool W, Reg n, Reg t, Imm<5> imm5, ShiftType shift, Reg m) {
    assert(!(!P && W));
    if (t == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if ((!P || W) && (n == Reg::PC || n == t)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto offset = EmitImmShift(ir.GetRegister(m), shift, imm5, ir.GetCFlag()).result;
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);
    const auto data = ir.ZeroExtendByteToWord(ir.ReadMemory8(address, AccType::NORMAL));

    ir.SetRegister(t, data);
    return true;
}

// LDRD <Rt>, <Rt2>, [PC, #+/-<imm>]
bool arm_LDRD_lit(Cond cond, bool U, Reg t, Imm<4> imm8a, Imm<4> imm8b) {
    if (RegNumber(t) % 2 == 1) {
        return UnpredictableInstruction();
    }

    if (t + 1 == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Reg t2 = t + 1;
    const u32 imm32 = concatenate(imm8a, imm8b).ZeroExtend();
    const bool add = U;

    const u32 base = ir.AlignPC(4);
    const u32 address = add ? (base + imm32) : (base - imm32);

    ir.LoadDual(ir.Imm32(address), t, t2);
    return true;
}

// LDRD <Rt>, [<Rn>, #+/-<imm>]{!}
// LDRD <Rt>, [<Rn>], #+/-<imm>
bool arm_LDRD_imm(Cond cond, bool P, bool U, bool W, Reg n, Reg t, Imm<4> imm8a, Imm<4> imm8b) {
    if (n == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (RegNumber(t) % 2 == 1) {
        return UnpredictableInstruction();
    }

    if (!P && W) {
        return UnpredictableInstruction();
    }

    if ((!P || W) && (n == t || n == t + 1)) {
        return UnpredictableInstruction();
    }

    if (t + 1 == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Reg t2 = t + 1;
    const u32 imm32 = concatenate(imm8a, imm8b).ZeroExtend();

    const auto offset = ir.Imm32(imm32);
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);

    ir.LoadDual(address, t, t2);
    return true;
}

// LDRD <Rt>, [<Rn>, #+/-<Rm>]{!}
// LDRD <Rt>, [<Rn>], #+/-<Rm>
bool arm_LDRD_reg(Cond cond, bool P, bool U, bool W, Reg n, Reg t, Reg m) {
    if (RegNumber(t) % 2 == 1) {
        return UnpredictableInstruction();
    }

    if (!P && W) {
        return UnpredictableInstruction();
    }

    if (t + 1 == Reg::PC || m == Reg::PC || m == t || m == t + 1) {
        return UnpredictableInstruction();
    }

    if ((!P || W) && (n == Reg::PC || n == t || n == t + 1)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const Reg t2 = t + 1;
    const auto offset = ir.GetRegister(m);
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);

    ir.LoadDual(address, t, t2);
    return true;
}

// LDRH <Rt>, [PC, #-/+<imm>]
bool arm_LDRH_lit(Cond cond, bool P, bool U, bool W, Reg t, Imm<4> imm8a, Imm<4> imm8b) {
    assert(!(!P && W));
    if (P == W) {
        return UnpredictableInstruction();
    }

    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = concatenate(imm8a, imm8b).ZeroExtend();
    const bool add = U;
    const u32 base = ir.AlignPC(4);
    const u32 address = add ? (base + imm32) : (base - imm32);
    const auto data = ir.ZeroExtendHalfToWord(ir.ReadMemory16(ir.Imm32(address), AccType::NORMAL));

    ir.SetRegister(t, data);
    return true;
}

// LDRH <Rt>, [<Rn>, #+/-<imm>]{!}
// LDRH <Rt>, [<Rn>], #+/-<imm>
bool arm_LDRH_imm(Cond cond, bool P, bool U, bool W, Reg n, Reg t, Imm<4> imm8a, Imm<4> imm8b) {
    if (n == Reg::PC) {
        return UnpredictableInstruction();
    }

    assert(!(!P && W));
    if ((!P || W) && n == t) {
        return UnpredictableInstruction();
    }

    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = concatenate(imm8a, imm8b).ZeroExtend();
    const auto offset = ir.Imm32(imm32);
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);
    const auto data = ir.ZeroExtendHalfToWord(ir.ReadMemory16(address, AccType::NORMAL));

    ir.SetRegister(t, data);
    return true;
}

// LDRH <Rt>, [<Rn>, #+/-<Rm>]{!}
// LDRH <Rt>, [<Rn>], #+/-<Rm>
bool arm_LDRH_reg(Cond cond, bool P, bool U, bool W, Reg n, Reg t, Reg m) {
    assert(!(!P && W));
    if (t == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if ((!P || W) && (n == Reg::PC || n == t)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto offset = ir.GetRegister(m);
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);
    const auto data = ir.ZeroExtendHalfToWord(ir.ReadMemory16(address, AccType::NORMAL));

    ir.SetRegister(t, data);
    return true;
}

// LDRSB <Rt>, [PC, #+/-<imm>]
bool arm_LDRSB_lit(Cond cond, bool U, Reg t, Imm<4> imm8a, Imm<4> imm8b) {
    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = concatenate(imm8a, imm8b).ZeroExtend();
    const bool add = U;

    const u32 base = ir.AlignPC(4);
    const u32 address = add ? (base + imm32) : (base - imm32);
    const auto data = ir.SignExtendByteToWord(ir.ReadMemory8(ir.Imm32(address), AccType::NORMAL));

    ir.SetRegister(t, data);
    return true;
}

// LDRSB <Rt>, [<Rn>, #+/-<imm>]{!}
// LDRSB <Rt>, [<Rn>], #+/-<imm>
bool arm_LDRSB_imm(Cond cond, bool P, bool U, bool W, Reg n, Reg t, Imm<4> imm8a, Imm<4> imm8b) {
    if (n == Reg::PC) {
        return UnpredictableInstruction();
    }

    assert(!(!P && W));
    if ((!P || W) && n == t) {
        return UnpredictableInstruction();
    }

    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = concatenate(imm8a, imm8b).ZeroExtend();
    const auto offset = ir.Imm32(imm32);
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);
    const auto data = ir.SignExtendByteToWord(ir.ReadMemory8(address, AccType::NORMAL));

    ir.SetRegister(t, data);
    return true;
}

// LDRSB <Rt>, [<Rn>, #+/-<Rm>]{!}
// LDRSB <Rt>, [<Rn>], #+/-<Rm>
bool arm_LDRSB_reg(Cond cond, bool P, bool U, bool W, Reg n, Reg t, Reg m) {
    assert(!(!P && W));
    if (t == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if ((!P || W) && (n == Reg::PC || n == t)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto offset = ir.GetRegister(m);
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);
    const auto data = ir.SignExtendByteToWord(ir.ReadMemory8(address, AccType::NORMAL));

    ir.SetRegister(t, data);
    return true;
}

// LDRSH <Rt>, [PC, #-/+<imm>]
bool arm_LDRSH_lit(Cond cond, bool U, Reg t, Imm<4> imm8a, Imm<4> imm8b) {
    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = concatenate(imm8a, imm8b).ZeroExtend();
    const bool add = U;
    const u32 base = ir.AlignPC(4);
    const u32 address = add ? (base + imm32) : (base - imm32);
    const auto data = ir.SignExtendHalfToWord(ir.ReadMemory16(ir.Imm32(address), AccType::NORMAL));

    ir.SetRegister(t, data);
    return true;
}

// LDRSH <Rt>, [<Rn>, #+/-<imm>]{!}
// LDRSH <Rt>, [<Rn>], #+/-<imm>
bool arm_LDRSH_imm(Cond cond, bool P, bool U, bool W, Reg n, Reg t, Imm<4> imm8a, Imm<4> imm8b) {
    if (n == Reg::PC) {
        return UnpredictableInstruction();
    }

    assert(!(!P && W));
    if ((!P || W) && n == t) {
        return UnpredictableInstruction();
    }

    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = concatenate(imm8a, imm8b).ZeroExtend();
    const auto offset = ir.Imm32(imm32);
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);
    const auto data = ir.SignExtendHalfToWord(ir.ReadMemory16(address, AccType::NORMAL));

    ir.SetRegister(t, data);
    return true;
}

// LDRSH <Rt>, [<Rn>, #+/-<Rm>]{!}
// LDRSH <Rt>, [<Rn>], #+/-<Rm>
bool arm_LDRSH_reg(Cond cond, bool P, bool U, bool W, Reg n, Reg t, Reg m) {
    assert(!(!P && W));
    if (t == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if ((!P || W) && (n == Reg::PC || n == t)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto offset = ir.GetRegister(m);
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);
    const auto data = ir.SignExtendHalfToWord(ir.ReadMemory16(address, AccType::NORMAL));

    ir.SetRegister(t, data);
    return true;
}

// STR <Rt>, [<Rn>, #+/-<imm>]{!}
// STR <Rt>, [<Rn>], #+/-<imm>
bool arm_STR_imm(Cond cond, bool P, bool U, bool W, Reg n, Reg t, Imm<12> imm12) {
    if ((!P || W) && (n == Reg::PC || n == t)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto offset = ir.Imm32(imm12.ZeroExtend());
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);
    ir.WriteMemory32(address, ir.GetRegister(t), AccType::NORMAL);
    return true;
}

// STR <Rt>, [<Rn>, #+/-<Rm>]{!}
// STR <Rt>, [<Rn>], #+/-<Rm>
bool arm_STR_reg(
        Cond cond, bool P, bool U, bool W, Reg n, Reg t, Imm<5> imm5, ShiftType shift, Reg m) {
    if (m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if ((!P || W) && (n == Reg::PC || n == t)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto offset = EmitImmShift(ir.GetRegister(m), shift, imm5, ir.GetCFlag()).result;
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);
    ir.WriteMemory32(address, ir.GetRegister(t), AccType::NORMAL);
    return true;
}

// STRB <Rt>, [<Rn>, #+/-<imm>]{!}
// STRB <Rt>, [<Rn>], #+/-<imm>
bool arm_STRB_imm(Cond cond, bool P, bool U, bool W, Reg n, Reg t, Imm<12> imm12) {
    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }

    if ((!P || W) && (n == Reg::PC || n == t)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto offset = ir.Imm32(imm12.ZeroExtend());
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);
    ir.WriteMemory8(address, ir.LeastSignificantByte(ir.GetRegister(t)), AccType::NORMAL);
    return true;
}

// STRB <Rt>, [<Rn>, #+/-<Rm>]{!}
// STRB <Rt>, [<Rn>], #+/-<Rm>
bool arm_STRB_reg(
        Cond cond, bool P, bool U, bool W, Reg n, Reg t, Imm<5> imm5, ShiftType shift, Reg m) {
    if (t == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if ((!P || W) && (n == Reg::PC || n == t)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto offset = EmitImmShift(ir.GetRegister(m), shift, imm5, ir.GetCFlag()).result;
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);
    ir.WriteMemory8(address, ir.LeastSignificantByte(ir.GetRegister(t)), AccType::NORMAL);
    return true;
}

// STRD <Rt>, [<Rn>, #+/-<imm>]{!}
// STRD <Rt>, [<Rn>], #+/-<imm>
bool arm_STRD_imm(Cond cond, bool P, bool U, bool W, Reg n, Reg t, Imm<4> imm8a, Imm<4> imm8b) {
    if (size_t(t) % 2 != 0) {
        return UnpredictableInstruction();
    }

    if (!P && W) {
        return UnpredictableInstruction();
    }

    const Reg t2 = t + 1;
    if ((!P || W) && (n == Reg::PC || n == t || n == t2)) {
        return UnpredictableInstruction();
    }

    if (t2 == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = concatenate(imm8a, imm8b).ZeroExtend();
    const auto offset = ir.Imm32(imm32);
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);
    ir.StoreDual(address, t, t2);
    return true;
}

// STRD <Rt>, [<Rn>, #+/-<Rm>]{!}
// STRD <Rt>, [<Rn>], #+/-<Rm>
bool arm_STRD_reg(Cond cond, bool P, bool U, bool W, Reg n, Reg t, Reg m) {
    if (size_t(t) % 2 != 0) {
        return UnpredictableInstruction();
    }

    if (!P && W) {
        return UnpredictableInstruction();
    }

    const Reg t2 = t + 1;
    if (t2 == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if ((!P || W) && (n == Reg::PC || n == t || n == t2)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto offset = ir.GetRegister(m);
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);
    ir.StoreDual(address, t, t2);
    return true;
}

// STRH <Rt>, [<Rn>, #+/-<imm>]{!}
// STRH <Rt>, [<Rn>], #+/-<imm>
bool arm_STRH_imm(Cond cond, bool P, bool U, bool W, Reg n, Reg t, Imm<4> imm8a, Imm<4> imm8b) {
    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }

    if ((!P || W) && (n == Reg::PC || n == t)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const u32 imm32 = concatenate(imm8a, imm8b).ZeroExtend();
    const auto offset = ir.Imm32(imm32);
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);

    ir.WriteMemory16(address, ir.LeastSignificantHalf(ir.GetRegister(t)), AccType::NORMAL);
    return true;
}

// STRH <Rt>, [<Rn>, #+/-<Rm>]{!}
// STRH <Rt>, [<Rn>], #+/-<Rm>
bool arm_STRH_reg(Cond cond, bool P, bool U, bool W, Reg n, Reg t, Reg m) {
    if (t == Reg::PC || m == Reg::PC) {
        return UnpredictableInstruction();
    }

    if ((!P || W) && (n == Reg::PC || n == t)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto offset = ir.GetRegister(m);
    const auto address = load_store_GetAddress(ir, P, U, W, n, offset);

    ir.WriteMemory16(address, ir.LeastSignificantHalf(ir.GetRegister(t)), AccType::NORMAL);
    return true;
}

static bool load_store_LDMHelper(
        Semantics& ir, bool W, Reg n, RegList list, Value start_address, Value writeback_address) {
    u32 offset = 0;
    const auto word_at = [&] {
        const auto addr = offset == 0 ? start_address : ir.Add(start_address, ir.Imm32(offset));
        const auto acc = offset == 0 ? AccType::ATOMIC : AccType::NORMAL;
        offset += 4;
        return ir.ReadMemory32(addr, acc);
    };
    for (size_t i = 0; i <= 14; i++) {
        if (bits::get_bit(i, list)) ir.SetRegister(static_cast<Reg>(i), word_at());
    }
    Value branch_pc;
    if (bits::get_bit<15>(list)) branch_pc = word_at();
    if (W && !bits::get_bit(RegNumber(n), list)) {
        ir.SetRegister(n, writeback_address);
    }
    if (bits::get_bit<15>(list)) {
        ir.LoadWritePC(branch_pc);
        if (n == Reg::R13)
            ir.SetTerm(Term::PopRSBHint{});
        else
            ir.SetTerm(Term::FastDispatchHint{});
        return false;
    }
    return true;
}

// LDM <Rn>{!}, <reg_list>
bool arm_LDM(Cond cond, bool W, Reg n, RegList list) {
    if (n == Reg::PC || bits::count_ones(list) < 1) {
        return UnpredictableInstruction();
    }
    if (W && bits::get_bit(static_cast<size_t>(n), list)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto start_address = ir.GetRegister(n);
    const auto writeback_address = ir.Add(start_address, ir.Imm32(u32(bits::count_ones(list) * 4)));
    return load_store_LDMHelper(ir, W, n, list, start_address, writeback_address);
}

// LDMDA <Rn>{!}, <reg_list>
bool arm_LDMDA(Cond cond, bool W, Reg n, RegList list) {
    if (n == Reg::PC || bits::count_ones(list) < 1) {
        return UnpredictableInstruction();
    }
    if (W && bits::get_bit(static_cast<size_t>(n), list)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto start_address =
            ir.Sub(ir.GetRegister(n), ir.Imm32(u32(4 * bits::count_ones(list) - 4)));
    const auto writeback_address = ir.Sub(start_address, ir.Imm32(4));
    return load_store_LDMHelper(ir, W, n, list, start_address, writeback_address);
}

// LDMDB <Rn>{!}, <reg_list>
bool arm_LDMDB(Cond cond, bool W, Reg n, RegList list) {
    if (n == Reg::PC || bits::count_ones(list) < 1) {
        return UnpredictableInstruction();
    }
    if (W && bits::get_bit(static_cast<size_t>(n), list)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto start_address = ir.Sub(ir.GetRegister(n), ir.Imm32(u32(4 * bits::count_ones(list))));
    const auto writeback_address = start_address;
    return load_store_LDMHelper(ir, W, n, list, start_address, writeback_address);
}

// LDMIB <Rn>{!}, <reg_list>
bool arm_LDMIB(Cond cond, bool W, Reg n, RegList list) {
    if (n == Reg::PC || bits::count_ones(list) < 1) {
        return UnpredictableInstruction();
    }
    if (W && bits::get_bit(static_cast<size_t>(n), list)) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto start_address = ir.Add(ir.GetRegister(n), ir.Imm32(4));
    const auto writeback_address =
            ir.Add(ir.GetRegister(n), ir.Imm32(u32(4 * bits::count_ones(list))));
    return load_store_LDMHelper(ir, W, n, list, start_address, writeback_address);
}

bool arm_LDM_usr() { return UndefinedInstruction(); }

bool arm_LDM_eret() { return UndefinedInstruction(); }

static bool load_store_STMHelper(
        Semantics& ir, bool W, Reg n, RegList list, Value start_address, Value writeback_address) {
    u32 offset = 0;
    const auto store_word = [&](Value val) {
        const auto addr = offset == 0 ? start_address : ir.Add(start_address, ir.Imm32(offset));
        const auto acc = offset == 0 ? AccType::ATOMIC : AccType::NORMAL;
        offset += 4;
        ir.WriteMemory32(addr, val, acc);
    };
    for (size_t i = 0; i <= 14; i++) {
        if (bits::get_bit(i, list)) store_word(ir.GetRegister(static_cast<Reg>(i)));
    }
    if (bits::get_bit<15>(list)) store_word(ir.Imm32(ir.PC()));
    if (W) {
        ir.SetRegister(n, writeback_address);
    }
    return true;
}

// STM <Rn>{!}, <reg_list>
bool arm_STM(Cond cond, bool W, Reg n, RegList list) {
    if (n == Reg::PC || bits::count_ones(list) < 1) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto start_address = ir.GetRegister(n);
    const auto writeback_address = ir.Add(start_address, ir.Imm32(u32(bits::count_ones(list) * 4)));
    return load_store_STMHelper(ir, W, n, list, start_address, writeback_address);
}

// STMDA <Rn>{!}, <reg_list>
bool arm_STMDA(Cond cond, bool W, Reg n, RegList list) {
    if (n == Reg::PC || bits::count_ones(list) < 1) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto start_address =
            ir.Sub(ir.GetRegister(n), ir.Imm32(u32(4 * bits::count_ones(list) - 4)));
    const auto writeback_address = ir.Sub(start_address, ir.Imm32(4));
    return load_store_STMHelper(ir, W, n, list, start_address, writeback_address);
}

// STMDB <Rn>{!}, <reg_list>
bool arm_STMDB(Cond cond, bool W, Reg n, RegList list) {
    if (n == Reg::PC || bits::count_ones(list) < 1) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto start_address = ir.Sub(ir.GetRegister(n), ir.Imm32(u32(4 * bits::count_ones(list))));
    const auto writeback_address = start_address;
    return load_store_STMHelper(ir, W, n, list, start_address, writeback_address);
}

// STMIB <Rn>{!}, <reg_list>
bool arm_STMIB(Cond cond, bool W, Reg n, RegList list) {
    if (n == Reg::PC || bits::count_ones(list) < 1) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto start_address = ir.Add(ir.GetRegister(n), ir.Imm32(4));
    const auto writeback_address =
            ir.Add(ir.GetRegister(n), ir.Imm32(u32(4 * bits::count_ones(list))));
    return load_store_STMHelper(ir, W, n, list, start_address, writeback_address);
}

bool arm_STM_usr() { return UndefinedInstruction(); }

// synchronization
/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

// CLREX
bool arm_CLREX() {
    ir.ClearExclusive();
    return true;
}

// SWP<c> <Rt>, <Rt2>, [<Rn>]
// TODO: UNDEFINED if current mode is Hypervisor
bool arm_SWP(Cond, Reg n, Reg t, Reg t2) {
    if (t == Reg::PC || t2 == Reg::PC || n == Reg::PC || n == t || n == t2)
        return UnpredictableInstruction();
    ir.SetRegister(t, ir.AtomicSwap(32, ir.GetRegister(n), ir.GetRegister(t2)));
    return true;
}

// SWPB<c> <Rt>, <Rt2>, [<Rn>]
// TODO: UNDEFINED if current mode is Hypervisor
bool arm_SWPB(Cond, Reg n, Reg t, Reg t2) {
    if (t == Reg::PC || t2 == Reg::PC || n == Reg::PC || n == t || n == t2)
        return UnpredictableInstruction();
    ir.SetRegister(t, ir.AtomicSwap(8, ir.GetRegister(n), ir.GetRegister(t2)));
    return true;
}

// LDA<c> <Rt>, [<Rn>]
bool arm_LDA(Cond cond, Reg n, Reg t) {
    if (t == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto address = ir.GetRegister(n);
    ir.SetRegister(t, ir.ReadMemory32(address, AccType::ORDERED));
    return true;
}

// LDAB<c> <Rt>, [<Rn>]
bool arm_LDAB(Cond cond, Reg n, Reg t) {
    if (t == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto address = ir.GetRegister(n);
    ir.SetRegister(t, ir.ZeroExtendToWord(ir.ReadMemory8(address, AccType::ORDERED)));
    return true;
}

// LDAH<c> <Rt>, [<Rn>]
bool arm_LDAH(Cond cond, Reg n, Reg t) {
    if (t == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto address = ir.GetRegister(n);
    ir.SetRegister(t, ir.ZeroExtendToWord(ir.ReadMemory16(address, AccType::ORDERED)));
    return true;
}

// LDAEX<c> <Rt>, [<Rn>]
bool arm_LDAEX(Cond cond, Reg n, Reg t) { return ExclusiveLoad(2, n, t, Reg::R0, 0, true); }

// LDAEXB<c> <Rt>, [<Rn>]
bool arm_LDAEXB(Cond cond, Reg n, Reg t) { return ExclusiveLoad(0, n, t, Reg::R0, 0, true); }

// LDAEXD<c> <Rt>, <Rt2>, [<Rn>]
bool arm_LDAEXD(Cond cond, Reg n, Reg t) {
    if (static_cast<unsigned>(t) & 1) return UnpredictableInstruction();
    return ExclusiveLoad(3, n, t, t + 1, 0, true);
}

// LDAEXH<c> <Rt>, [<Rn>]
bool arm_LDAEXH(Cond cond, Reg n, Reg t) { return ExclusiveLoad(1, n, t, Reg::R0, 0, true); }

// STL<c> <Rt>, [<Rn>]
bool arm_STL(Cond cond, Reg n, Reg t) {
    if (t == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto address = ir.GetRegister(n);
    ir.WriteMemory32(address, ir.GetRegister(t), AccType::ORDERED);
    return true;
}

// STLB<c> <Rt>, [<Rn>]
bool arm_STLB(Cond cond, Reg n, Reg t) {
    if (t == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto address = ir.GetRegister(n);
    ir.WriteMemory8(address, ir.LeastSignificantByte(ir.GetRegister(t)), AccType::ORDERED);
    return true;
}

// STLH<c> <Rd>, <Rt>, [<Rn>]
bool arm_STLH(Cond cond, Reg n, Reg t) {
    if (t == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const auto address = ir.GetRegister(n);
    ir.WriteMemory16(address, ir.LeastSignificantHalf(ir.GetRegister(t)), AccType::ORDERED);
    return true;
}

// STLEXB<c> <Rd>, <Rt>, [<Rn>]
bool arm_STLEXB(Cond cond, Reg n, Reg d, Reg t) {
    return ExclusiveStore(0, n, d, t, Reg::R0, 0, true);
}

// STLEXD<c> <Rd>, <Rt>, <Rt2>, [<Rn>]
bool arm_STLEXD(Cond cond, Reg n, Reg d, Reg t) {
    if (static_cast<unsigned>(t) & 1) return UnpredictableInstruction();
    return ExclusiveStore(3, n, d, t, t + 1, 0, true);
}

// STLEXH<c> <Rd>, <Rt>, [<Rn>]
bool arm_STLEXH(Cond cond, Reg n, Reg d, Reg t) {
    return ExclusiveStore(1, n, d, t, Reg::R0, 0, true);
}

// STLEX<c> <Rd>, <Rt>, [<Rn>]
bool arm_STLEX(Cond cond, Reg n, Reg d, Reg t) {
    return ExclusiveStore(2, n, d, t, Reg::R0, 0, true);
}

// LDREX<c> <Rt>, [<Rn>]
bool arm_LDREX(Cond cond, Reg n, Reg t) { return ExclusiveLoad(2, n, t, Reg::R0, 0, false); }

// LDREXB<c> <Rt>, [<Rn>]
bool arm_LDREXB(Cond cond, Reg n, Reg t) { return ExclusiveLoad(0, n, t, Reg::R0, 0, false); }

// LDREXD<c> <Rt>, <Rt2>, [<Rn>]
bool arm_LDREXD(Cond cond, Reg n, Reg t) {
    if (static_cast<unsigned>(t) & 1) return UnpredictableInstruction();
    return ExclusiveLoad(3, n, t, t + 1, 0, false);
}

// LDREXH<c> <Rt>, [<Rn>]
bool arm_LDREXH(Cond cond, Reg n, Reg t) { return ExclusiveLoad(1, n, t, Reg::R0, 0, false); }

// STREX<c> <Rd>, <Rt>, [<Rn>]
bool arm_STREX(Cond cond, Reg n, Reg d, Reg t) {
    return ExclusiveStore(2, n, d, t, Reg::R0, 0, false);
}

// STREXB<c> <Rd>, <Rt>, [<Rn>]
bool arm_STREXB(Cond cond, Reg n, Reg d, Reg t) {
    return ExclusiveStore(0, n, d, t, Reg::R0, 0, false);
}

// STREXD<c> <Rd>, <Rt>, <Rt2>, [<Rn>]
bool arm_STREXD(Cond cond, Reg n, Reg d, Reg t) {
    if (static_cast<unsigned>(t) & 1) return UnpredictableInstruction();
    return ExclusiveStore(3, n, d, t, t + 1, 0, false);
}

// STREXH<c> <Rd>, <Rt>, [<Rn>]
bool arm_STREXH(Cond cond, Reg n, Reg d, Reg t) {
    return ExclusiveStore(1, n, d, t, Reg::R0, 0, false);
}

// thumb32_load_byte
/* This file is part of the dynarmic project.
 * Copyright (c) 2021 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

static bool thumb32_load_byte_PLDHandler(Semantics& v, bool W) {
    if (!v.options.hook_hint_instructions) {
        return true;
    }

    const auto exception = W ? Exception::PreloadDataWithIntentToWrite : Exception::PreloadData;
    return v.RaiseException(exception);
}

static bool thumb32_load_byte_PLIHandler(Semantics& v) {
    if (!v.options.hook_hint_instructions) {
        return true;
    }

    return v.RaiseException(Exception::PreloadInstruction);
}

using thumb32_load_byte_ExtensionFunction = Value (Semantics::*)(Value);

static bool thumb32_load_byte_LoadByteLiteral(
        Semantics& v, bool U, Reg t, Imm<12> imm12, thumb32_load_byte_ExtensionFunction ext_fn) {
    const u32 imm32 = imm12.ZeroExtend();
    const u32 base = v.ir.AlignPC(4);
    const u32 address = U ? (base + imm32) : (base - imm32);
    const auto data = (v.ir.*ext_fn)(v.ir.ReadMemory8(v.ir.Imm32(address), AccType::NORMAL));

    v.ir.SetRegister(t, data);
    return true;
}

static bool thumb32_load_byte_LoadByteRegister(Semantics& v,
                                               Reg n,
                                               Reg t,
                                               Imm<2> imm2,
                                               Reg m,
                                               thumb32_load_byte_ExtensionFunction ext_fn) {
    if (m == Reg::PC) {
        return v.UnpredictableInstruction();
    }

    const auto reg_n = v.ir.GetRegister(n);
    const auto reg_m = v.ir.GetRegister(m);
    const auto offset = v.ir.LogicalShiftLeft(reg_m, v.ir.Imm8(imm2.ZeroExtend<u8>()));
    const auto address = v.ir.Add(reg_n, offset);
    const auto data = (v.ir.*ext_fn)(v.ir.ReadMemory8(address, AccType::NORMAL));

    v.ir.SetRegister(t, data);
    return true;
}

static bool thumb32_load_byte_LoadByteImmediate(Semantics& v,
                                                Reg n,
                                                Reg t,
                                                bool P,
                                                bool U,
                                                bool W,
                                                Imm<12> imm12,
                                                thumb32_load_byte_ExtensionFunction ext_fn) {
    const u32 imm32 = imm12.ZeroExtend();
    const Value reg_n = v.ir.GetRegister(n);
    const Value offset_address =
            U ? v.ir.Add(reg_n, v.ir.Imm32(imm32)) : v.ir.Sub(reg_n, v.ir.Imm32(imm32));
    const Value address = P ? offset_address : reg_n;
    const Value data = (v.ir.*ext_fn)(v.ir.ReadMemory8(address, AccType::NORMAL));

    v.ir.SetRegister(t, data);
    if (W) {
        v.ir.SetRegister(n, offset_address);
    }
    return true;
}

bool thumb32_PLD_lit(bool /*U*/, Imm<12> /*imm12*/) {
    return thumb32_load_byte_PLDHandler(*this, false);
}

bool thumb32_PLD_imm8(bool W, Reg /*n*/, Imm<8> /*imm8*/) {
    return thumb32_load_byte_PLDHandler(*this, W);
}

bool thumb32_PLD_imm12(bool W, Reg /*n*/, Imm<12> /*imm12*/) {
    return thumb32_load_byte_PLDHandler(*this, W);
}

bool thumb32_PLD_reg(bool W, Reg /*n*/, Imm<2> /*imm2*/, Reg m) {
    if (m == Reg::PC) {
        return UnpredictableInstruction();
    }

    return thumb32_load_byte_PLDHandler(*this, W);
}

bool thumb32_PLI_lit(bool /*U*/, Imm<12> /*imm12*/) { return thumb32_load_byte_PLIHandler(*this); }

bool thumb32_PLI_imm8(Reg /*n*/, Imm<8> /*imm8*/) { return thumb32_load_byte_PLIHandler(*this); }

bool thumb32_PLI_imm12(Reg /*n*/, Imm<12> /*imm12*/) { return thumb32_load_byte_PLIHandler(*this); }

bool thumb32_PLI_reg(Reg /*n*/, Imm<2> /*imm2*/, Reg m) {
    if (m == Reg::PC) {
        return UnpredictableInstruction();
    }

    return thumb32_load_byte_PLIHandler(*this);
}

bool thumb32_LDRB_lit(bool U, Reg t, Imm<12> imm12) {
    return thumb32_load_byte_LoadByteLiteral(*this, U, t, imm12, &Semantics::ZeroExtendByteToWord);
}

bool thumb32_LDRB_imm8(Reg n, Reg t, bool P, bool U, bool W, Imm<8> imm8) {
    if (t == Reg::PC && W) {
        return UnpredictableInstruction();
    }
    if (W && n == t) {
        return UnpredictableInstruction();
    }
    if (!P && !W) {
        return UndefinedInstruction();
    }

    return thumb32_load_byte_LoadByteImmediate(
            *this, n, t, P, U, W, Imm<12>{imm8.ZeroExtend()}, &Semantics::ZeroExtendByteToWord);
}

bool thumb32_LDRB_imm12(Reg n, Reg t, Imm<12> imm12) {
    return thumb32_load_byte_LoadByteImmediate(
            *this, n, t, true, true, false, imm12, &Semantics::ZeroExtendByteToWord);
}

bool thumb32_LDRB_reg(Reg n, Reg t, Imm<2> imm2, Reg m) {
    return thumb32_load_byte_LoadByteRegister(
            *this, n, t, imm2, m, &Semantics::ZeroExtendByteToWord);
}

bool thumb32_LDRBT(Reg n, Reg t, Imm<8> imm8) {
    // TODO: Add an unpredictable instruction path if this
    //       is executed in hypervisor mode if we ever support
    //       privileged execution modes.

    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }

    // Treat it as a normal LDRB, given we don't support
    // execution levels other than EL0 currently.
    return thumb32_LDRB_imm8(n, t, true, true, false, imm8);
}

bool thumb32_LDRSB_lit(bool U, Reg t, Imm<12> imm12) {
    return thumb32_load_byte_LoadByteLiteral(*this, U, t, imm12, &Semantics::SignExtendByteToWord);
}

bool thumb32_LDRSB_imm8(Reg n, Reg t, bool P, bool U, bool W, Imm<8> imm8) {
    if (t == Reg::PC && W) {
        return UnpredictableInstruction();
    }
    if (W && n == t) {
        return UnpredictableInstruction();
    }
    if (!P && !W) {
        return UndefinedInstruction();
    }

    return thumb32_load_byte_LoadByteImmediate(
            *this, n, t, P, U, W, Imm<12>{imm8.ZeroExtend()}, &Semantics::SignExtendByteToWord);
}

bool thumb32_LDRSB_imm12(Reg n, Reg t, Imm<12> imm12) {
    return thumb32_load_byte_LoadByteImmediate(
            *this, n, t, true, true, false, imm12, &Semantics::SignExtendByteToWord);
}

bool thumb32_LDRSB_reg(Reg n, Reg t, Imm<2> imm2, Reg m) {
    return thumb32_load_byte_LoadByteRegister(
            *this, n, t, imm2, m, &Semantics::SignExtendByteToWord);
}

bool thumb32_LDRSBT(Reg n, Reg t, Imm<8> imm8) {
    // TODO: Add an unpredictable instruction path if this
    //       is executed in hypervisor mode if we ever support
    //       privileged execution modes.

    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }

    // Treat it as a normal LDRSB, given we don't support
    // execution levels other than EL0 currently.
    return thumb32_LDRSB_imm8(n, t, true, true, false, imm8);
}

// thumb32_load_halfword
/* This file is part of the dynarmic project.
 * Copyright (c) 2021 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

using thumb32_load_halfword_ExtensionFunction = Value (Semantics::*)(Value);

static bool thumb32_load_halfword_LoadHalfLiteral(Semantics& v,
                                                  bool U,
                                                  Reg t,
                                                  Imm<12> imm12,
                                                  thumb32_load_halfword_ExtensionFunction ext_fn) {
    const auto imm32 = imm12.ZeroExtend();
    const auto base = v.ir.AlignPC(4);
    const auto address = U ? (base + imm32) : (base - imm32);
    const auto data = (v.ir.*ext_fn)(v.ir.ReadMemory16(v.ir.Imm32(address), AccType::NORMAL));

    v.ir.SetRegister(t, data);
    return true;
}

static bool thumb32_load_halfword_LoadHalfRegister(Semantics& v,
                                                   Reg n,
                                                   Reg t,
                                                   Imm<2> imm2,
                                                   Reg m,
                                                   thumb32_load_halfword_ExtensionFunction ext_fn) {
    if (m == Reg::PC) {
        return v.UnpredictableInstruction();
    }

    const Value reg_m = v.ir.GetRegister(m);
    const Value reg_n = v.ir.GetRegister(n);
    const Value offset = v.ir.LogicalShiftLeft(reg_m, v.ir.Imm8(imm2.ZeroExtend<u8>()));
    const Value address = v.ir.Add(reg_n, offset);
    const Value data = (v.ir.*ext_fn)(v.ir.ReadMemory16(address, AccType::NORMAL));

    v.ir.SetRegister(t, data);
    return true;
}

static bool thumb32_load_halfword_LoadHalfImmediate(
        Semantics& v,
        Reg n,
        Reg t,
        bool P,
        bool U,
        bool W,
        Imm<12> imm12,
        thumb32_load_halfword_ExtensionFunction ext_fn) {
    const u32 imm32 = imm12.ZeroExtend();
    const Value reg_n = v.ir.GetRegister(n);
    const Value offset_address =
            U ? v.ir.Add(reg_n, v.ir.Imm32(imm32)) : v.ir.Sub(reg_n, v.ir.Imm32(imm32));
    const Value address = P ? offset_address : reg_n;
    const Value data = (v.ir.*ext_fn)(v.ir.ReadMemory16(address, AccType::NORMAL));

    if (W) {
        v.ir.SetRegister(n, offset_address);
    }

    v.ir.SetRegister(t, data);
    return true;
}

bool thumb32_LDRH_lit(bool U, Reg t, Imm<12> imm12) {
    return thumb32_load_halfword_LoadHalfLiteral(
            *this, U, t, imm12, &Semantics::ZeroExtendHalfToWord);
}

bool thumb32_LDRH_reg(Reg n, Reg t, Imm<2> imm2, Reg m) {
    return thumb32_load_halfword_LoadHalfRegister(
            *this, n, t, imm2, m, &Semantics::ZeroExtendHalfToWord);
}

bool thumb32_LDRH_imm8(Reg n, Reg t, bool P, bool U, bool W, Imm<8> imm8) {
    if (!P && !W) {
        return UndefinedInstruction();
    }
    if (t == Reg::PC && W) {
        return UnpredictableInstruction();
    }
    if (W && n == t) {
        return UnpredictableInstruction();
    }

    return thumb32_load_halfword_LoadHalfImmediate(
            *this, n, t, P, U, W, Imm<12>{imm8.ZeroExtend()}, &Semantics::ZeroExtendHalfToWord);
}

bool thumb32_LDRH_imm12(Reg n, Reg t, Imm<12> imm12) {
    return thumb32_load_halfword_LoadHalfImmediate(
            *this, n, t, true, true, false, imm12, &Semantics::ZeroExtendHalfToWord);
}

bool thumb32_LDRHT(Reg n, Reg t, Imm<8> imm8) {
    // TODO: Add an unpredictable instruction path if this
    //       is executed in hypervisor mode if we ever support
    //       privileged execution levels.

    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }

    // Treat it as a normal LDRH, given we don't support
    // execution levels other than EL0 currently.
    return thumb32_LDRH_imm8(n, t, true, true, false, imm8);
}

bool thumb32_LDRSH_lit(bool U, Reg t, Imm<12> imm12) {
    return thumb32_load_halfword_LoadHalfLiteral(
            *this, U, t, imm12, &Semantics::SignExtendHalfToWord);
}

bool thumb32_LDRSH_reg(Reg n, Reg t, Imm<2> imm2, Reg m) {
    return thumb32_load_halfword_LoadHalfRegister(
            *this, n, t, imm2, m, &Semantics::SignExtendHalfToWord);
}

bool thumb32_LDRSH_imm8(Reg n, Reg t, bool P, bool U, bool W, Imm<8> imm8) {
    if (!P && !W) {
        return UndefinedInstruction();
    }
    if (t == Reg::PC && W) {
        return UnpredictableInstruction();
    }
    if (W && n == t) {
        return UnpredictableInstruction();
    }

    return thumb32_load_halfword_LoadHalfImmediate(
            *this, n, t, P, U, W, Imm<12>{imm8.ZeroExtend()}, &Semantics::SignExtendHalfToWord);
}

bool thumb32_LDRSH_imm12(Reg n, Reg t, Imm<12> imm12) {
    return thumb32_load_halfword_LoadHalfImmediate(
            *this, n, t, true, true, false, imm12, &Semantics::SignExtendHalfToWord);
}

bool thumb32_LDRSHT(Reg n, Reg t, Imm<8> imm8) {
    // TODO: Add an unpredictable instruction path if this
    //       is executed in hypervisor mode if we ever support
    //       privileged execution levels.

    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }

    // Treat it as a normal LDRSH, given we don't support
    // execution levels other than EL0 currently.
    return thumb32_LDRSH_imm8(n, t, true, true, false, imm8);
}

// thumb32_load_word
/* This file is part of the dynarmic project.
 * Copyright (c) 2021 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

static bool thumb32_load_word_ITBlockCheck(const Semantics& ir) {
    return ir.current_location.IT().IsInITBlock() && !ir.current_location.IT().IsLastInITBlock();
}

bool thumb32_LDR_lit(bool U, Reg t, Imm<12> imm12) {
    if (t == Reg::PC && thumb32_load_word_ITBlockCheck(ir)) {
        return UnpredictableInstruction();
    }

    const u32 imm32 = imm12.ZeroExtend();
    const u32 base = ir.AlignPC(4);
    const u32 address = U ? base + imm32 : base - imm32;
    const auto data = ir.ReadMemory32(ir.Imm32(address), AccType::NORMAL);

    if (t == Reg::PC) {
        ir.UpdateUpperLocationDescriptor();
        ir.LoadWritePC(data);
        ir.SetTerm(Term::FastDispatchHint{});
        return false;
    }

    ir.SetRegister(t, data);
    return true;
}

bool thumb32_LDR_imm8(Reg n, Reg t, bool P, bool U, bool W, Imm<8> imm8) {
    if (!P && !W) {
        return UndefinedInstruction();
    }
    if (W && n == t) {
        return UnpredictableInstruction();
    }
    if (t == Reg::PC && thumb32_load_word_ITBlockCheck(ir)) {
        return UnpredictableInstruction();
    }

    const u32 imm32 = imm8.ZeroExtend();
    const Value reg_n = ir.GetRegister(n);
    const Value offset_address =
            U ? ir.Add(reg_n, ir.Imm32(imm32)) : ir.Sub(reg_n, ir.Imm32(imm32));
    const Value address = P ? offset_address : reg_n;
    const Value data = ir.ReadMemory32(address, AccType::NORMAL);

    if (W) {
        ir.SetRegister(n, offset_address);
    }

    if (t == Reg::PC) {
        ir.UpdateUpperLocationDescriptor();
        ir.LoadWritePC(data);

        if (!P && W && n == Reg::R13) {
            ir.SetTerm(Term::PopRSBHint{});
        } else {
            ir.SetTerm(Term::FastDispatchHint{});
        }

        return false;
    }

    ir.SetRegister(t, data);
    return true;
}

bool thumb32_LDR_imm12(Reg n, Reg t, Imm<12> imm12) {
    if (t == Reg::PC && thumb32_load_word_ITBlockCheck(ir)) {
        return UnpredictableInstruction();
    }

    const auto imm32 = imm12.ZeroExtend();
    const auto reg_n = ir.GetRegister(n);
    const auto address = ir.Add(reg_n, ir.Imm32(imm32));
    const auto data = ir.ReadMemory32(address, AccType::NORMAL);

    if (t == Reg::PC) {
        ir.UpdateUpperLocationDescriptor();
        ir.LoadWritePC(data);
        ir.SetTerm(Term::FastDispatchHint{});
        return false;
    }

    ir.SetRegister(t, data);
    return true;
}

bool thumb32_LDR_reg(Reg n, Reg t, Imm<2> imm2, Reg m) {
    if (m == Reg::PC) {
        return UnpredictableInstruction();
    }
    if (t == Reg::PC && thumb32_load_word_ITBlockCheck(ir)) {
        return UnpredictableInstruction();
    }

    const auto reg_m = ir.GetRegister(m);
    const auto reg_n = ir.GetRegister(n);
    const auto offset = ir.LogicalShiftLeft(reg_m, ir.Imm8(imm2.ZeroExtend<u8>()));
    const auto address = ir.Add(reg_n, offset);
    const auto data = ir.ReadMemory32(address, AccType::NORMAL);

    if (t == Reg::PC) {
        ir.UpdateUpperLocationDescriptor();
        ir.LoadWritePC(data);
        ir.SetTerm(Term::FastDispatchHint{});
        return false;
    }

    ir.SetRegister(t, data);
    return true;
}

bool thumb32_LDRT(Reg n, Reg t, Imm<8> imm8) {
    // TODO: Add an unpredictable instruction path if this
    //       is executed in hypervisor mode if we ever support
    //       privileged execution levels.

    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }

    // Treat it as a normal LDR, given we don't support
    // execution levels other than EL0 currently.
    return thumb32_LDR_imm8(n, t, true, true, false, imm8);
}

// thumb32_store_single_data_item
/* This file is part of the dynarmic project.
 * Copyright (c) 2021 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

template <typename StoreRegFn> static bool thumb32_store_single_data_item_StoreRegister(
        Semantics& v, Reg n, Reg t, Imm<2> imm2, Reg m, StoreRegFn store_fn) {
    if (n == Reg::PC) {
        return v.UndefinedInstruction();
    }

    if (t == Reg::PC || m == Reg::PC) {
        return v.UnpredictableInstruction();
    }

    const auto reg_m = v.ir.GetRegister(m);
    const auto reg_n = v.ir.GetRegister(n);
    const auto reg_t = v.ir.GetRegister(t);

    const auto shift_amount = v.ir.Imm8(static_cast<u8>(imm2.ZeroExtend()));
    const auto offset = v.ir.LogicalShiftLeft(reg_m, shift_amount);
    const auto offset_address = v.ir.Add(reg_n, offset);

    store_fn(offset_address, reg_t);
    return true;
}

using thumb32_store_single_data_item_StoreImmFn = void (*)(Semantics&, const Value&, const Value&);

static void thumb32_store_single_data_item_StoreImmByteFn(Semantics& v,
                                                          const Value& address,
                                                          const Value& data) {
    v.ir.WriteMemory8(address, v.ir.LeastSignificantByte(data), AccType::NORMAL);
}

static void thumb32_store_single_data_item_StoreImmHalfFn(Semantics& v,
                                                          const Value& address,
                                                          const Value& data) {
    v.ir.WriteMemory16(address, v.ir.LeastSignificantHalf(data), AccType::NORMAL);
}

static void thumb32_store_single_data_item_StoreImmWordFn(Semantics& v,
                                                          const Value& address,
                                                          const Value& data) {
    v.ir.WriteMemory32(address, data, AccType::NORMAL);
}

static bool thumb32_store_single_data_item_StoreImmediate(
        Semantics& v,
        Reg n,
        Reg t,
        bool P,
        bool U,
        bool W,
        Imm<12> imm12,
        thumb32_store_single_data_item_StoreImmFn store_fn) {
    const auto imm32 = imm12.ZeroExtend();
    const auto reg_n = v.ir.GetRegister(n);
    const auto reg_t = v.ir.GetRegister(t);

    const Value offset_address =
            U ? v.ir.Add(reg_n, v.ir.Imm32(imm32)) : v.ir.Sub(reg_n, v.ir.Imm32(imm32));
    const Value address = P ? offset_address : reg_n;

    store_fn(v, address, reg_t);
    if (W) {
        v.ir.SetRegister(n, offset_address);
    }

    return true;
}

bool thumb32_STRB_imm_1(Reg n, Reg t, bool P, bool U, Imm<8> imm8) {
    if (n == Reg::PC) {
        return UndefinedInstruction();
    }
    if (t == Reg::PC || n == t) {
        return UnpredictableInstruction();
    }
    return thumb32_store_single_data_item_StoreImmediate(
            *this,
            n,
            t,
            P,
            U,
            true,
            Imm<12>{imm8.ZeroExtend()},
            thumb32_store_single_data_item_StoreImmByteFn);
}

bool thumb32_STRB_imm_2(Reg n, Reg t, Imm<8> imm8) {
    if (n == Reg::PC) {
        return UndefinedInstruction();
    }
    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }
    return thumb32_store_single_data_item_StoreImmediate(
            *this,
            n,
            t,
            true,
            false,
            false,
            Imm<12>{imm8.ZeroExtend()},
            thumb32_store_single_data_item_StoreImmByteFn);
}

bool thumb32_STRB_imm_3(Reg n, Reg t, Imm<12> imm12) {
    if (n == Reg::PC) {
        return UndefinedInstruction();
    }
    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }
    return thumb32_store_single_data_item_StoreImmediate(
            *this, n, t, true, true, false, imm12, thumb32_store_single_data_item_StoreImmByteFn);
}

bool thumb32_STRBT(Reg n, Reg t, Imm<8> imm8) {
    // TODO: Add an unpredictable instruction path if this
    //       is executed in hypervisor mode if we ever support
    //       privileged execution levels.

    if (n == Reg::PC) {
        return UndefinedInstruction();
    }
    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }

    // Treat this as a normal STRB, given we don't support
    // execution levels other than EL0 currently.
    return thumb32_store_single_data_item_StoreImmediate(
            *this,
            n,
            t,
            true,
            true,
            false,
            Imm<12>{imm8.ZeroExtend()},
            thumb32_store_single_data_item_StoreImmByteFn);
}

bool thumb32_STRB(Reg n, Reg t, Imm<2> imm2, Reg m) {
    return thumb32_store_single_data_item_StoreRegister(
            *this, n, t, imm2, m, [this](const Value& offset_address, const Value& data) {
                ir.WriteMemory8(offset_address, ir.LeastSignificantByte(data), AccType::NORMAL);
            });
}

bool thumb32_STRH_imm_1(Reg n, Reg t, bool P, bool U, Imm<8> imm8) {
    if (n == Reg::PC) {
        return UndefinedInstruction();
    }
    if (t == Reg::PC || n == t) {
        return UnpredictableInstruction();
    }
    return thumb32_store_single_data_item_StoreImmediate(
            *this,
            n,
            t,
            P,
            U,
            true,
            Imm<12>{imm8.ZeroExtend()},
            thumb32_store_single_data_item_StoreImmHalfFn);
}

bool thumb32_STRH_imm_2(Reg n, Reg t, Imm<8> imm8) {
    if (n == Reg::PC) {
        return UndefinedInstruction();
    }
    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }
    return thumb32_store_single_data_item_StoreImmediate(
            *this,
            n,
            t,
            true,
            false,
            false,
            Imm<12>{imm8.ZeroExtend()},
            thumb32_store_single_data_item_StoreImmHalfFn);
}

bool thumb32_STRH_imm_3(Reg n, Reg t, Imm<12> imm12) {
    if (n == Reg::PC) {
        return UndefinedInstruction();
    }
    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }
    return thumb32_store_single_data_item_StoreImmediate(
            *this, n, t, true, true, false, imm12, thumb32_store_single_data_item_StoreImmHalfFn);
}

bool thumb32_STRHT(Reg n, Reg t, Imm<8> imm8) {
    // TODO: Add an unpredictable instruction path if this
    //       is executed in hypervisor mode if we ever support
    //       privileged execution levels.

    if (n == Reg::PC) {
        return UndefinedInstruction();
    }
    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }

    // Treat this as a normal STRH, given we don't support
    // execution levels other than EL0 currently.
    return thumb32_store_single_data_item_StoreImmediate(
            *this,
            n,
            t,
            true,
            true,
            false,
            Imm<12>{imm8.ZeroExtend()},
            thumb32_store_single_data_item_StoreImmHalfFn);
}

bool thumb32_STRH(Reg n, Reg t, Imm<2> imm2, Reg m) {
    return thumb32_store_single_data_item_StoreRegister(
            *this, n, t, imm2, m, [this](const Value& offset_address, const Value& data) {
                ir.WriteMemory16(offset_address, ir.LeastSignificantHalf(data), AccType::NORMAL);
            });
}

bool thumb32_STR_imm_1(Reg n, Reg t, bool P, bool U, Imm<8> imm8) {
    if (n == Reg::PC) {
        return UndefinedInstruction();
    }
    if (t == Reg::PC || n == t) {
        return UnpredictableInstruction();
    }
    return thumb32_store_single_data_item_StoreImmediate(
            *this,
            n,
            t,
            P,
            U,
            true,
            Imm<12>{imm8.ZeroExtend()},
            thumb32_store_single_data_item_StoreImmWordFn);
}

bool thumb32_STR_imm_2(Reg n, Reg t, Imm<8> imm8) {
    if (n == Reg::PC) {
        return UndefinedInstruction();
    }
    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }
    return thumb32_store_single_data_item_StoreImmediate(
            *this,
            n,
            t,
            true,
            false,
            false,
            Imm<12>{imm8.ZeroExtend()},
            thumb32_store_single_data_item_StoreImmWordFn);
}

bool thumb32_STR_imm_3(Reg n, Reg t, Imm<12> imm12) {
    if (n == Reg::PC) {
        return UndefinedInstruction();
    }
    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }
    return thumb32_store_single_data_item_StoreImmediate(
            *this, n, t, true, true, false, imm12, thumb32_store_single_data_item_StoreImmWordFn);
}

bool thumb32_STRT(Reg n, Reg t, Imm<8> imm8) {
    // TODO: Add an unpredictable instruction path if this
    //       is executed in hypervisor mode if we ever support
    //       privileged execution levels.

    if (n == Reg::PC) {
        return UndefinedInstruction();
    }
    if (t == Reg::PC) {
        return UnpredictableInstruction();
    }

    // Treat this as a normal STR, given we don't support
    // execution levels other than EL0 currently.
    return thumb32_store_single_data_item_StoreImmediate(
            *this,
            n,
            t,
            true,
            true,
            false,
            Imm<12>{imm8.ZeroExtend()},
            thumb32_store_single_data_item_StoreImmWordFn);
}

bool thumb32_STR_reg(Reg n, Reg t, Imm<2> imm2, Reg m) {
    return thumb32_store_single_data_item_StoreRegister(
            *this, n, t, imm2, m, [this](const Value& offset_address, const Value& data) {
                ir.WriteMemory32(offset_address, data, AccType::NORMAL);
            });
}

// thumb32_load_store_dual
/* This file is part of the dynarmic project.
 * Copyright (c) 2021 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

static bool thumb32_load_store_dual_ITBlockCheck(const Semantics& ir) {
    return ir.current_location.IT().IsInITBlock() && !ir.current_location.IT().IsLastInITBlock();
}

static bool thumb32_load_store_dual_TableBranch(Semantics& v, Reg n, Reg m, bool half) {
    if (m == Reg::PC) {
        return v.UnpredictableInstruction();
    }
    if (thumb32_load_store_dual_ITBlockCheck(v.ir)) {
        return v.UnpredictableInstruction();
    }

    const auto reg_m = v.ir.GetRegister(m);
    const auto reg_n = v.ir.GetRegister(n);

    Value halfwords;
    if (half) {
        const auto data = v.ir.ReadMemory16(
                v.ir.Add(reg_n, v.ir.LogicalShiftLeft(reg_m, v.ir.Imm8(1))), AccType::NORMAL);
        halfwords = v.ir.ZeroExtendToWord(data);
    } else {
        halfwords =
                v.ir.ZeroExtendToWord(v.ir.ReadMemory8(v.ir.Add(reg_n, reg_m), AccType::NORMAL));
    }

    const auto current_pc = v.ir.Imm32(v.ir.PC());
    const auto branch_value = v.ir.Add(current_pc, v.ir.Add(halfwords, halfwords));

    v.ir.UpdateUpperLocationDescriptor();
    v.ir.BranchWritePC(branch_value);
    v.ir.SetTerm(Term::FastDispatchHint{});
    return false;
}

static bool thumb32_load_store_dual_LoadDualImmediate(
        Semantics& v, bool P, bool U, bool W, Reg n, Reg t, Reg t2, Imm<8> imm8) {
    if (W && (n == t || n == t2)) {
        return v.UnpredictableInstruction();
    }
    if (t == Reg::PC || t2 == Reg::PC || t == t2) {
        return v.UnpredictableInstruction();
    }

    const u32 imm = imm8.ZeroExtend() << 2;
    const Value reg_n = v.ir.GetRegister(n);
    const Value offset_address =
            U ? v.ir.Add(reg_n, v.ir.Imm32(imm)) : v.ir.Sub(reg_n, v.ir.Imm32(imm));
    const Value address = P ? offset_address : reg_n;

    v.ir.LoadDual(address, t, t2);

    if (W) {
        v.ir.SetRegister(n, offset_address);
    }
    return true;
}

static bool thumb32_load_store_dual_LoadDualLiteral(
        Semantics& v, bool U, bool W, Reg t, Reg t2, Imm<8> imm8) {
    if (t == Reg::PC || t2 == Reg::PC || t == t2) {
        return v.UnpredictableInstruction();
    }
    if (W) {
        return v.UnpredictableInstruction();
    }

    const auto imm = imm8.ZeroExtend() << 2;
    const auto address = U ? v.ir.Add(v.ir.Imm32(v.ir.AlignPC(4)), v.ir.Imm32(imm))
                           : v.ir.Sub(v.ir.Imm32(v.ir.AlignPC(4)), v.ir.Imm32(imm));

    v.ir.LoadDual(address, t, t2);

    return true;
}

static bool thumb32_load_store_dual_StoreDual(
        Semantics& v, bool P, bool U, bool W, Reg n, Reg t, Reg t2, Imm<8> imm8) {
    if (W && (n == t || n == t2)) {
        return v.UnpredictableInstruction();
    }
    if (n == Reg::PC || t == Reg::PC || t2 == Reg::PC) {
        return v.UnpredictableInstruction();
    }

    const u32 imm = imm8.ZeroExtend() << 2;
    const Value reg_n = v.ir.GetRegister(n);
    const Value offset_address =
            U ? v.ir.Add(reg_n, v.ir.Imm32(imm)) : v.ir.Sub(reg_n, v.ir.Imm32(imm));
    const Value address = P ? offset_address : reg_n;
    v.ir.StoreDual(address, t, t2);

    if (W) {
        v.ir.SetRegister(n, offset_address);
    }
    return true;
}

bool thumb32_LDA(Reg n, Reg t) {
    if (t == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto address = ir.GetRegister(n);
    ir.SetRegister(t, ir.ReadMemory32(address, AccType::ORDERED));
    return true;
}

bool thumb32_LDRD_imm_1(bool U, Reg n, Reg t, Reg t2, Imm<8> imm8) {
    return thumb32_load_store_dual_LoadDualImmediate(*this, false, U, true, n, t, t2, imm8);
}

bool thumb32_LDRD_imm_2(bool U, bool W, Reg n, Reg t, Reg t2, Imm<8> imm8) {
    return thumb32_load_store_dual_LoadDualImmediate(*this, true, U, W, n, t, t2, imm8);
}

bool thumb32_LDRD_lit_1(bool U, Reg t, Reg t2, Imm<8> imm8) {
    return thumb32_load_store_dual_LoadDualLiteral(*this, U, true, t, t2, imm8);
}

bool thumb32_LDRD_lit_2(bool U, bool W, Reg t, Reg t2, Imm<8> imm8) {
    return thumb32_load_store_dual_LoadDualLiteral(*this, U, W, t, t2, imm8);
}

bool thumb32_STRD_imm_1(bool U, Reg n, Reg t, Reg t2, Imm<8> imm8) {
    return thumb32_load_store_dual_StoreDual(*this, false, U, true, n, t, t2, imm8);
}

bool thumb32_STRD_imm_2(bool U, bool W, Reg n, Reg t, Reg t2, Imm<8> imm8) {
    return thumb32_load_store_dual_StoreDual(*this, true, U, W, n, t, t2, imm8);
}

bool thumb32_LDREX(Reg n, Reg t, Imm<8> imm8) {
    return ExclusiveLoad(2, n, t, Reg::R0, imm8.ZeroExtend(), false);
}

bool thumb32_LDREXB(Reg n, Reg t) { return ExclusiveLoad(0, n, t, Reg::R0, 0, false); }

bool thumb32_LDREXD(Reg n, Reg t, Reg t2) { return ExclusiveLoad(3, n, t, t2, 0, false); }

bool thumb32_LDREXH(Reg n, Reg t) { return ExclusiveLoad(1, n, t, Reg::R0, 0, false); }

bool thumb32_STL(Reg n, Reg t) {
    if (t == Reg::PC || n == Reg::PC) {
        return UnpredictableInstruction();
    }

    const auto address = ir.GetRegister(n);
    ir.WriteMemory32(address, ir.GetRegister(t), AccType::ORDERED);
    return true;
}

bool thumb32_STREX(Reg n, Reg t, Reg d, Imm<8> imm8) {
    return ExclusiveStore(2, n, d, t, Reg::R0, imm8.ZeroExtend(), false);
}

bool thumb32_STREXB(Reg n, Reg t, Reg d) { return ExclusiveStore(0, n, d, t, Reg::R0, 0, false); }

bool thumb32_STREXD(Reg n, Reg t, Reg t2, Reg d) {
    return ExclusiveStore(3, n, d, t, t2, 0, false);
}

bool thumb32_STREXH(Reg n, Reg t, Reg d) { return ExclusiveStore(1, n, d, t, Reg::R0, 0, false); }

bool thumb32_TBB(Reg n, Reg m) { return thumb32_load_store_dual_TableBranch(*this, n, m, false); }

bool thumb32_TBH(Reg n, Reg m) { return thumb32_load_store_dual_TableBranch(*this, n, m, true); }

// thumb32_load_store_multiple
/* This file is part of the dynarmic project.
 * Copyright (c) 2021 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

static bool thumb32_load_store_multiple_ITBlockCheck(const Semantics& ir) {
    return ir.current_location.IT().IsInITBlock() && !ir.current_location.IT().IsLastInITBlock();
}

static bool thumb32_load_store_multiple_LDMHelper(Semantics& ir,
                                                  bool W,
                                                  Reg n,
                                                  u32 list,
                                                  const Value& start_address,
                                                  const Value& writeback_address) {
    u32 offset = 0;
    const auto word_at = [&] {
        const auto addr = offset == 0 ? start_address : ir.Add(start_address, ir.Imm32(offset));
        const auto acc = offset == 0 ? AccType::ATOMIC : AccType::NORMAL;
        offset += 4;
        return ir.ReadMemory32(addr, acc);
    };
    for (size_t i = 0; i <= 14; i++) {
        if (bits::get_bit(i, list)) ir.SetRegister(static_cast<Reg>(i), word_at());
    }
    Value branch_pc;
    if (bits::get_bit<15>(list)) branch_pc = word_at();
    if (W && !bits::get_bit(RegNumber(n), list)) {
        ir.SetRegister(n, writeback_address);
    }
    if (bits::get_bit<15>(list)) {
        ir.UpdateUpperLocationDescriptor();
        ir.LoadWritePC(branch_pc);
        if (n == Reg::R13) {
            ir.SetTerm(Term::PopRSBHint{});
        } else {
            ir.SetTerm(Term::FastDispatchHint{});
        }
        return false;
    }
    return true;
}

static bool thumb32_load_store_multiple_STMHelper(Semantics& ir,
                                                  bool W,
                                                  Reg n,
                                                  u32 list,
                                                  const Value& start_address,
                                                  const Value& writeback_address) {
    u32 offset = 0;
    for (size_t i = 0; i <= 14; i++) {
        if (bits::get_bit(i, list)) {
            const auto addr =
                    offset == 0 ? start_address : ir.Add(start_address, ir.Imm32(offset));
            const auto acc = offset == 0 ? AccType::ATOMIC : AccType::NORMAL;
            offset += 4;
            ir.WriteMemory32(addr, ir.GetRegister(static_cast<Reg>(i)), acc);
        }
    }
    if (W) {
        ir.SetRegister(n, writeback_address);
    }
    return true;
}

bool thumb32_LDMDB(bool W, Reg n, Imm<16> reg_list) {
    const auto regs_imm = reg_list.ZeroExtend();
    const auto num_regs = static_cast<u32>(bits::count_ones(regs_imm));

    if (n == Reg::PC || num_regs < 2) {
        return UnpredictableInstruction();
    }
    if (reg_list.Bit<15>() && reg_list.Bit<14>()) {
        return UnpredictableInstruction();
    }
    if (W && bits::get_bit(static_cast<size_t>(n), regs_imm)) {
        return UnpredictableInstruction();
    }
    if (reg_list.Bit<13>()) {
        return UnpredictableInstruction();
    }
    if (reg_list.Bit<15>() && thumb32_load_store_multiple_ITBlockCheck(ir)) {
        return UnpredictableInstruction();
    }

    // Start address is the same as the writeback address.
    const Value start_address = ir.Sub(ir.GetRegister(n), ir.Imm32(4 * num_regs));
    return thumb32_load_store_multiple_LDMHelper(ir, W, n, regs_imm, start_address, start_address);
}

bool thumb32_LDMIA(bool W, Reg n, Imm<16> reg_list) {
    const auto regs_imm = reg_list.ZeroExtend();
    const auto num_regs = static_cast<u32>(bits::count_ones(regs_imm));

    if (n == Reg::PC || num_regs < 2) {
        return UnpredictableInstruction();
    }
    if (reg_list.Bit<15>() && reg_list.Bit<14>()) {
        return UnpredictableInstruction();
    }
    if (W && bits::get_bit(static_cast<size_t>(n), regs_imm)) {
        return UnpredictableInstruction();
    }
    if (reg_list.Bit<13>()) {
        return UnpredictableInstruction();
    }
    if (reg_list.Bit<15>() && thumb32_load_store_multiple_ITBlockCheck(ir)) {
        return UnpredictableInstruction();
    }

    const auto start_address = ir.GetRegister(n);
    const auto writeback_address = ir.Add(start_address, ir.Imm32(num_regs * 4));
    return thumb32_load_store_multiple_LDMHelper(
            ir, W, n, regs_imm, start_address, writeback_address);
}

bool thumb32_POP(Imm<16> reg_list) { return thumb32_LDMIA(true, Reg::SP, reg_list); }

bool thumb32_PUSH(Imm<15> reg_list) { return thumb32_STMDB(true, Reg::SP, reg_list); }

bool thumb32_STMIA(bool W, Reg n, Imm<15> reg_list) {
    const auto regs_imm = reg_list.ZeroExtend();
    const auto num_regs = static_cast<u32>(bits::count_ones(regs_imm));

    if (n == Reg::PC || num_regs < 2) {
        return UnpredictableInstruction();
    }
    if (W && bits::get_bit(static_cast<size_t>(n), regs_imm)) {
        return UnpredictableInstruction();
    }
    if (reg_list.Bit<13>()) {
        return UnpredictableInstruction();
    }

    const auto start_address = ir.GetRegister(n);
    const auto writeback_address = ir.Add(start_address, ir.Imm32(num_regs * 4));
    return thumb32_load_store_multiple_STMHelper(
            ir, W, n, regs_imm, start_address, writeback_address);
}

bool thumb32_STMDB(bool W, Reg n, Imm<15> reg_list) {
    const auto regs_imm = reg_list.ZeroExtend();
    const auto num_regs = static_cast<u32>(bits::count_ones(regs_imm));

    if (n == Reg::PC || num_regs < 2) {
        return UnpredictableInstruction();
    }
    if (W && bits::get_bit(static_cast<size_t>(n), regs_imm)) {
        return UnpredictableInstruction();
    }
    if (reg_list.Bit<13>()) {
        return UnpredictableInstruction();
    }

    // Start address is the same as the writeback address.
    const Value start_address = ir.Sub(ir.GetRegister(n), ir.Imm32(4 * num_regs));
    return thumb32_load_store_multiple_STMHelper(ir, W, n, regs_imm, start_address, start_address);
}

bool thumb16_LDR_literal(Reg t, Imm<8> imm8) {
    const u32 imm32 = imm8.ZeroExtend() << 2;
    const u32 address = ir.AlignPC(4) + imm32;
    const auto data = ir.ReadMemory32(ir.Imm32(address), AccType::NORMAL);

    ir.SetRegister(t, data);
    return true;
}

bool thumb16_STR_reg(Reg m, Reg n, Reg t) {
    const auto address = ir.Add(ir.GetRegister(n), ir.GetRegister(m));
    const auto data = ir.GetRegister(t);

    ir.WriteMemory32(address, data, AccType::NORMAL);
    return true;
}

bool thumb16_STRH_reg(Reg m, Reg n, Reg t) {
    const auto address = ir.Add(ir.GetRegister(n), ir.GetRegister(m));
    const auto data = ir.LeastSignificantHalf(ir.GetRegister(t));

    ir.WriteMemory16(address, data, AccType::NORMAL);
    return true;
}

bool thumb16_STRB_reg(Reg m, Reg n, Reg t) {
    const auto address = ir.Add(ir.GetRegister(n), ir.GetRegister(m));
    const auto data = ir.LeastSignificantByte(ir.GetRegister(t));

    ir.WriteMemory8(address, data, AccType::NORMAL);
    return true;
}

bool thumb16_LDRSB_reg(Reg m, Reg n, Reg t) {
    const auto address = ir.Add(ir.GetRegister(n), ir.GetRegister(m));
    const auto data = ir.SignExtendByteToWord(ir.ReadMemory8(address, AccType::NORMAL));

    ir.SetRegister(t, data);
    return true;
}

bool thumb16_LDR_reg(Reg m, Reg n, Reg t) {
    const auto address = ir.Add(ir.GetRegister(n), ir.GetRegister(m));
    const auto data = ir.ReadMemory32(address, AccType::NORMAL);

    ir.SetRegister(t, data);
    return true;
}

bool thumb16_LDRH_reg(Reg m, Reg n, Reg t) {
    const auto address = ir.Add(ir.GetRegister(n), ir.GetRegister(m));
    const auto data = ir.ZeroExtendHalfToWord(ir.ReadMemory16(address, AccType::NORMAL));

    ir.SetRegister(t, data);
    return true;
}

bool thumb16_LDRB_reg(Reg m, Reg n, Reg t) {
    const auto address = ir.Add(ir.GetRegister(n), ir.GetRegister(m));
    const auto data = ir.ZeroExtendByteToWord(ir.ReadMemory8(address, AccType::NORMAL));

    ir.SetRegister(t, data);
    return true;
}

bool thumb16_LDRSH_reg(Reg m, Reg n, Reg t) {
    const auto address = ir.Add(ir.GetRegister(n), ir.GetRegister(m));
    const auto data = ir.SignExtendHalfToWord(ir.ReadMemory16(address, AccType::NORMAL));

    ir.SetRegister(t, data);
    return true;
}

bool thumb16_STR_imm_t1(Imm<5> imm5, Reg n, Reg t) {
    const u32 imm32 = imm5.ZeroExtend() << 2;
    const auto address = ir.Add(ir.GetRegister(n), ir.Imm32(imm32));
    const auto data = ir.GetRegister(t);

    ir.WriteMemory32(address, data, AccType::NORMAL);
    return true;
}

bool thumb16_LDR_imm_t1(Imm<5> imm5, Reg n, Reg t) {
    const u32 imm32 = imm5.ZeroExtend() << 2;
    const auto address = ir.Add(ir.GetRegister(n), ir.Imm32(imm32));
    const auto data = ir.ReadMemory32(address, AccType::NORMAL);

    ir.SetRegister(t, data);
    return true;
}

bool thumb16_STRB_imm(Imm<5> imm5, Reg n, Reg t) {
    const u32 imm32 = imm5.ZeroExtend();
    const auto address = ir.Add(ir.GetRegister(n), ir.Imm32(imm32));
    const auto data = ir.LeastSignificantByte(ir.GetRegister(t));

    ir.WriteMemory8(address, data, AccType::NORMAL);
    return true;
}

bool thumb16_LDRB_imm(Imm<5> imm5, Reg n, Reg t) {
    const u32 imm32 = imm5.ZeroExtend();
    const auto address = ir.Add(ir.GetRegister(n), ir.Imm32(imm32));
    const auto data = ir.ZeroExtendByteToWord(ir.ReadMemory8(address, AccType::NORMAL));

    ir.SetRegister(t, data);
    return true;
}

bool thumb16_STRH_imm(Imm<5> imm5, Reg n, Reg t) {
    const u32 imm32 = imm5.ZeroExtend() << 1;
    const auto address = ir.Add(ir.GetRegister(n), ir.Imm32(imm32));
    const auto data = ir.LeastSignificantHalf(ir.GetRegister(t));

    ir.WriteMemory16(address, data, AccType::NORMAL);
    return true;
}

bool thumb16_LDRH_imm(Imm<5> imm5, Reg n, Reg t) {
    const u32 imm32 = imm5.ZeroExtend() << 1;
    const auto address = ir.Add(ir.GetRegister(n), ir.Imm32(imm32));
    const auto data = ir.ZeroExtendHalfToWord(ir.ReadMemory16(address, AccType::NORMAL));

    ir.SetRegister(t, data);
    return true;
}

bool thumb16_STR_imm_t2(Reg t, Imm<8> imm8) {
    const u32 imm32 = imm8.ZeroExtend() << 2;
    const Reg n = Reg::SP;
    const auto address = ir.Add(ir.GetRegister(n), ir.Imm32(imm32));
    const auto data = ir.GetRegister(t);

    ir.WriteMemory32(address, data, AccType::NORMAL);
    return true;
}

bool thumb16_LDR_imm_t2(Reg t, Imm<8> imm8) {
    const u32 imm32 = imm8.ZeroExtend() << 2;
    const Reg n = Reg::SP;
    const auto address = ir.Add(ir.GetRegister(n), ir.Imm32(imm32));
    const auto data = ir.ReadMemory32(address, AccType::NORMAL);

    ir.SetRegister(t, data);
    return true;
}

bool thumb16_PUSH(bool M, RegList reg_list) {
    if (M) {
        reg_list |= 1 << 14;
    }
    if (bits::count_ones(reg_list) < 1) {
        return UnpredictableInstruction();
    }

    const u32 num_bytes_to_push = static_cast<u32>(4 * bits::count_ones(reg_list));
    const auto final_address = ir.Sub(ir.GetRegister(Reg::SP), ir.Imm32(num_bytes_to_push));
    u32 offset = 0;
    for (size_t i = 0; i < 16; i++) {
        if (bits::get_bit(i, reg_list)) {
            const auto addr =
                    offset == 0 ? final_address : ir.Add(final_address, ir.Imm32(offset));
            const auto acc = offset == 0 ? AccType::ATOMIC : AccType::NORMAL;
            offset += 4;
            ir.WriteMemory32(addr, ir.GetRegister(static_cast<Reg>(i)), acc);
        }
    }

    ir.SetRegister(Reg::SP, final_address);
    return true;
}

bool thumb16_POP(bool P, RegList reg_list) {
    if (P) {
        reg_list |= 1 << 15;
    }
    if (bits::count_ones(reg_list) < 1) {
        return UnpredictableInstruction();
    }

    const auto start_address = ir.GetRegister(Reg::SP);
    u32 offset = 0;
    const auto word_at = [&] {
        const auto addr = offset == 0 ? start_address : ir.Add(start_address, ir.Imm32(offset));
        const auto acc = offset == 0 ? AccType::ATOMIC : AccType::NORMAL;
        offset += 4;
        return ir.ReadMemory32(addr, acc);
    };
    for (size_t i = 0; i < 15; i++) {
        if (bits::get_bit(i, reg_list)) ir.SetRegister(static_cast<Reg>(i), word_at());
    }

    if (bits::get_bit<15>(reg_list)) {
        const auto data = word_at();
        ir.UpdateUpperLocationDescriptor();
        ir.LoadWritePC(data);
        ir.SetRegister(Reg::SP, ir.Add(start_address, ir.Imm32(offset)));
        ir.SetTerm(Term::PopRSBHint{});
        return false;
    }
    ir.SetRegister(Reg::SP, ir.Add(start_address, ir.Imm32(offset)));
    return true;
}

bool thumb16_STMIA(Reg n, RegList reg_list) {
    if (bits::count_ones(reg_list) == 0) {
        return UnpredictableInstruction();
    }
    if (bits::get_bit(static_cast<size_t>(n), reg_list) &&
        n != static_cast<Reg>(bits::lowest_set_bit(reg_list))) {
        return UnpredictableInstruction();
    }

    const auto start_address = ir.GetRegister(n);
    u32 offset = 0;
    for (size_t i = 0; i < 8; i++) {
        if (bits::get_bit(i, reg_list)) {
            const auto addr =
                    offset == 0 ? start_address : ir.Add(start_address, ir.Imm32(offset));
            const auto acc = offset == 0 ? AccType::ATOMIC : AccType::NORMAL;
            offset += 4;
            ir.WriteMemory32(addr, ir.GetRegister(static_cast<Reg>(i)), acc);
        }
    }

    ir.SetRegister(n, ir.Add(start_address, ir.Imm32(offset)));
    return true;
}

bool thumb16_LDMIA(Reg n, RegList reg_list) {
    if (bits::count_ones(reg_list) == 0) {
        return UnpredictableInstruction();
    }

    const bool write_back = !bits::get_bit(static_cast<size_t>(n), reg_list);
    const auto start_address = ir.GetRegister(n);
    u32 offset = 0;

    for (size_t i = 0; i < 8; i++) {
        if (bits::get_bit(i, reg_list)) {
            const auto addr =
                    offset == 0 ? start_address : ir.Add(start_address, ir.Imm32(offset));
            const auto acc = offset == 0 ? AccType::ATOMIC : AccType::NORMAL;
            offset += 4;
            ir.SetRegister(static_cast<Reg>(i), ir.ReadMemory32(addr, acc));
        }
    }

    if (write_back) {
        ir.SetRegister(n, ir.Add(start_address, ir.Imm32(offset)));
    }
    return true;
}
