/* This file is part of the dynarmic project.
 * Copyright (c) 2020 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

std::optional<std::tuple<size_t, size_t, size_t>> DecodeType(Imm<4> type,
                                                             size_t size,
                                                             size_t align) {
    switch (type.ZeroExtend()) {
        case 0b0111:  // VST1 A1 / VLD1 A1
            if (NeonBit<1>(align)) {
                return std::nullopt;
            }
            return std::tuple<size_t, size_t, size_t>{1, 1, 0};
        case 0b1010:  // VST1 A2 / VLD1 A2
            if (align == 0b11) {
                return std::nullopt;
            }
            return std::tuple<size_t, size_t, size_t>{1, 2, 0};
        case 0b0110:  // VST1 A3 / VLD1 A3
            if (NeonBit<1>(align)) {
                return std::nullopt;
            }
            return std::tuple<size_t, size_t, size_t>{1, 3, 0};
        case 0b0010:  // VST1 A4 / VLD1 A4
            return std::tuple<size_t, size_t, size_t>{1, 4, 0};
        case 0b1000:  // VST2 A1 / VLD2 A1
            if (size == 0b11 || align == 0b11) {
                return std::nullopt;
            }
            return std::tuple<size_t, size_t, size_t>{2, 1, 1};
        case 0b1001:  // VST2 A1 / VLD2 A1
            if (size == 0b11 || align == 0b11) {
                return std::nullopt;
            }
            return std::tuple<size_t, size_t, size_t>{2, 1, 2};
        case 0b0011:  // VST2 A2 / VLD2 A2
            if (size == 0b11) {
                return std::nullopt;
            }
            return std::tuple<size_t, size_t, size_t>{2, 2, 2};
        case 0b0100:  // VST3 / VLD3
            if (size == 0b11 || NeonBit<1>(align)) {
                return std::nullopt;
            }
            return std::tuple<size_t, size_t, size_t>{3, 1, 1};
        case 0b0101:  // VST3 / VLD3
            if (size == 0b11 || NeonBit<1>(align)) {
                return std::nullopt;
            }
            return std::tuple<size_t, size_t, size_t>{3, 1, 2};
        case 0b0000:  // VST4 / VLD4
            if (size == 0b11) {
                return std::nullopt;
            }
            return std::tuple<size_t, size_t, size_t>{4, 1, 1};
        case 0b0001:  // VST4 / VLD4
            if (size == 0b11) {
                return std::nullopt;
            }
            return std::tuple<size_t, size_t, size_t>{4, 1, 2};
    }
    return std::nullopt;
}

bool v8_VST_multiple(bool D, Reg n, size_t Vd, Imm<4> type, size_t size, size_t align, Reg m) {
    if (type == 0b1011 || type.Bits<2, 3>() == 0b11) {
        return DecodeError();
    }

    const auto decoded_type = DecodeType(type, size, align);
    if (!decoded_type) {
        return UndefinedInstruction();
    }
    const auto [nelem, regs, inc] = *decoded_type;

    const ExtReg d = ToExtRegD(Vd, D);
    const size_t d_last = RegNumber(d) + inc * (nelem - 1);
    if (n == Reg::R15 || d_last + regs > 32) {
        return UnpredictableInstruction();
    }

    const size_t alignment = align == 0 ? 1 : 4 << align;
    const size_t ebytes = static_cast<size_t>(1) << size;
    const size_t elements = 8 / ebytes;

    const bool wback = m != Reg::R15;
    const bool register_index = m != Reg::R15 && m != Reg::R13;

    Value address = ir.GetRegister(n);
    if (alignment > 1) Emit<OpCode::CheckMemoryAlignment>(address, u64(alignment - 1));
    for (size_t r = 0; r < regs; r++) {
        for (size_t e = 0; e < elements; e++) {
            for (size_t i = 0; i < nelem; i++) {
                const ExtReg ext_reg = d + i * inc + r;
                const Value shifted_element = ir.LogicalShiftRight(
                        ir.GetExtendedRegister(ext_reg), ir.Imm8(static_cast<u8>(e * ebytes * 8)));
                const Value element = ir.LeastSignificant(8 * ebytes, shifted_element);
                ir.WriteMemory(8 * ebytes, address, element, AccType::NORMAL);

                address = ir.Add(address, ir.Imm32(static_cast<u32>(ebytes)));
            }
        }
    }

    if (wback) {
        if (register_index) {
            ir.SetRegister(n, ir.Add(ir.GetRegister(n), ir.GetRegister(m)));
        } else {
            ir.SetRegister(n,
                           ir.Add(ir.GetRegister(n), ir.Imm32(static_cast<u32>(8 * nelem * regs))));
        }
    }

    return true;
}

bool v8_VLD_multiple(bool D, Reg n, size_t Vd, Imm<4> type, size_t size, size_t align, Reg m) {
    if (type == 0b1011 || type.Bits<2, 3>() == 0b11) {
        return DecodeError();
    }

    const auto decoded_type = DecodeType(type, size, align);
    if (!decoded_type) {
        return UndefinedInstruction();
    }
    const auto [nelem, regs, inc] = *decoded_type;

    const ExtReg d = ToExtRegD(Vd, D);
    const size_t d_last = RegNumber(d) + inc * (nelem - 1);
    if (n == Reg::R15 || d_last + regs > 32) {
        return UnpredictableInstruction();
    }

    const size_t alignment = align == 0 ? 1 : 4 << align;
    const size_t ebytes = static_cast<size_t>(1) << size;
    const size_t elements = 8 / ebytes;

    const bool wback = m != Reg::R15;
    const bool register_index = m != Reg::R15 && m != Reg::R13;

    // Publish each completed element and retain untouched lanes. On an abort,
    // completed elements remain visible and base writeback has not occurred.
    std::array<Value, 4> loaded;
    for (size_t r = 0; r < regs; ++r)
        for (size_t i = 0; i < nelem; ++i)
            loaded[r * nelem + i] = ir.GetExtendedRegister(d + i * inc + r);

    Value address = ir.GetRegister(n);
    if (alignment > 1) Emit<OpCode::CheckMemoryAlignment>(address, u64(alignment - 1));
    for (size_t r = 0; r < regs; r++) {
        for (size_t e = 0; e < elements; e++) {
            for (size_t i = 0; i < nelem; i++) {
                const Value element =
                        ir.ZeroExtendToLong(ir.ReadMemory(ebytes * 8, address, AccType::NORMAL));
                auto& destination = loaded[r * nelem + i];
                destination = Emit<OpCode::BitInsert, Type::U64>(
                        destination, element, u64(e * ebytes * 8), u64(ebytes * 8));
                ir.SetExtendedRegister(d + i * inc + r, destination);

                address = ir.Add(address, ir.Imm32(static_cast<u32>(ebytes)));
            }
        }
    }

    if (wback) {
        if (register_index) {
            ir.SetRegister(n, ir.Add(ir.GetRegister(n), ir.GetRegister(m)));
        } else {
            ir.SetRegister(n,
                           ir.Add(ir.GetRegister(n), ir.Imm32(static_cast<u32>(8 * nelem * regs))));
        }
    }

    return true;
}

bool v8_VLD_all_lanes(bool D, Reg n, size_t Vd, size_t nn, size_t sz, bool T, bool a, Reg m) {
    const size_t nelem = nn + 1;

    if (nelem == 1 && (sz == 0b11 || (sz == 0b00 && a))) {
        return UndefinedInstruction();
    }
    if (nelem == 2 && sz == 0b11) {
        return UndefinedInstruction();
    }
    if (nelem == 3 && (sz == 0b11 || a)) {
        return UndefinedInstruction();
    }
    if (nelem == 4 && (sz == 0b11 && !a)) {
        return UndefinedInstruction();
    }

    const size_t ebytes = sz == 0b11 ? 4 : (1 << sz);
    const size_t inc = T ? 2 : 1;
    const size_t regs = nelem == 1 ? inc : 1;
    const size_t alignment = [&]() -> size_t {
        if (a && nelem == 1) {
            return ebytes;
        }
        if (a && nelem == 2) {
            return ebytes * 2;
        }
        if (a && nelem == 4) {
            return sz >= 0b10 ? 2 * ebytes : 4 * ebytes;
        }
        return 1;
    }();

    const ExtReg d = ToExtRegD(Vd, D);
    const size_t d_last = RegNumber(d) + inc * (nelem - 1);
    if (n == Reg::R15 || d_last + regs > 32) {
        return UnpredictableInstruction();
    }

    const bool wback = m != Reg::R15;
    const bool register_index = m != Reg::R15 && m != Reg::R13;

    auto address = ir.GetRegister(n);
    if (alignment > 1) Emit<OpCode::CheckMemoryAlignment>(address, u64(alignment - 1));
    for (size_t i = 0; i < nelem; i++) {
        const auto element = ir.ReadMemory(ebytes * 8, address, AccType::NORMAL);
        const auto replicated_element = ir.VectorBroadcast(ebytes * 8, element);

        for (size_t r = 0; r < regs; r++) {
            const ExtReg ext_reg = d + i * inc + r;
            ir.SetVector(ext_reg, replicated_element);
        }

        address = ir.Add(address, ir.Imm32(static_cast<u32>(ebytes)));
    }

    if (wback) {
        if (register_index) {
            ir.SetRegister(n, ir.Add(ir.GetRegister(n), ir.GetRegister(m)));
        } else {
            ir.SetRegister(n,
                           ir.Add(ir.GetRegister(n), ir.Imm32(static_cast<u32>(nelem * ebytes))));
        }
    }

    return true;
}

bool v8_VST_single(bool D, Reg n, size_t Vd, size_t sz, size_t nn, size_t index_align, Reg m) {
    const size_t nelem = nn + 1;

    if (sz == 0b11) {
        return DecodeError();
    }

    if (nelem == 1 && NeonBit(sz, index_align)) {
        return UndefinedInstruction();
    }

    const size_t ebytes = size_t(1) << sz;
    const size_t index = NeonBits(sz + 1, 3, index_align);
    const size_t inc = (sz != 0 && NeonBit(sz, index_align)) ? 2 : 1;
    const size_t a = NeonBits(0, sz ? sz - 1 : 0, index_align);

    if (nelem == 1 && inc == 2) {
        return UndefinedInstruction();
    }
    if (nelem == 1 && sz == 2 && (a != 0b00 && a != 0b11)) {
        return UndefinedInstruction();
    }
    if (nelem == 2 && NeonBit<1>(a)) {
        return UndefinedInstruction();
    }
    if (nelem == 3 && a != 0b00) {
        return UndefinedInstruction();
    }
    if (nelem == 4 && a == 0b11) {
        return UndefinedInstruction();
    }

    const size_t alignment = a == 0       ? 1
                             : nelem == 1 ? ebytes
                             : nelem == 2 ? ebytes * 2
                             : sz == 2    ? (size_t(1) << (sz + a))
                                          : ebytes * 4;

    const ExtReg d = ToExtRegD(Vd, D);
    const size_t d_last = RegNumber(d) + inc * (nelem - 1);
    if (n == Reg::R15 || d_last + 1 > 32) {
        return UnpredictableInstruction();
    }

    const bool wback = m != Reg::R15;
    const bool register_index = m != Reg::R15 && m != Reg::R13;

    auto address = ir.GetRegister(n);
    if (alignment > 1) Emit<OpCode::CheckMemoryAlignment>(address, u64(alignment - 1));
    for (size_t i = 0; i < nelem; i++) {
        const ExtReg ext_reg = d + i * inc;
        const auto element = ir.VectorGetElement(ebytes * 8, ir.GetVector(ext_reg), index);

        ir.WriteMemory(ebytes * 8, address, element, AccType::NORMAL);

        address = ir.Add(address, ir.Imm32(static_cast<u32>(ebytes)));
    }

    if (wback) {
        if (register_index) {
            ir.SetRegister(n, ir.Add(ir.GetRegister(n), ir.GetRegister(m)));
        } else {
            ir.SetRegister(n,
                           ir.Add(ir.GetRegister(n), ir.Imm32(static_cast<u32>(nelem * ebytes))));
        }
    }

    return true;
}

bool v8_VLD_single(bool D, Reg n, size_t Vd, size_t sz, size_t nn, size_t index_align, Reg m) {
    const size_t nelem = nn + 1;

    if (sz == 0b11) {
        return DecodeError();
    }

    if (nelem == 1 && NeonBit(sz, index_align)) {
        return UndefinedInstruction();
    }

    const size_t ebytes = size_t(1) << sz;
    const size_t index = NeonBits(sz + 1, 3, index_align);
    const size_t inc = (sz != 0 && NeonBit(sz, index_align)) ? 2 : 1;
    const size_t a = NeonBits(0, sz ? sz - 1 : 0, index_align);

    if (nelem == 1 && inc == 2) {
        return UndefinedInstruction();
    }
    if (nelem == 1 && sz == 2 && (a != 0b00 && a != 0b11)) {
        return UndefinedInstruction();
    }
    if (nelem == 2 && NeonBit<1>(a)) {
        return UndefinedInstruction();
    }
    if (nelem == 3 && a != 0b00) {
        return UndefinedInstruction();
    }
    if (nelem == 4 && a == 0b11) {
        return UndefinedInstruction();
    }

    const size_t alignment = a == 0       ? 1
                             : nelem == 1 ? ebytes
                             : nelem == 2 ? ebytes * 2
                             : sz == 2    ? (size_t(1) << (sz + a))
                                          : ebytes * 4;

    const ExtReg d = ToExtRegD(Vd, D);
    const size_t d_last = RegNumber(d) + inc * (nelem - 1);
    if (n == Reg::R15 || d_last + 1 > 32) {
        return UnpredictableInstruction();
    }

    const bool wback = m != Reg::R15;
    const bool register_index = m != Reg::R15 && m != Reg::R13;

    auto address = ir.GetRegister(n);
    if (alignment > 1) Emit<OpCode::CheckMemoryAlignment>(address, u64(alignment - 1));
    for (size_t i = 0; i < nelem; i++) {
        const auto element = ir.ReadMemory(ebytes * 8, address, AccType::NORMAL);

        const ExtReg ext_reg = d + i * inc;
        const auto new_reg = ir.VectorSetElement(ebytes * 8, ir.GetVector(ext_reg), index, element);
        ir.SetVector(ext_reg, new_reg);

        address = ir.Add(address, ir.Imm32(static_cast<u32>(ebytes)));
    }

    if (wback) {
        if (register_index) {
            ir.SetRegister(n, ir.Add(ir.GetRegister(n), ir.GetRegister(m)));
        } else {
            ir.SetRegister(n,
                           ir.Add(ir.GetRegister(n), ir.Imm32(static_cast<u32>(nelem * ebytes))));
        }
    }

    return true;
}
