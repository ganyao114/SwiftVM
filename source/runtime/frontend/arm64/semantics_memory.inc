void VisitLoadLiteral(const vixl::aarch64::Instruction* instr) override {
    this->SetLocation(Input{Imm{current_pc}});
    unsigned rt = instr->GetRt();
    auto address = instr->GetLiteralAddress<VAddr>(current_pc);

    // Verify that the calculated address is available to the host.
    VIXL_ASSERT(address == static_cast<uintptr_t>(address));

    switch (instr->Mask(LoadLiteralMask)) {
        case LDR_w_lit:
            WriteWRegister(rt, ReadMemory(Input{Imm{address}}, Type::U32));
            break;
        case LDR_x_lit:
            WriteXRegister(rt, ReadMemory(Input{Imm{address}}, Type::U64));
            break;
        case LDR_s_lit:
            WriteSRegister(rt, ReadMemory(Input{Imm{address}}, Type::V32));
            break;
        case LDR_d_lit:
            WriteDRegister(rt, ReadMemory(Input{Imm{address}}, Type::V64));
            break;
        case LDR_q_lit:
            WriteQRegister(rt, ReadMemory(Input{Imm{address}}, Type::V128));
            break;
        case LDRSW_x_lit:
            WriteXRegister(rt, SignExtendValue(ReadMemory(Input{Imm{address}}, Type::U32), 32));
            break;

        // Ignore prfm hint instructions.
        case PRFM_lit:
            break;

        default:
            VIXL_UNREACHABLE();
    }
}

void LoadStoreHelper(const vixl::aarch64::Instruction* instr, s64 offset, AddrMode mode) {
    LoadStoreHelper(instr, Input{Imm{offset}}, mode);
}

void LoadStoreHelper(const vixl::aarch64::Instruction* instr, const Input& offset, AddrMode mode) {
    auto rt = instr->GetRt();
    auto rn = instr->GetRn();

    // Writeback overlap is constrained-unpredictable. Request fallback
    // before mutation instead of choosing a value from aliased registers.
    if (mode != AddrMode::Offset && !(instr->GetInstructionBits() & (1u << 26)) && rn != 31 &&
        rn == rt) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    this->SetLocation(Input{Imm{current_pc}});

    auto base = ReadXRegister(rn, Reg31IsStackPointer);

    auto add_offset = [&](Value b) {
        if (offset.IsImm()) {
            return AddressAdd(b, offset.imm.GetSigned());
        }
        return this->template Add<Type::U64>(b, SingleOperand(offset.value)).WithView(Type::U64);
    };

    Value address, writeback;
    if (mode == AddrMode::Offset) {
        address = add_offset(base);
    } else {
        writeback = add_offset(base);
        address = (mode == AddrMode::PreIndex) ? writeback : base;
    }

    switch (static_cast<LoadStoreOp>(instr->Mask(LoadStoreMask))) {
        case LDRB_w:
            WriteWRegister(rt, ReadMemory(Input{address}, Type::U8));
            break;
        case LDRH_w:
            WriteWRegister(rt, ReadMemory(Input{address}, Type::U16));
            break;
        case LDR_w:
            WriteWRegister(rt, ReadMemory(Input{address}, Type::U32));
            break;
        case LDR_x:
            WriteXRegister(rt, ReadMemory(Input{address}, Type::U64));
            break;
        case LDRSB_w:
            WriteWRegister(rt, SignExtendValue(ReadMemory(Input{address}, Type::U8), 8));
            break;
        case LDRSH_w:
            WriteWRegister(rt, SignExtendValue(ReadMemory(Input{address}, Type::U16), 16));
            break;
        case LDRSB_x:
            WriteXRegister(rt, SignExtendValue(ReadMemory(Input{address}, Type::U8), 8));
            break;
        case LDRSH_x:
            WriteXRegister(rt, SignExtendValue(ReadMemory(Input{address}, Type::U16), 16));
            break;
        case LDRSW_x:
            WriteXRegister(rt, SignExtendValue(ReadMemory(Input{address}, Type::U32), 32));
            break;
        case STRB_w:
            WriteMemory(address, ReadRegister(rt, Type::U8));
            break;
        case STRH_w:
            WriteMemory(address, ReadRegister(rt, Type::U16));
            break;
        case STR_w:
            WriteMemory(address, ReadRegister(rt, Type::U32));
            break;
        case STR_x:
            WriteMemory(address, ReadRegister(rt, Type::U64));
            break;
        case LDR_b:
            WriteVRegister(rt, ReadMemory(Input{address}, Type::V8));
            break;
        case LDR_h:
            WriteVRegister(rt, ReadMemory(Input{address}, Type::V16));
            break;
        case LDR_s:
            WriteVRegister(rt, ReadMemory(Input{address}, Type::V32));
            break;
        case LDR_d:
            WriteVRegister(rt, ReadMemory(Input{address}, Type::V64));
            break;
        case LDR_q:
            WriteVRegister(rt, ReadMemory(Input{address}, Type::V128));
            break;
        case STR_b:
            WriteMemory(address, ReadVRegister(rt, Type::V8));
            break;
        case STR_h:
            WriteMemory(address, ReadVRegister(rt, Type::V16));
            break;
        case STR_s:
            WriteMemory(address, ReadVRegister(rt, Type::V32));
            break;
        case STR_d:
            WriteMemory(address, ReadVRegister(rt, Type::V64));
            break;
        case STR_q:
            WriteMemory(address, ReadVRegister(rt, Type::V128));
            break;

        // Ignore prfm hint instructions.
        case PRFM:
            break;

        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }
    // A memory fault exits before this architectural publication.
    if (mode != AddrMode::Offset) WriteXRegister(rn, writeback, Reg31IsStackPointer);
}

