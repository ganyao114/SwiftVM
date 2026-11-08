Value Widen(Value value) {
    if (value.ViewType() == Type::U64) {
        return value;
    }
    return this->ZeroExtend64(value).WithView(Type::U64);
}

Value ReadRegister(u8 code, Type size, Reg31Mode r31mode = Reg31IsZeroRegister) {
    if (r31mode == Reg31IsZeroRegister && code == kZeroRegCode) {
        return ImmValue(0, size);
    }
    return this->LoadUniform(Uniform{GPROffset(code), size}).WithView(size);
}

Value ReadVRegister(u8 code, Type type) {
    type = StorageType(type);
    auto offset = u32(offsetof(GuestState, q) + code * sizeof(u128));
    return this->LoadUniform(Uniform{offset, type}).WithView(type);
}

Value ReadVHalf(u8 code, bool high) {
    auto offset = u32(offsetof(GuestState, q) + code * sizeof(u128)) + (high ? 8 : 0);
    return this->template LoadUniform<Type::U64>(Uniform{offset, Type::U64}).WithView(Type::U64);
}

void WriteVHalves(u8 code, Value low, Value high) {
    auto offset = u32(offsetof(GuestState, q) + code * sizeof(u128));
    this->StoreUniform(Uniform{offset, Type::U64}, low);
    this->StoreUniform(Uniform{offset + 8, Type::U64}, high);
}

Value ZeroByteMask(Value value) {
    constexpr u64 low_bits = 0x7F7F7F7F7F7F7F7F;
    constexpr u64 high_bits = 0x8080808080808080;
    auto low =
            this->template And<Type::U64>(value, SingleOperand(Imm{low_bits})).WithView(Type::U64);
    auto sum = this->template Add<Type::U64>(low, SingleOperand(Imm{low_bits})).WithView(Type::U64);
    auto set = this->template Or<Type::U64>(sum, SingleOperand(value)).WithView(Type::U64);
    set = this->template Or<Type::U64>(set, SingleOperand(Imm{low_bits})).WithView(Type::U64);
    auto inverted =
            this->template Xor<Type::U64>(set, SingleOperand(Imm{u64(-1)})).WithView(Type::U64);
    auto msb = this->template And<Type::U64>(inverted, SingleOperand(Imm{high_bits}))
                       .WithView(Type::U64);
    auto bits = this->template LsrImm<Type::U64>(msb, Imm{7}).WithView(Type::U64);
    return this->template Mul<Type::U64>(bits, SingleOperand(Imm{0xFF})).WithView(Type::U64);
}

void WriteXRegister(u8 code, Value value, Reg31Mode r31mode = Reg31IsZeroRegister) {
    if (r31mode == Reg31IsZeroRegister && code == kZeroRegCode) {
        return;
    }
    this->StoreUniform(Uniform{GPROffset(code), Type::U64}, Widen(value));
}

void WriteWRegister(u8 code, Value value, Reg31Mode r31mode = Reg31IsZeroRegister) {
    if (r31mode == Reg31IsZeroRegister && code == kZeroRegCode) {
        return;
    }
    Value wide;
    switch (value.ViewType()) {
        case Type::U8:
        case Type::U16:
        case Type::U32:
            wide = this->ZeroExtend64(value).WithView(Type::U64);
            break;
        default:
            // 64 bit (or untyped) value: explicitly clear the top half.
            wide = this->template And<Type::U64>(value, SingleOperand(Imm{u64(0xFFFFFFFF)}))
                           .WithView(Type::U64);
            break;
    }
    this->StoreUniform(Uniform{GPROffset(code), Type::U64}, wide);
}

void WriteVRegister(u8 code, Value value) {
    value = Reinterpret(value, value.ViewType());
    if (TypeBits(value.ViewType()) <= 64) {
        // Scalar SIMD writes clear the complete remainder of the Q register.
        WriteVHalves(code, Widen(value), ImmValue(0, Type::U64));
        return;
    }
    auto offset = u32(offsetof(GuestState, q) + code * sizeof(u128));
    this->StoreUniform(Uniform{offset, value.ViewType()}, value);
}

Value SignExtendValue(Value value, u32 from_bits) {
    auto wide = Widen(value);
    u32 shift = 64 - from_bits;
    if (shift == 0) {
        return wide;
    }
    auto shifted = this->template LslImm<Type::U64>(wide, Imm{shift}).WithView(Type::U64);
    return this->template AsrImm<Type::U64>(shifted, Imm{shift}).WithView(Type::U64);
}

Value ShiftOperand(bool is64, Value value, Shift shift, u32 amount) {
    if (is64) {
        switch (shift) {
            case LSL:
                return amount ? this->template LslImm<Type::U64>(value, Imm{amount})
                                        .WithView(Type::U64)
                              : value;
            case LSR:
                return amount ? this->template LsrImm<Type::U64>(value, Imm{amount})
                                        .WithView(Type::U64)
                              : value;
            case ASR:
                return amount ? this->template AsrImm<Type::U64>(value, Imm{amount})
                                        .WithView(Type::U64)
                              : value;
            case ROR:
                return amount ? this->template RorImm<Type::U64>(value, Imm{amount})
                                        .WithView(Type::U64)
                              : value;
            default:
                VIXL_UNREACHABLE();
        }
    } else {
        switch (shift) {
            case LSL:
                return amount ? this->template LslImm<Type::U32>(value, Imm{amount})
                                        .WithView(Type::U32)
                              : value;
            case LSR:
                return amount ? this->template LsrImm<Type::U32>(value, Imm{amount})
                                        .WithView(Type::U32)
                              : value;
            case ASR:
                return amount ? this->template AsrImm<Type::U32>(value, Imm{amount})
                                        .WithView(Type::U32)
                              : value;
            case ROR: {
                // 32 bit rotate of a zero extended value.
                if (!amount) return value;
                auto lo = this->template LsrImm<Type::U32>(value, Imm{amount}).WithView(Type::U32);
                auto hi = this->template LslImm<Type::U32>(value, Imm{32 - amount})
                                  .WithView(Type::U32);
                return this->template Or<Type::U32>(lo, SingleOperand(hi)).WithView(Type::U32);
            }
            default:
                VIXL_UNREACHABLE();
        }
    }
    return {};
}

