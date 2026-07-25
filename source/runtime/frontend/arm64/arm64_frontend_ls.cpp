#include "arm64_frontend_internal.h"

namespace swift::arm64 {

#define __ assembler->

void A64Decoder::VisitLoadLiteral(const Instruction* instr) {
    unsigned rt = instr->GetRt();
    auto address = instr->GetLiteralAddress<VAddr>(current_pc);

    // Verify that the calculated address is available to the host.
    VIXL_ASSERT(address == static_cast<uintptr_t>(address));

    switch (instr->Mask(LoadLiteralMask)) {
        case LDR_w_lit:
            WriteWRegister(rt, ReadMemory(ir::Lambda{ir::Imm{address}}, ir::ValueType::U32));
            break;
        case LDR_x_lit:
            WriteXRegister(rt, ReadMemory(ir::Lambda{ir::Imm{address}}, ir::ValueType::U64));
            break;
        case LDR_s_lit:
            WriteSRegister(rt, ReadMemory(ir::Lambda{ir::Imm{address}}, ir::ValueType::V32));
            break;
        case LDR_d_lit:
            WriteDRegister(rt, ReadMemory(ir::Lambda{ir::Imm{address}}, ir::ValueType::V64));
            break;
        case LDR_q_lit:
            WriteQRegister(rt, ReadMemory(ir::Lambda{ir::Imm{address}}, ir::ValueType::V128));
            break;
        case LDRSW_x_lit:
            WriteXRegister(
                    rt,
                    SignExtendValue(ReadMemory(ir::Lambda{ir::Imm{address}}, ir::ValueType::U32),
                                    32));
            break;

        // Ignore prfm hint instructions.
        case PRFM_lit:
            break;

        default:
            VIXL_UNREACHABLE();
    }
}

void A64Decoder::LoadStoreHelper(const Instruction* instr, s64 offset, AddrMode mode) {
    LoadStoreHelper(instr, ir::DataClass{ir::Imm{offset}}, mode);
}

void A64Decoder::LoadStoreHelper(const Instruction* instr,
                                 const ir::DataClass& offset,
                                 AddrMode mode) {
    auto rt = instr->GetRt();
    auto rn = instr->GetRn();

    auto base = ReadXRegister(rn, Reg31IsStackPointer);

    auto add_offset = [&](ir::Value b) {
        if (offset.IsImm()) {
            return AddressAdd(b, offset.imm.GetSigned());
        }
        return __ Add<ir::U64>(b, SingleOperand(offset.value)).SetType(ir::ValueType::U64);
    };

    ir::Value address;
    if (mode == AddrMode::Offset) {
        address = add_offset(base);
    } else {
        auto writeback = add_offset(base);
        WriteXRegister(rn, writeback, Reg31IsStackPointer);
        address = (mode == AddrMode::PreIndex) ? writeback : base;
    }

    switch (static_cast<LoadStoreOp>(instr->Mask(LoadStoreMask))) {
        case LDRB_w:
            WriteWRegister(rt, ReadMemory(ir::Lambda{address}, ir::ValueType::U8));
            break;
        case LDRH_w:
            WriteWRegister(rt, ReadMemory(ir::Lambda{address}, ir::ValueType::U16));
            break;
        case LDR_w:
            WriteWRegister(rt, ReadMemory(ir::Lambda{address}, ir::ValueType::U32));
            break;
        case LDR_x:
            WriteXRegister(rt, ReadMemory(ir::Lambda{address}, ir::ValueType::U64));
            break;
        case LDRSB_w:
            WriteWRegister(rt, SignExtendValue(ReadMemory(ir::Lambda{address}, ir::ValueType::U8), 8));
            break;
        case LDRSH_w:
            WriteWRegister(rt,
                           SignExtendValue(ReadMemory(ir::Lambda{address}, ir::ValueType::U16), 16));
            break;
        case LDRSB_x:
            WriteXRegister(rt, SignExtendValue(ReadMemory(ir::Lambda{address}, ir::ValueType::U8), 8));
            break;
        case LDRSH_x:
            WriteXRegister(rt,
                           SignExtendValue(ReadMemory(ir::Lambda{address}, ir::ValueType::U16), 16));
            break;
        case LDRSW_x:
            WriteXRegister(rt,
                           SignExtendValue(ReadMemory(ir::Lambda{address}, ir::ValueType::U32), 32));
            break;
        case STRB_w:
            WriteMemory(address, ReadRegister(rt, ir::ValueType::U8));
            break;
        case STRH_w:
            WriteMemory(address, ReadRegister(rt, ir::ValueType::U16));
            break;
        case STR_w:
            WriteMemory(address, ReadRegister(rt, ir::ValueType::U32));
            break;
        case STR_x:
            WriteMemory(address, ReadRegister(rt, ir::ValueType::U64));
            break;
        case LDR_b:
            WriteVRegister(rt, ReadMemory(ir::Lambda{address}, ir::ValueType::V8));
            break;
        case LDR_h:
            WriteVRegister(rt, ReadMemory(ir::Lambda{address}, ir::ValueType::V16));
            break;
        case LDR_s:
            WriteVRegister(rt, ReadMemory(ir::Lambda{address}, ir::ValueType::V32));
            break;
        case LDR_d:
            WriteVRegister(rt, ReadMemory(ir::Lambda{address}, ir::ValueType::V64));
            break;
        case LDR_q:
            WriteVRegister(rt, ReadMemory(ir::Lambda{address}, ir::ValueType::V128));
            break;
        case STR_b:
            WriteMemory(address, ReadVRegister(rt, ir::ValueType::V8));
            break;
        case STR_h:
            WriteMemory(address, ReadVRegister(rt, ir::ValueType::V16));
            break;
        case STR_s:
            WriteMemory(address, ReadVRegister(rt, ir::ValueType::V32));
            break;
        case STR_d:
            WriteMemory(address, ReadVRegister(rt, ir::ValueType::V64));
            break;
        case STR_q:
            WriteMemory(address, ReadVRegister(rt, ir::ValueType::V128));
            break;

        // Ignore prfm hint instructions.
        case PRFM:
            break;

        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            break;
    }
}

void A64Decoder::VisitLoadStoreUnsignedOffset(const Instruction* instr) {
    s64 offset = s64(instr->GetImmLSUnsigned()) << instr->GetSizeLS();
    LoadStoreHelper(instr, offset, AddrMode::Offset);
}

void A64Decoder::VisitLoadStoreUnscaledOffset(const Instruction* instr) {
    LoadStoreHelper(instr, s64(instr->GetImmLS()), AddrMode::Offset);
}

void A64Decoder::VisitLoadStorePreIndex(const Instruction* instr) {
    LoadStoreHelper(instr, s64(instr->GetImmLS()), AddrMode::PreIndex);
}

void A64Decoder::VisitLoadStorePostIndex(const Instruction* instr) {
    LoadStoreHelper(instr, s64(instr->GetImmLS()), AddrMode::PostIndex);
}

void A64Decoder::VisitLoadStoreRegisterOffset(const Instruction* instr) {
    auto extend = static_cast<Extend>(instr->GetExtendMode());
    u32 shift = u32(instr->GetImmShiftLS()) * instr->GetSizeLS();
    auto rm = ReadXRegister(instr->GetRm());
    auto offset = ExtendOperand(rm, extend, shift);
    LoadStoreHelper(instr, ir::DataClass{offset}, AddrMode::Offset);
}

void A64Decoder::LoadStorePairHelper(const Instruction* instr, AddrMode mode) {
    auto rt = instr->GetRt();
    auto rt2 = instr->GetRt2();
    auto rn = instr->GetRn();

    s64 element_size = s64(1u << instr->GetSizeLSPair());
    s64 offset = s64(instr->GetImmLSPair()) * element_size;

    auto base = ReadXRegister(rn, Reg31IsStackPointer);
    ir::Value address;
    if (mode == AddrMode::Offset) {
        address = AddressAdd(base, offset);
    } else {
        auto writeback = AddressAdd(base, offset);
        WriteXRegister(rn, writeback, Reg31IsStackPointer);
        address = (mode == AddrMode::PreIndex) ? writeback : base;
    }
    auto address2 = AddressAdd(address, element_size);

    auto load = [&](ir::Value addr, ir::ValueType type) {
        return ReadMemory(ir::Lambda{addr}, type);
    };

    switch (instr->Mask(LoadStorePairMask)) {
        case LDP_w:
            WriteWRegister(rt, load(address, ir::ValueType::U32));
            WriteWRegister(rt2, load(address2, ir::ValueType::U32));
            break;
        case LDP_x:
            WriteXRegister(rt, load(address, ir::ValueType::U64));
            WriteXRegister(rt2, load(address2, ir::ValueType::U64));
            break;
        case LDPSW_x:
            WriteXRegister(rt, SignExtendValue(load(address, ir::ValueType::U32), 32));
            WriteXRegister(rt2, SignExtendValue(load(address2, ir::ValueType::U32), 32));
            break;
        case STP_w:
            WriteMemory(address, ReadRegister(rt, ir::ValueType::U32));
            WriteMemory(address2, ReadRegister(rt2, ir::ValueType::U32));
            break;
        case STP_x:
            WriteMemory(address, ReadRegister(rt, ir::ValueType::U64));
            WriteMemory(address2, ReadRegister(rt2, ir::ValueType::U64));
            break;
        case LDP_s:
            WriteVRegister(rt, load(address, ir::ValueType::V32));
            WriteVRegister(rt2, load(address2, ir::ValueType::V32));
            break;
        case LDP_d:
            WriteVRegister(rt, load(address, ir::ValueType::V64));
            WriteVRegister(rt2, load(address2, ir::ValueType::V64));
            break;
        case LDP_q:
            WriteVRegister(rt, load(address, ir::ValueType::V128));
            WriteVRegister(rt2, load(address2, ir::ValueType::V128));
            break;
        case STP_s:
            WriteMemory(address, ReadVRegister(rt, ir::ValueType::V32));
            WriteMemory(address2, ReadVRegister(rt2, ir::ValueType::V32));
            break;
        case STP_d:
            WriteMemory(address, ReadVRegister(rt, ir::ValueType::V64));
            WriteMemory(address2, ReadVRegister(rt2, ir::ValueType::V64));
            break;
        case STP_q:
            WriteMemory(address, ReadVRegister(rt, ir::ValueType::V128));
            WriteMemory(address2, ReadVRegister(rt2, ir::ValueType::V128));
            break;
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            break;
    }
}

void A64Decoder::VisitLoadStorePairOffset(const Instruction* instr) {
    LoadStorePairHelper(instr, AddrMode::Offset);
}

void A64Decoder::VisitLoadStorePairPreIndex(const Instruction* instr) {
    LoadStorePairHelper(instr, AddrMode::PreIndex);
}

void A64Decoder::VisitLoadStorePairPostIndex(const Instruction* instr) {
    LoadStorePairHelper(instr, AddrMode::PostIndex);
}

void A64Decoder::VisitLoadStorePairNonTemporal(const Instruction* instr) {
    LoadStorePairHelper(instr, AddrMode::Offset);
}

void A64Decoder::VisitLoadStoreExclusive(const Instruction* instr) {
    auto rt = u8(instr->GetRt());
    auto rn = u8(instr->GetRn());
    auto rs = u8(instr->GetRs());
    auto base = ReadXRegister(rn, Reg31IsStackPointer);
    auto addr = ir::Lambda{base};
    auto masked = instr->Mask(LoadStoreExclusiveMask);

    // Determine access size from the instruction encoding.
    auto size_bits = u32(instr->GetLdStXSizeLog2());
    ir::ValueType type;
    switch (size_bits) {
        case 0: type = ir::ValueType::U8; break;
        case 1: type = ir::ValueType::U16; break;
        case 2: type = ir::ValueType::U32; break;
        default: type = ir::ValueType::U64; break;
    }

    bool is_load = instr->GetLdStXLoad() != 0;
    bool is_exclusive = instr->GetLdStXNotExclusive() == 0;
    bool is_pair = instr->GetLdStXPair() != 0;

    // STLR / LDAR (release/acquire, non-exclusive)
    if (!is_exclusive) {
        if (is_load) {
            auto val = ReadMemory(addr, type);
            if (type == ir::ValueType::U64) WriteXRegister(rt, val);
            else WriteWRegister(rt, val);
        } else {
            auto val = ReadRegister(rt, type);
            WriteMemory(base, val);
        }
        return;
    }

    if (is_pair) {
        // LDXP / STXP / LDAXP / STLXP
        auto rt2 = u8(instr->GetRt2());
        auto base2 = AddressAdd(base, s64(1 << size_bits));
        if (is_load) {
            auto v0 = ReadMemory(ir::Lambda{base}, type);
            auto v1 = ReadMemory(ir::Lambda{base2}, type);
            if (type == ir::ValueType::U64) { WriteXRegister(rt, v0); WriteXRegister(rt2, v1); }
            else { WriteWRegister(rt, v0); WriteWRegister(rt2, v1); }
        } else {
            WriteMemory(base, ReadRegister(rt, type));
            WriteMemory(base2, ReadRegister(rt2, type));
            WriteWRegister(rs, ImmValue(0, ir::ValueType::U32));
        }
        return;
    }

    // CAS / CASA / CASAL / CASL (compare-and-swap)
    if (instr->Mask(0x3FE07C00) == 0x08A00000) {
        auto old_val = ReadMemory(addr, type);
        auto desired = ReadRegister(rt, type);
        WriteMemory(base, desired);
        if (type == ir::ValueType::U64) WriteXRegister(rs, old_val);
        else WriteWRegister(rs, old_val);
        return;
    }

    // CASP (pair compare-and-swap)
    if (instr->Mask(0x3FE07C00) == 0x08200000) {
        auto rt2 = u8(rt + 1);
        auto rs2 = u8(rs + 1);
        auto base2 = AddressAdd(base, s64(1 << size_bits));
        auto old0 = ReadMemory(ir::Lambda{base}, type);
        auto old1 = ReadMemory(ir::Lambda{base2}, type);
        WriteMemory(base, ReadRegister(rt, type));
        WriteMemory(base2, ReadRegister(rt2, type));
        if (type == ir::ValueType::U64) { WriteXRegister(rs, old0); WriteXRegister(rs2, old1); }
        else { WriteWRegister(rs, old0); WriteWRegister(rs2, old1); }
        return;
    }

    // LDXR / LDAXR / STXR / STLXR
    if (is_load) {
        auto val = ReadMemory(addr, type);
        if (type == ir::ValueType::U64) WriteXRegister(rt, val);
        else WriteWRegister(rt, val);
    } else {
        WriteMemory(base, ReadRegister(rt, type));
        WriteWRegister(rs, ImmValue(0, ir::ValueType::U32));  // success
    }
}

void A64Decoder::VisitAtomicMemory(const Instruction* instr) {
    auto rs = u8(instr->GetRs());
    auto rt = u8(instr->GetRt());
    auto rn = u8(instr->GetRn());
    auto base = ReadXRegister(rn, Reg31IsStackPointer);
    auto addr = ir::Lambda{base};
    bool is64 = instr->GetSixtyFourBits();
    auto type = GPRType(is64);

    // SWP (bits [14:12] = 000, bit 15 = 1)
    auto bits = instr->GetInstructionBits();
    bool is_swp = (bits & 0x8000) != 0;  // bit 15

    auto old_val = ReadMemory(addr, type);
    auto rs_val = ReadRegister(rs, type);

    if (is_swp) {
        WriteMemory(base, rs_val);
    } else {
        u32 opc = (bits >> 12) & 0x7;
        ir::Value new_val{};
        switch (opc) {
            case 0: // LDADD
                new_val = is64 ? __ Add<ir::U64>(old_val, SingleOperand(rs_val)).SetType(type)
                               : __ Add<ir::U32>(old_val, SingleOperand(rs_val)).SetType(type);
                break;
            case 1: // LDCLR
                new_val = is64 ? __ AndNot<ir::U64>(old_val, SingleOperand(rs_val)).SetType(type)
                               : __ AndNot<ir::U32>(old_val, SingleOperand(rs_val)).SetType(type);
                break;
            case 2: // LDEOR
                new_val = is64 ? __ Xor<ir::U64>(old_val, SingleOperand(rs_val)).SetType(type)
                               : __ Xor<ir::U32>(old_val, SingleOperand(rs_val)).SetType(type);
                break;
            case 3: // LDSET
                new_val = is64 ? __ Or<ir::U64>(old_val, SingleOperand(rs_val)).SetType(type)
                               : __ Or<ir::U32>(old_val, SingleOperand(rs_val)).SetType(type);
                break;
            default:
                Interrupt(InterruptReason::FALLBACK, current_pc);
                return;
        }
        WriteMemory(base, new_val);
    }

    if (rt != 31) {
        if (is64) WriteXRegister(rt, old_val);
        else WriteWRegister(rt, old_val);
    }
}

}  // namespace swift::arm64