void VisitLoadStoreUnsignedOffset(const vixl::aarch64::Instruction* instr) override {
    s64 offset = s64(instr->GetImmLSUnsigned()) << instr->GetSizeLS();
    LoadStoreHelper(instr, offset, AddrMode::Offset);
}

void VisitLoadStoreUnscaledOffset(const vixl::aarch64::Instruction* instr) override {
    LoadStoreHelper(instr, s64(instr->GetImmLS()), AddrMode::Offset);
}

void VisitLoadStorePreIndex(const vixl::aarch64::Instruction* instr) override {
    LoadStoreHelper(instr, s64(instr->GetImmLS()), AddrMode::PreIndex);
}

void VisitLoadStorePostIndex(const vixl::aarch64::Instruction* instr) override {
    LoadStoreHelper(instr, s64(instr->GetImmLS()), AddrMode::PostIndex);
}

void VisitLoadStoreRegisterOffset(const vixl::aarch64::Instruction* instr) override {
    auto extend = static_cast<Extend>(instr->GetExtendMode());
    u32 shift = u32(instr->GetImmShiftLS()) * instr->GetSizeLS();
    auto rm = ReadXRegister(instr->GetRm());
    auto offset = ExtendOperand(rm, extend, shift);
    LoadStoreHelper(instr, Input{offset}, AddrMode::Offset);
}

void LoadStorePairHelper(const vixl::aarch64::Instruction* instr, AddrMode mode) {
    auto rt = instr->GetRt();
    auto rt2 = instr->GetRt2();
    auto rn = instr->GetRn();

    const bool simd = instr->GetInstructionBits() & (1u << 26);
    const bool is_load = instr->GetInstructionBits() & (1u << 22);
    if ((is_load && rt == rt2) ||
        (mode != AddrMode::Offset && !simd && rn != 31 && (rn == rt || rn == rt2))) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    this->SetLocation(Input{Imm{current_pc}});

    s64 element_size = s64(1u << instr->GetSizeLSPair());
    s64 offset = s64(instr->GetImmLSPair()) * element_size;

    auto base = ReadXRegister(rn, Reg31IsStackPointer);
    Value address, writeback;
    if (mode == AddrMode::Offset) {
        address = AddressAdd(base, offset);
    } else {
        writeback = AddressAdd(base, offset);
        address = (mode == AddrMode::PreIndex) ? writeback : base;
    }
    auto address2 = mode == AddrMode::Offset ? AddressAdd(base, offset + element_size)
                                             : AddressAdd(address, element_size);

    auto load = [&](Value addr, Type type) { return ReadMemory(Input{addr}, type); };

    switch (instr->Mask(LoadStorePairMask)) {
        case LDP_w: {
            const auto first = load(address, Type::U32);
            const auto second = load(address2, Type::U32);
            WriteWRegister(rt, first);
            WriteWRegister(rt2, second);
            break;
        }
        case LDP_x: {
            const auto first = load(address, Type::U64);
            const auto second = load(address2, Type::U64);
            WriteXRegister(rt, first);
            WriteXRegister(rt2, second);
            break;
        }
        case LDPSW_x: {
            const auto first = load(address, Type::U32);
            const auto second = load(address2, Type::U32);
            WriteXRegister(rt, SignExtendValue(first, 32));
            WriteXRegister(rt2, SignExtendValue(second, 32));
            break;
        }
        case STP_w: {
            const auto first = ReadRegister(rt, Type::U32);
            const auto second = ReadRegister(rt2, Type::U32);
            WriteMemory(address, first);
            WriteMemory(address2, second);
            break;
        }
        case STP_x: {
            const auto first = ReadRegister(rt, Type::U64);
            const auto second = ReadRegister(rt2, Type::U64);
            WriteMemory(address, first);
            WriteMemory(address2, second);
            break;
        }
        case LDP_s:
        case LDP_d:
        case LDP_q: {
            const auto op = instr->Mask(LoadStorePairMask);
            const auto type = op == LDP_s ? Type::V32 : op == LDP_d ? Type::V64 : Type::V128;
            const auto first = load(address, type);
            auto second = load(address2, type);
            if (X64FP() && type != Type::V128)
                second = Emit<OpCode::BitCast, Type::V128>(Widen(second));
            WriteVRegister(rt, first);
            WriteVRegister(rt2, second);
            break;
        }
        case STP_s:
        case STP_d:
        case STP_q: {
            const auto op = instr->Mask(LoadStorePairMask);
            const auto type = op == STP_s ? Type::V32 : op == STP_d ? Type::V64 : Type::V128;
            const auto first = ReadVRegister(rt, type), second = ReadVRegister(rt2, type);
            WriteMemory(address, first);
            WriteMemory(address2, second);
            break;
        }
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }
    if (mode != AddrMode::Offset) WriteXRegister(rn, writeback, Reg31IsStackPointer);
}