Value ExtendOperand(Value value, Extend extend, u32 shift) {
    Value extended;
    switch (extend) {
        case UXTB:
            extended = this->template And<Type::U64>(Widen(value), SingleOperand(Imm{u64(0xFF)}))
                               .WithView(Type::U64);
            break;
        case UXTH:
            extended = this->template And<Type::U64>(Widen(value), SingleOperand(Imm{u64(0xFFFF)}))
                               .WithView(Type::U64);
            break;
        case UXTW:
            extended =
                    this->template And<Type::U64>(Widen(value), SingleOperand(Imm{u64(0xFFFFFFFF)}))
                            .WithView(Type::U64);
            break;
        case UXTX:
            extended = Widen(value);
            break;
        case SXTB:
            extended = SignExtendValue(value, 8);
            break;
        case SXTH:
            extended = SignExtendValue(value, 16);
            break;
        case SXTW:
            extended = SignExtendValue(value, 32);
            break;
        case SXTX:
            extended = Widen(value);
            break;
        default:
            VIXL_UNREACHABLE();
    }
    if (shift) {
        extended = this->template LslImm<Type::U64>(extended, Imm{shift}).WithView(Type::U64);
    }
    return extended;
}

Value BoolAnd(Value a, Value b) {
    return Value{this->template And<Type::U8>(a, SingleOperand(b)).WithView(Type::U8)};
}

Value BoolOr(Value a, Value b) {
    return Value{this->template Or<Type::U8>(a, SingleOperand(b)).WithView(Type::U8)};
}

Value BoolXor(Value a, Value b) {
    return Value{this->template Xor<Type::U8>(a, SingleOperand(b)).WithView(Type::U8)};
}

Value CondPassed(vixl::aarch64::Condition cond) {
    auto boolean = [](Value value) { return Value{value.WithView(Type::U8)}; };
    auto n = [&] { return boolean(this->TestFlags(Flag::Negate)); };
    auto z = [&] { return boolean(this->TestFlags(Flag::Zero)); };
    auto c = [&] { return boolean(this->TestFlags(Flag::Carry)); };
    auto v = [&] { return boolean(this->TestFlags(Flag::Overflow)); };
    auto not_n = [&] { return boolean(this->TestNotFlags(Flag::Negate)); };
    auto not_z = [&] { return boolean(this->TestNotFlags(Flag::Zero)); };
    auto not_c = [&] { return boolean(this->TestNotFlags(Flag::Carry)); };
    auto not_v = [&] { return boolean(this->TestNotFlags(Flag::Overflow)); };
    switch (cond) {
        case eq:
            return z();
        case ne:
            return not_z();
        case cs:
            return c();
        case cc:
            return not_c();
        case mi:
            return n();
        case pl:
            return not_n();
        case vs:
            return v();
        case vc:
            return not_v();
        case hi:
            return BoolAnd(c(), not_z());
        case ls:
            return BoolOr(not_c(), z());
        case ge:
            // N == V
            return boolean(this->TestZero(BoolXor(n(), v())));
        case lt:
            // N != V
            return boolean(this->TestNotZero(BoolXor(n(), v())));
        case gt:
            // !Z && (N == V)
            return BoolAnd(boolean(this->TestZero(BoolXor(n(), v()))), not_z());
        case le:
            // Z || (N != V)
            return BoolOr(boolean(this->TestNotZero(BoolXor(n(), v()))), z());
        case al:
        case nv:
        default:
            return boolean(this->template LoadImm<Type::U8>(Imm{true}));
    }
}

Value AddressAdd(Value base, s64 offset) {
    if (offset == 0) {
        return base;
    } else if (offset > 0) {
        return this->template Add<Type::U64>(base, SingleOperand(Imm{u64(offset)}))
                .WithView(Type::U64);
    } else {
        return this->template Sub<Type::U64>(base, SingleOperand(Imm{u64(-offset)}))
                .WithView(Type::U64);
    }
}

Value ReadMemory(Input address, Type type) {
    auto operand =
            address.IsValue() ? SingleOperand(address.GetValue()) : SingleOperand(address.GetImm());
    return TypedLoad(type, operand);
}

void WriteMemory(Value address, Value value) { this->StoreMemory(SingleOperand(address), value); }

void VisitUnimplemented(const vixl::aarch64::Instruction* instr) override {
    if (TryBaseMemoryExtension(instr)) return;
    Interrupt(InterruptReason::FALLBACK, current_pc);
}

void VisitUnallocated(const vixl::aarch64::Instruction* instr) override {
    Interrupt(InterruptReason::ILL_CODE, current_pc);
}

void VisitReserved(const vixl::aarch64::Instruction* instr) override {
    Interrupt(InterruptReason::ILL_CODE, current_pc);
}
