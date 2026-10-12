#include "arm64_frontend_internal.h"
#include "runtime/ir/atomic_rmw.h"

namespace swift::arm64 {

#define __ assembler->

void A64Decoder::VisitLoadLiteral(const Instruction* instr) {
    __ SetLocation(ir::Lambda{ir::Imm{current_pc}});
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

    // Writeback overlap is constrained-unpredictable. Request fallback
    // before mutation instead of choosing a value from aliased registers.
    if (mode != AddrMode::Offset && !(instr->GetInstructionBits() & (1u << 26)) &&
        rn != 31 && rn == rt) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    __ SetLocation(ir::Lambda{ir::Imm{current_pc}});

    auto base = ReadXRegister(rn, Reg31IsStackPointer);

    auto add_offset = [&](ir::Value b) {
        if (offset.IsImm()) {
            return AddressAdd(b, offset.imm.GetSigned());
        }
        return __ Add<ir::U64>(b, SingleOperand(offset.value)).SetType(ir::ValueType::U64);
    };

    ir::Value address, writeback;
    if (mode == AddrMode::Offset) {
        address = add_offset(base);
    } else {
        writeback = add_offset(base);
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
            return;
    }
    // A memory fault exits before this architectural publication.
    if (mode != AddrMode::Offset) WriteXRegister(rn, writeback, Reg31IsStackPointer);
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

    const bool simd = instr->GetInstructionBits() & (1u << 26);
    const bool is_load = instr->GetInstructionBits() & (1u << 22);
    if ((is_load && rt == rt2) ||
        (mode != AddrMode::Offset && !simd && rn != 31 && (rn == rt || rn == rt2))) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    __ SetLocation(ir::Lambda{ir::Imm{current_pc}});

    s64 element_size = s64(1u << instr->GetSizeLSPair());
    s64 offset = s64(instr->GetImmLSPair()) * element_size;

    auto base = ReadXRegister(rn, Reg31IsStackPointer);
    ir::Value address, writeback;
    if (mode == AddrMode::Offset) {
        address = AddressAdd(base, offset);
    } else {
        writeback = AddressAdd(base, offset);
        address = (mode == AddrMode::PreIndex) ? writeback : base;
    }
    auto address2 = AddressAdd(address, element_size);

    auto load = [&](ir::Value addr, ir::ValueType type) {
        return ReadMemory(ir::Lambda{addr}, type);
    };

    switch (instr->Mask(LoadStorePairMask)) {
        case LDP_w: {
            const auto first = load(address, ir::ValueType::U32);
            const auto second = load(address2, ir::ValueType::U32);
            WriteWRegister(rt, first); WriteWRegister(rt2, second);
            break;
        }
        case LDP_x: {
            const auto first = load(address, ir::ValueType::U64);
            const auto second = load(address2, ir::ValueType::U64);
            WriteXRegister(rt, first); WriteXRegister(rt2, second);
            break;
        }
        case LDPSW_x: {
            const auto first = load(address, ir::ValueType::U32);
            const auto second = load(address2, ir::ValueType::U32);
            WriteXRegister(rt, SignExtendValue(first, 32));
            WriteXRegister(rt2, SignExtendValue(second, 32));
            break;
        }
        case STP_w: {
            const auto first = ReadRegister(rt, ir::ValueType::U32);
            const auto second = ReadRegister(rt2, ir::ValueType::U32);
            WriteMemory(address, first); WriteMemory(address2, second);
            break;
        }
        case STP_x: {
            const auto first = ReadRegister(rt, ir::ValueType::U64);
            const auto second = ReadRegister(rt2, ir::ValueType::U64);
            WriteMemory(address, first); WriteMemory(address2, second);
            break;
        }
        case LDP_s: case LDP_d: case LDP_q: {
            const auto op = instr->Mask(LoadStorePairMask);
            const auto type = op == LDP_s ? ir::ValueType::V32 : op == LDP_d ? ir::ValueType::V64 : ir::ValueType::V128;
            const auto first = load(address, type), second = load(address2, type);
            WriteVRegister(rt, first); WriteVRegister(rt2, second);
            break;
        }
        case STP_s: case STP_d: case STP_q: {
            const auto op = instr->Mask(LoadStorePairMask);
            const auto type = op == STP_s ? ir::ValueType::V32 : op == STP_d ? ir::ValueType::V64 : ir::ValueType::V128;
            const auto first = ReadVRegister(rt, type), second = ReadVRegister(rt2, type);
            WriteMemory(address, first); WriteMemory(address2, second);
            break;
        }
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }
    if (mode != AddrMode::Offset) WriteXRegister(rn, writeback, Reg31IsStackPointer);
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

    const u32 family = masked & ~(0xC0000000u | LSEBit_l | LSEBit_o0);
    // Bit 31 separates CASP (W/X pairs) from pair exclusive STXP encodings.
    // Clearing both size bits for the family comparison otherwise aliases them.
    if (family == CASPFixed && !(instr->GetInstructionBits() & 0x80000000u)) {
        const bool wide_pair = (instr->GetInstructionBits() & LSEBit_sz) != 0;
        if ((rs & 1) || (rt & 1)) {
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return;
        }
        __ SetLocation(ir::Lambda{ir::Imm{current_pc}});
        if (wide_pair) {
            __ CheckMemoryAlignment(base, ir::Imm{15});
            auto expected_low = ReadXRegister(rs), expected_high = ReadXRegister(rs + 1);
            auto desired_low = ReadXRegister(rt), desired_high = ReadXRegister(rt + 1);
            auto observed = __ CompareAndSwap128(base, expected_low, expected_high, desired_low, desired_high)
                                .SetType(ir::ValueType::V128);
            auto observed_low = __ VecExtract64(observed, ir::Imm{0u}).SetType(ir::ValueType::U64);
            auto observed_high = __ VecExtract64(observed, ir::Imm{1u}).SetType(ir::ValueType::U64);
            WriteXRegister(rs, observed_low);
            WriteXRegister(rs + 1, observed_high);
            return;
        }
        __ CheckMemoryAlignment(base, ir::Imm{7});
        const auto pack = [&](u8 first) {
            auto low = Widen(ReadRegister(first, ir::ValueType::U32));
            auto high = __ LslImm<ir::U64>(Widen(ReadRegister(first + 1, ir::ValueType::U32)),
                                          ir::Imm{32}).SetType(ir::ValueType::U64);
            return __ Or<ir::U64>(low, SingleOperand(high)).SetType(ir::ValueType::U64);
        };
        // Read both pairs before writing either observed result: Rs/Rt and
        // the address register may alias a destination pair.
        auto expected = pack(rs);
        auto desired = pack(rt);
        auto old = __ CompareAndSwap(base, expected, desired).SetType(ir::ValueType::U64);
        auto high = __ LsrImm<ir::U64>(old, ir::Imm{32}).SetType(ir::ValueType::U64);
        WriteWRegister(rs, old);
        WriteWRegister(rs + 1, high);
        return;
    }
    // Size and acquire/release variants share the same scalar CAS operation.
    // The existing atomic IR uses stronger sequentially consistent ordering.
    if (family == CASBFixed) {
        __ CheckMemoryAlignment(base, ir::Imm{(u32{1} << size_bits) - 1});
        auto expected = ReadRegister(rs, type);
        auto desired = ReadRegister(rt, type);
        auto old_val = __ CompareAndSwap(base, expected, desired).SetType(type);
        if (type == ir::ValueType::U64) WriteXRegister(rs, old_val);
        else WriteWRegister(rs, old_val);
        return;
    }
    switch (masked) {
        case LDARB_w: case LDARH_w: case LDAR_w: case LDAR_x:
        case LDLARB: case LDLARH: case LDLAR_w: case LDLAR_x: {
            __ CheckMemoryAlignment(base, ir::Imm{(u32{1} << size_bits) - 1});
            auto value = __ LoadMemoryTSO(SingleOperand(base)).SetType(type);
            if (type == ir::ValueType::U64) WriteXRegister(rt, value);
            else WriteWRegister(rt, value);
            return;
        }
        case STLRB_w: case STLRH_w: case STLR_w: case STLR_x:
        case STLLRB: case STLLRH: case STLLR_w: case STLLR_x:
            __ CheckMemoryAlignment(base, ir::Imm{(u32{1} << size_bits) - 1});
            __ StoreMemoryTSO(SingleOperand(base), ReadRegister(rt, type));
            return;
        default:
            // LDXR/STXR and pair exclusives need a per-thread reservation
            // monitor.
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }
}

void A64Decoder::VisitAtomicMemory(const Instruction* instr) {
    if (TryBaseMemoryExtension(instr)) return;
    auto rs = u8(instr->GetRs());
    auto rt = u8(instr->GetRt());
    auto rn = u8(instr->GetRn());
    auto base = ReadXRegister(rn, Reg31IsStackPointer);
    const u32 bits = instr->GetInstructionBits();
    const u32 size = (bits >> 30) & 3;
    const ir::ValueType types[] = {ir::ValueType::U8, ir::ValueType::U16,
                                   ir::ValueType::U32, ir::ValueType::U64};
    const auto type = types[size];
    const u32 op = (bits >> 12) & 15;
    if (op > 8) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    __ CheckMemoryAlignment(base, ir::Imm{(u32{1} << size) - 1});
    auto rs_val = ReadRegister(rs, type);
    ir::Value old_val;
    switch (op) {
        case 8:
            old_val = __ AtomicExchange(base, rs_val).SetType(type);
            break;
        case 0:
            old_val = __ AtomicFetchAdd(base, rs_val).SetType(type);
            break;
        default: {
            const ir::AtomicRMWOp operations[] = {ir::AtomicRMWOp::Add, ir::AtomicRMWOp::And,
                ir::AtomicRMWOp::Xor, ir::AtomicRMWOp::Or, ir::AtomicRMWOp::SignedMax,
                ir::AtomicRMWOp::SignedMin, ir::AtomicRMWOp::UnsignedMax, ir::AtomicRMWOp::UnsignedMin};
            const auto operation = operations[op];
            if (op == 1) {
                rs_val = __ Xor<ir::U64>(rs_val, SingleOperand(ir::Imm{~u64{0}})).SetType(type);
            }
            old_val = __ AtomicRMW(ir::Imm{static_cast<u8>(operation)}, base, rs_val,
                                   ImmValue(0, ir::ValueType::U8)).SetType(type);
            break;
        }
    }
    if (type == ir::ValueType::U64) WriteXRegister(rt, old_val);
    else WriteWRegister(rt, old_val);
}

}  // namespace swift::arm64