void VisitLoadStorePairOffset(const vixl::aarch64::Instruction* instr) override {
    LoadStorePairHelper(instr, AddrMode::Offset);
}

void VisitLoadStorePairPreIndex(const vixl::aarch64::Instruction* instr) override {
    LoadStorePairHelper(instr, AddrMode::PreIndex);
}

void VisitLoadStorePairPostIndex(const vixl::aarch64::Instruction* instr) override {
    LoadStorePairHelper(instr, AddrMode::PostIndex);
}

void VisitLoadStorePairNonTemporal(const vixl::aarch64::Instruction* instr) override {
    LoadStorePairHelper(instr, AddrMode::Offset);
}

void VisitLoadStoreExclusive(const vixl::aarch64::Instruction* instr) override {
    const auto request = StateRequest(instr->GetInstructionBits());
    if (request.kind != runtime::arm64::StateServiceKind::None) {
        RequestStateService(request);
        return;
    }
    auto rt = u8(instr->GetRt());
    auto rn = u8(instr->GetRn());
    auto rs = u8(instr->GetRs());
    auto base = ReadXRegister(rn, Reg31IsStackPointer);
    auto masked = instr->Mask(LoadStoreExclusiveMask);

    // Determine access size from the instruction encoding.
    auto size_bits = u32(instr->GetLdStXSizeLog2());
    Type type;
    switch (size_bits) {
        case 0:
            type = Type::U8;
            break;
        case 1:
            type = Type::U16;
            break;
        case 2:
            type = Type::U32;
            break;
        default:
            type = Type::U64;
            break;
    }

    const u32 family = masked & ~(0xC0000000u | LSEBit_l | LSEBit_o0);
    // Bit 31 separates CASP (W/X pairs) from pair exclusive STXP encodings.
    // Clearing both size bits for the family comparison otherwise aliases them.
    if (family == CASPFixed && !(instr->GetInstructionBits() & 0x80000000u)) {
        const bool wide_pair = (instr->GetInstructionBits() & LSEBit_sz) != 0;
        if ((rs & 1) || (rt & 1)) {
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return;
        }
        this->SetLocation(Input{Imm{current_pc}});
        if (wide_pair) {
            this->CheckMemoryAlignment(base, Imm{15});
            auto expected_low = ReadXRegister(rs), expected_high = ReadXRegister(rs + 1);
            auto desired_low = ReadXRegister(rt), desired_high = ReadXRegister(rt + 1);
            auto observed = TypedCAS128(
                    Type::V128, base, expected_low, expected_high, desired_low, desired_high);
            auto observed_low = this->VecExtract64(observed, Imm{0u}).WithView(Type::U64);
            auto observed_high = this->VecExtract64(observed, Imm{1u}).WithView(Type::U64);
            WriteXRegister(rs, observed_low);
            WriteXRegister(rs + 1, observed_high);
            return;
        }
        this->CheckMemoryAlignment(base, Imm{7});
        const auto pack = [&](u8 first) {
            auto low = Widen(ReadRegister(first, Type::U32));
            auto high = this->template LslImm<Type::U64>(Widen(ReadRegister(first + 1, Type::U32)),
                                                         Imm{32})
                                .WithView(Type::U64);
            return this->template Or<Type::U64>(low, SingleOperand(high)).WithView(Type::U64);
        };
        // Read both pairs before writing either observed result: Rs/Rt and
        // the address register may alias a destination pair.
        auto expected = pack(rs);
        auto desired = pack(rt);
        auto old = TypedCAS(Type::U64, base, expected, desired);
        auto high = this->template LsrImm<Type::U64>(old, Imm{32}).WithView(Type::U64);
        WriteWRegister(rs, old);
        WriteWRegister(rs + 1, high);
        return;
    }
    // Size and acquire/release variants share the same scalar CAS operation.
    // The existing atomic IR uses stronger sequentially consistent ordering.
    if (family == CASBFixed) {
        this->CheckMemoryAlignment(base, Imm{(u32{1} << size_bits) - 1});
        auto expected = ReadRegister(rs, type);
        auto desired = ReadRegister(rt, type);
        auto old_val = TypedCAS(type, base, expected, desired);
        if (type == Type::U64)
            WriteXRegister(rs, old_val);
        else
            WriteWRegister(rs, old_val);
        return;
    }
    switch (masked) {
        case LDARB_w:
        case LDARH_w:
        case LDAR_w:
        case LDAR_x:
        case LDLARB:
        case LDLARH:
        case LDLAR_w:
        case LDLAR_x: {
            this->CheckMemoryAlignment(base, Imm{(u32{1} << size_bits) - 1});
            auto value = TypedLoadTSO(type, SingleOperand(base));
            if (type == Type::U64)
                WriteXRegister(rt, value);
            else
                WriteWRegister(rt, value);
            return;
        }
        case STLRB_w:
        case STLRH_w:
        case STLR_w:
        case STLR_x:
        case STLLRB:
        case STLLRH:
        case STLLR_w:
        case STLLR_x:
            this->CheckMemoryAlignment(base, Imm{(u32{1} << size_bits) - 1});
            this->StoreMemoryTSO(SingleOperand(base), ReadRegister(rt, type));
            return;
        default:
            // Reserved fields and unpredictable overlaps are not accepted.
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }
}

void VisitAtomicMemory(const vixl::aarch64::Instruction* instr) override {
    if (TryBaseMemoryExtension(instr)) return;
    auto rs = u8(instr->GetRs());
    auto rt = u8(instr->GetRt());
    auto rn = u8(instr->GetRn());
    auto base = ReadXRegister(rn, Reg31IsStackPointer);
    const u32 bits = instr->GetInstructionBits();
    const u32 size = (bits >> 30) & 3;
    const Type types[] = {Type::U8, Type::U16, Type::U32, Type::U64};
    const auto type = types[size];
    const u32 op = (bits >> 12) & 15;
    if (op > 8) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    this->CheckMemoryAlignment(base, Imm{(u32{1} << size) - 1});
    auto rs_val = ReadRegister(rs, type);
    Value old_val;
    switch (op) {
        case 8:
            old_val = TypedExchange(type, base, rs_val);
            break;
        case 0:
            old_val = TypedFetchAdd(type, base, rs_val);
            break;
        default: {
            const AtomicRMWOp operations[] = {AtomicRMWOp::Add,
                                              AtomicRMWOp::And,
                                              AtomicRMWOp::Xor,
                                              AtomicRMWOp::Or,
                                              AtomicRMWOp::SignedMax,
                                              AtomicRMWOp::SignedMin,
                                              AtomicRMWOp::UnsignedMax,
                                              AtomicRMWOp::UnsignedMin};
            const auto operation = operations[op];
            if (op == 1) {
                rs_val = this->template Xor<Type::U64>(rs_val, SingleOperand(Imm{~u64{0}}))
                                 .WithView(type);
            }
            old_val = TypedRMW(
                    type, Imm{static_cast<u8>(operation)}, base, rs_val, ImmValue(0, Type::U8));
            break;
        }
    }
    if (type == Type::U64)
        WriteXRegister(rt, old_val);
    else
        WriteWRegister(rt, old_val);
}

void VisitLoadStoreRCpcUnscaledOffset(const vixl::aarch64::Instruction* instr) override {
    // User-mode Normal-memory profile: SCTLR.nAA=1 permits accesses crossing
    // a 16-byte boundary. This frontend does not model privileged alignment
    // controls or Device memory. Do not infer this policy from VIXL's simulator,
    // which instead rejects crossings, or from single-threaded oracle results.
    const auto op = instr->Mask(LoadStoreRCpcUnscaledOffsetMask);
    bool load{}, sign{}, destination64{};
    u32 bits{};
    switch (op) {
        case STLURB:
            bits = 8;
            break;
        case STLURH:
            bits = 16;
            break;
        case STLUR_w:
            bits = 32;
            break;
        case STLUR_x:
            bits = 64;
            break;
        case LDAPURB:
            bits = 8;
            load = true;
            break;
        case LDAPURH:
            bits = 16;
            load = true;
            break;
        case LDAPUR_w:
            bits = 32;
            load = true;
            break;
        case LDAPUR_x:
            bits = 64;
            load = destination64 = true;
            break;
        case LDAPURSB_w:
            bits = 8;
            load = sign = true;
            break;
        case LDAPURSB_x:
            bits = 8;
            load = sign = destination64 = true;
            break;
        case LDAPURSH_w:
            bits = 16;
            load = sign = true;
            break;
        case LDAPURSH_x:
            bits = 16;
            load = sign = destination64 = true;
            break;
        case LDAPURSW:
            bits = 32;
            load = sign = destination64 = true;
            break;
        default:
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return;
    }
    this->SetLocation(Input{Imm{current_pc}});
    auto base = ReadXRegister(instr->GetRn(), Reg31IsStackPointer);
    auto address = AddressAdd(base, s64(instr->GetImmLS()));
    const auto type = bits == 8    ? Type::U8
                      : bits == 16 ? Type::U16
                      : bits == 32 ? Type::U32
                                   : Type::U64;
    if (load) {
        auto value = TypedLoadTSO(type, SingleOperand(address));
        if (sign) value = SignExtendValue(value, bits);
        if (destination64)
            WriteXRegister(instr->GetRt(), value);
        else
            WriteWRegister(instr->GetRt(), value);
    } else {
        this->StoreMemoryTSO(SingleOperand(address), ReadRegister(instr->GetRt(), type));
    }
}

bool TryBaseMemoryExtension(const vixl::aarch64::Instruction* instr) {
    const u32 word = instr->GetInstructionBits();
    const bool unprivileged = (word & 0x3B200C00u) == 0x38000800u;
    const bool rcpc = (word & 0x3B20FC00u) == 0x3820C000u;
    if (!unprivileged && !rcpc) return false;
    bool load = false, sign = false, destination64 = false;
    u32 bytes = 0;
    if (rcpc) {
        // LDAPR has no source register. Rs must be exactly 31; no LSE RMW
        // mode or release variant may be silently accepted by this helper.
        if (instr->GetRs() != 31) {
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return true;
        }
        switch (instr->Mask(AtomicMemoryMask)) {
            case LDAPRB:
                bytes = 1;
                break;
            case LDAPRH:
                bytes = 2;
                break;
            case LDAPR_w:
                bytes = 4;
                break;
            case LDAPR_x:
                bytes = 8;
                destination64 = true;
                break;
            default:
                Interrupt(InterruptReason::ILL_CODE, current_pc);
                return true;
        }
        load = true;
    } else {
        // VIXL routes this whole class to VisitUnimplemented, so validate the
        // SIMD bit and all opc/size combinations here, including reserved ones.
        if (word & (1u << 26)) {
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return true;
        }
        switch (instr->Mask(LoadStoreMask)) {
            case STRB_w:
                bytes = 1;
                break;
            case STRH_w:
                bytes = 2;
                break;
            case STR_w:
                bytes = 4;
                break;
            case STR_x:
                bytes = 8;
                break;
            case LDRB_w:
                bytes = 1;
                load = true;
                break;
            case LDRH_w:
                bytes = 2;
                load = true;
                break;
            case LDR_w:
                bytes = 4;
                load = true;
                break;
            case LDR_x:
                bytes = 8;
                load = destination64 = true;
                break;
            case LDRSB_w:
                bytes = 1;
                load = sign = true;
                break;
            case LDRSH_w:
                bytes = 2;
                load = sign = true;
                break;
            case LDRSB_x:
                bytes = 1;
                load = sign = destination64 = true;
                break;
            case LDRSH_x:
                bytes = 2;
                load = sign = destination64 = true;
                break;
            case LDRSW_x:
                bytes = 4;
                load = sign = destination64 = true;
                break;
            default:
                Interrupt(InterruptReason::ILL_CODE, current_pc);
                return true;
        }
    }
    // User-mode Normal memory: LDTR uses the same EL0 permissions as ordinary
    // loads, and RCpc follows the existing nAA=1 unaligned-memory profile.
    // Neither instruction writes the base or performs an exclusive/RMW access.
    this->SetLocation(Input{Imm{current_pc}});
    auto base = ReadXRegister(instr->GetRn(), Reg31IsStackPointer);
    auto address = unprivileged ? AddressAdd(base, s64(instr->GetImmLS())) : base;
    const auto type = bytes == 1   ? Type::U8
                      : bytes == 2 ? Type::U16
                      : bytes == 4 ? Type::U32
                                   : Type::U64;
    if (load) {
        auto value = rcpc ? TypedLoadTSO(type, SingleOperand(address))
                          : ReadMemory(Input{address}, type);
        if (sign) value = SignExtendValue(value, bytes * 8);
        if (destination64)
            WriteXRegister(instr->GetRt(), value);
        else
            WriteWRegister(instr->GetRt(), value);
    } else
        WriteMemory(address, ReadRegister(instr->GetRt(), type));
    return true;
}

void VisitNEONLoadStoreMultiStruct(const vixl::aarch64::Instruction* instr) override {
    NEONMultiMemory(instr, false);
}

void VisitNEONLoadStoreMultiStructPostIndex(const vixl::aarch64::Instruction* instr) override {
    NEONMultiMemory(instr, true);
}

void NEONMultiMemory(const vixl::aarch64::Instruction* instr, bool post_index) {
    const u32 opcode = (instr->GetInstructionBits() >> 12) & 15;
    u32 registers;
    bool interleaved = false;
    switch (opcode) {
        case 7:
            registers = 1;
            break;
        case 10:
            registers = 2;
            break;
        case 6:
            registers = 3;
            break;
        case 2:
            registers = 4;
            break;
        case 8:
            registers = 2;
            interleaved = true;
            break;
        case 4:
            registers = 3;
            interleaved = true;
            break;
        case 0:
            registers = 4;
            interleaved = true;
            break;
        default:
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return;
    }
    const bool q = instr->GetNEONQ();
    const u32 bytes = q ? 16 : 8;
    const u32 size = (instr->GetInstructionBits() >> 10) & 3;
    if (interleaved && size == 3 && !q) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const bool load = (instr->GetInstructionBits() >> 22) & 1;
    const u8 rn = instr->GetRn(), rt = instr->GetRt();
    this->SetLocation(Input{Imm{current_pc}});
    auto base = ReadXRegister(rn, Reg31IsStackPointer);
    auto offset = post_index && instr->GetRm() != 31 ? ReadXRegister(instr->GetRm())
                                                     : ImmValue(bytes * registers, Type::U64);
    auto address = [&](u32 position) {
        return position ? this->Add(base, SingleOperand(Imm{u64{position}})).WithView(Type::U64)
                        : base;
    };
    if (!interleaved) {
        const auto type = q ? Type::V128 : Type::U64;
        if (load) {
            // Keep every old Q register visible until the last read succeeds.
            Value loaded[4];
            for (u32 reg = 0; reg < registers; ++reg)
                loaded[reg] = ReadMemory(Input{address(reg * bytes)}, type);
            for (u32 reg = 0; reg < registers; ++reg) WriteVRegister((rt + reg) & 31, loaded[reg]);
        } else
            for (u32 reg = 0; reg < registers; ++reg)
                WriteMemory(address(reg * bytes), ReadVRegister((rt + reg) & 31, type));
    } else if (registers == 2 && q) {
        const u32 bits = 8u << size;
        if (load) {
            auto first = ReadMemory(Input{base}, Type::V128);
            auto second = ReadMemory(Input{address(16)}, Type::V128);
            auto even = this->VecUnzip(first, second, Imm{bits}, Imm{false}).WithView(Type::V128);
            auto odd = this->VecUnzip(first, second, Imm{bits}, Imm{true}).WithView(Type::V128);
            WriteVRegister(rt, even);
            WriteVRegister((rt + 1) & 31, odd);
        } else {
            auto first = ReadVRegister(rt, Type::V128);
            auto second = ReadVRegister((rt + 1) & 31, Type::V128);
            auto low = this->VecZip(first, second, Imm{bits}, Imm{false}).WithView(Type::V128);
            auto high = this->VecZip(first, second, Imm{bits}, Imm{true}).WithView(Type::V128);
            WriteMemory(base, low);
            WriteMemory(address(16), high);
        }
    } else if (registers == 4 && q) {
        const u32 bits = 8u << size;
        if (load) {
            auto m0 = ReadMemory(Input{base}, Type::V128);
            auto m1 = ReadMemory(Input{address(16)}, Type::V128);
            auto m2 = ReadMemory(Input{address(32)}, Type::V128);
            auto m3 = ReadMemory(Input{address(48)}, Type::V128);
            auto a0 = this->VecUnzip(m0, m1, Imm{bits}, Imm{false}).WithView(Type::V128);
            auto a1 = this->VecUnzip(m2, m3, Imm{bits}, Imm{false}).WithView(Type::V128);
            auto a2 = this->VecUnzip(m0, m1, Imm{bits}, Imm{true}).WithView(Type::V128);
            auto a3 = this->VecUnzip(m2, m3, Imm{bits}, Imm{true}).WithView(Type::V128);
            auto r0 = this->VecUnzip(a0, a1, Imm{bits}, Imm{false}).WithView(Type::V128);
            auto r2 = this->VecUnzip(a0, a1, Imm{bits}, Imm{true}).WithView(Type::V128);
            auto r1 = this->VecUnzip(a2, a3, Imm{bits}, Imm{false}).WithView(Type::V128);
            auto r3 = this->VecUnzip(a2, a3, Imm{bits}, Imm{true}).WithView(Type::V128);
            WriteVRegister(rt, r0);
            WriteVRegister((rt + 1) & 31, r1);
            WriteVRegister((rt + 2) & 31, r2);
            WriteVRegister((rt + 3) & 31, r3);
        } else {
            auto r0 = ReadVRegister(rt, Type::V128);
            auto r1 = ReadVRegister((rt + 1) & 31, Type::V128);
            auto r2 = ReadVRegister((rt + 2) & 31, Type::V128);
            auto r3 = ReadVRegister((rt + 3) & 31, Type::V128);
            auto z0 = this->VecZip(r0, r2, Imm{bits}, Imm{false}).WithView(Type::V128);
            auto z1 = this->VecZip(r0, r2, Imm{bits}, Imm{true}).WithView(Type::V128);
            auto z2 = this->VecZip(r1, r3, Imm{bits}, Imm{false}).WithView(Type::V128);
            auto z3 = this->VecZip(r1, r3, Imm{bits}, Imm{true}).WithView(Type::V128);
            WriteMemory(base, this->VecZip(z0, z2, Imm{bits}, Imm{false}).WithView(Type::V128));
            WriteMemory(address(16),
                        this->VecZip(z0, z2, Imm{bits}, Imm{true}).WithView(Type::V128));
            WriteMemory(address(32),
                        this->VecZip(z1, z3, Imm{bits}, Imm{false}).WithView(Type::V128));
            WriteMemory(address(48),
                        this->VecZip(z1, z3, Imm{bits}, Imm{true}).WithView(Type::V128));
        }
    } else {
        const u32 lane_bytes = 1u << size, bits = lane_bytes * 8;
        const auto lane_type = size == 0   ? Type::U8
                               : size == 1 ? Type::U16
                               : size == 2 ? Type::U32
                                           : Type::U64;
        const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
        Value low[4], high[4];
        for (u32 reg = 0; reg < registers; ++reg) {
            low[reg] = load ? ImmValue(0, Type::U64) : ReadVHalf((rt + reg) & 31, false);
            high[reg] = load ? low[reg] : ReadVHalf((rt + reg) & 31, true);
        }
        for (u32 lane = 0; lane < bytes / lane_bytes; ++lane) {
            const u32 position = lane * bits;
            for (u32 reg = 0; reg < registers; ++reg) {
                auto addr = address((lane * registers + reg) * lane_bytes);
                auto& half = position < 64 ? low[reg] : high[reg];
                if (load) {
                    auto value = Widen(ReadMemory(Input{addr}, lane_type));
                    value = this->LslImm(value, Imm{position % 64}).WithView(Type::U64);
                    half = this->Or(half, SingleOperand(value)).WithView(Type::U64);
                } else {
                    auto value = this->LsrImm(half, Imm{position % 64}).WithView(Type::U64);
                    value = this->And(value, SingleOperand(Imm{mask})).WithView(Type::U64);
                    value = Reinterpret(value, lane_type);
                    WriteMemory(addr, value);
                }
            }
        }
        if (load)
            for (u32 reg = 0; reg < registers; ++reg)
                WriteVHalves((rt + reg) & 31, low[reg], high[reg]);
    }
    if (post_index)
        WriteXRegister(rn,
                       this->Add(base, SingleOperand(offset)).WithView(Type::U64),
                       Reg31IsStackPointer);
}

void VisitNEONLoadStoreSingleStruct(const vixl::aarch64::Instruction* instr) override {
    NEONSingleMemory(instr, false);
}

void VisitNEONLoadStoreSingleStructPostIndex(const vixl::aarch64::Instruction* instr) override {
    NEONSingleMemory(instr, true);
}

void NEONSingleMemory(const vixl::aarch64::Instruction* instr, bool post_index) {
    const u32 word = instr->GetInstructionBits();
    const u32 count = 1 + ((word >> 21) & 1) + 2 * ((word >> 13) & 1);
    const u32 scale = (word >> 14) & 3;
    const u32 size = instr->GetNEONLSSize();
    const bool load = (word >> 22) & 1;
    const bool replicate = scale == 3;
    u32 lane_size;
    if (replicate) {
        if (!load || instr->GetNEONS()) {
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return;
        }
        lane_size = size;
    } else if (scale == 0)
        lane_size = 0;
    else if (scale == 1 && !(size & 1))
        lane_size = 1;
    else if (scale == 2 && !(size & 2) && (!(size & 1) || !instr->GetNEONS()))
        lane_size = 2 + (size & 1);
    else {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const u32 bytes = 1u << lane_size, bits = bytes * 8;
    const u32 lane = replicate ? 0 : instr->GetNEONLSIndex(lane_size);
    const u32 position = lane * bits;
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    const auto type = lane_size == 0   ? Type::U8
                      : lane_size == 1 ? Type::U16
                      : lane_size == 2 ? Type::U32
                                       : Type::U64;
    const u8 rn = instr->GetRn(), rt = instr->GetRt();
    this->SetLocation(Input{Imm{current_pc}});
    auto base = ReadXRegister(rn, Reg31IsStackPointer);
    auto offset = post_index && instr->GetRm() != 31 ? ReadXRegister(instr->GetRm())
                                                     : ImmValue(bytes * count, Type::U64);
    // Lane and replicate loads share the same all-reads-before-Q-commit rule.
    Value loaded[4];
    if (load)
        for (u32 reg = 0; reg < count; ++reg) {
            auto address =
                    reg ? this->Add(base, SingleOperand(Imm{u64{reg * bytes}})).WithView(Type::U64)
                        : base;
            loaded[reg] = ReadMemory(Input{address}, type);
        }
    for (u32 reg = 0; reg < count; ++reg) {
        const u8 code = (rt + reg) & 31;
        if (load) {
            auto value = Widen(loaded[reg]);
            if (replicate) {
                for (u32 shift = bits; shift < 64; shift *= 2) {
                    auto copy = this->LslImm(value, Imm{shift}).WithView(Type::U64);
                    value = this->Or(value, SingleOperand(copy)).WithView(Type::U64);
                }
                if (X64FP())
                    WriteVRegister(
                            code,
                            instr->GetNEONQ() ? this->VecDup64(value).WithView(Type::V128) : value);
                else
                    WriteVHalves(code, value, instr->GetNEONQ() ? value : ImmValue(0, Type::U64));
            } else {
                auto old = ReadVHalf(code, position >= 64);
                old = this->And(old, SingleOperand(Imm{~(mask << (position % 64))}))
                              .WithView(Type::U64);
                value = this->LslImm(value, Imm{position % 64}).WithView(Type::U64);
                value = this->Or(old, SingleOperand(value)).WithView(Type::U64);
                const auto uniform_offset = u32(offsetof(GuestState, q) + code * sizeof(u128)) +
                                            (position >= 64 ? 8u : 0u);
                // Lane loads preserve every untouched bit, including the high half at Q=0.
                this->StoreUniform(Uniform{uniform_offset, Type::U64}, value);
            }
        } else {
            auto address =
                    reg ? this->Add(base, SingleOperand(Imm{u64{reg * bytes}})).WithView(Type::U64)
                        : base;
            auto value = ReadVHalf(code, position >= 64);
            value = this->LsrImm(value, Imm{position % 64}).WithView(Type::U64);
            value = this->And(value, SingleOperand(Imm{mask})).WithView(Type::U64);
            value = Reinterpret(value, type);
            WriteMemory(address, value);
        }
    }
    if (post_index)
        WriteXRegister(rn,
                       this->Add(base, SingleOperand(offset)).WithView(Type::U64),
                       Reg31IsStackPointer);
}
