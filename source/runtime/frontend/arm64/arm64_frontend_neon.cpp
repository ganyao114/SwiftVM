#include "arm64_frontend_internal.h"

namespace swift::arm64 {

#define __ assembler->

static u32 VRegOff(u8 code) {
    return u32(offsetof(ThreadContext64, v) + code * sizeof(u128));
}

void A64Decoder::VisitNEONCopy(const Instruction* instr) {
    auto imm5 = u32(instr->GetImmNEON5());
    bool is_q = instr->GetNEONQ() != 0;
    auto rn = u8(instr->GetRn());
    auto rd = u8(instr->GetRd());
    // op bit is bit 29 (0 = DUP/INS, 1 = UMOV/SMOV)
    u32 op = (instr->GetInstructionBits() >> 29) & 1;

    // Element size from imm5 (lowest set bit).
    u32 esize, index;
    if (imm5 & 0x1)       { esize = 1; index = imm5 >> 1; }
    else if (imm5 & 0x2)  { esize = 2; index = imm5 >> 2; }
    else if (imm5 & 0x4)  { esize = 4; index = imm5 >> 3; }
    else if (imm5 & 0x8)  { esize = 8; index = imm5 >> 4; }
    else if (imm5 & 0x10) { esize = 16; index = imm5 >> 5; }
    else { Interrupt(InterruptReason::FALLBACK, current_pc); return; }

    if (op == 0) {
        // DUP Vd.T, Rn — broadcast GPR to all lanes.
        // Read the source GPR and mask to element width.
        ir::Value src = (esize <= 4)
            ? Widen(ReadRegister(rn, ir::ValueType::U32))
            : ReadXRegister(rn);
        if (esize < 8) {
            u64 mask = (esize == 1) ? 0xFF : (esize == 2) ? 0xFFFF : 0xFFFFFFFF;
            src = __ And<ir::U64>(src, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
        }
        // Build the broadcast pattern step by step using shift+or.
        // Each step doubles the number of copies.
        ir::Value half = src;
        u32 shift = esize * 8;
        while (shift < 64) {
            auto shifted = __ LslImm<ir::U64>(half, ir::Imm{shift}).SetType(ir::ValueType::U64);
            half = __ Or<ir::U64>(half, SingleOperand(shifted)).SetType(ir::ValueType::U64);
            shift *= 2;
        }
        auto voff = VRegOff(rd);
        __ StoreUniform(ir::Uniform{voff, ir::ValueType::U64}, half);
        __ StoreUniform(ir::Uniform{voff + 8, ir::ValueType::U64},
                        is_q ? half : ImmValue(0, ir::ValueType::U64));
    } else if (op == 1) {
        // UMOV Rd, Vn.T[index] — extract element to GPR.
        auto voff = VRegOff(rn);
        u32 byte_off = index * esize;
        u32 half_sel = byte_off >= 8 ? 8 : 0;
        u32 shift_in_half = (byte_off - half_sel) * 8;
        ir::Value val = __ LoadUniform(ir::Uniform{voff + half_sel, ir::ValueType::U64}).SetType(ir::ValueType::U64);
        if (shift_in_half) {
            val = __ LsrImm<ir::U64>(val, ir::Imm{shift_in_half}).SetType(ir::ValueType::U64);
        }
        if (esize < 8) {
            u64 mask = (esize == 1) ? 0xFF : (esize == 2) ? 0xFFFF : 0xFFFFFFFF;
            val = __ And<ir::U64>(val, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
        }
        if (is_q) { WriteXRegister(rd, val); }
        else      { WriteWRegister(rd, val); }
    } else {
        Interrupt(InterruptReason::FALLBACK, current_pc);
    }
}

void A64Decoder::VisitNEONModifiedImmediate(const Instruction* instr) {
    const u32 cmode = instr->GetNEONCmode();
    const u32 op_bit = instr->GetNEONModImmOp();
    const u64 imm8 = instr->GetImmNEONabcdefgh();
    const u32 cmode_3_1 = (cmode >> 1) & 7;

    u64 lane{};
    u32 lane_bits{};
    switch (cmode_3_1) {
        case 0:
        case 1:
        case 2:
        case 3:
            lane_bits = 32;
            lane = imm8 << (8 * cmode_3_1);
            break;
        case 4:
        case 5:
            lane_bits = 16;
            lane = imm8 << (8 * ((cmode >> 1) & 1));
            break;
        case 6:
            lane_bits = 32;
            lane = (cmode & 1) ? ((imm8 << 16) | 0xFFFF) : ((imm8 << 8) | 0xFF);
            break;
        case 7:
            if ((cmode & 1) || op_bit) {
                Interrupt(InterruptReason::FALLBACK, current_pc);
                return;
            }
            lane_bits = 8;
            lane = imm8;
            break;
        default:
            PANIC();
    }

    u64 fill;
    switch (lane_bits) {
        case 8: fill = lane * u64(0x0101010101010101); break;
        case 16: fill = lane * u64(0x0001000100010001); break;
        case 32: fill = lane | (lane << 32); break;
        default: PANIC();
    }

    const bool is_logic = (cmode & 1) != 0;
    auto apply = [&](bool high) {
        ir::Value result;
        if (!is_logic) {
            result = ImmValue(op_bit ? ~fill : fill, ir::ValueType::U64);
        } else {
            auto old = ReadVHalf(instr->GetRd(), high);
            result = op_bit
                    ? __ And<ir::U64>(old, SingleOperand(ir::Imm{~fill}))
                              .SetType(ir::ValueType::U64)
                    : __ Or<ir::U64>(old, SingleOperand(ir::Imm{fill}))
                              .SetType(ir::ValueType::U64);
        }
        return result;
    };
    auto low = apply(false);
    auto high = instr->GetNEONQ() ? apply(true) : ImmValue(0, ir::ValueType::U64);
    WriteVHalves(instr->GetRd(), low, high);
}

void A64Decoder::VisitNEON2RegMisc(const Instruction* instr) {
    if (instr->Mask(NEON2RegMiscMask) != NEON_CMEQ_zero || instr->GetNEONSize() != 0) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    auto rd = u8(instr->GetRd());
    auto rn = u8(instr->GetRn());
    auto low = ZeroByteMask(ReadVHalf(rn, false));
    auto high = instr->GetNEONQ() ? ZeroByteMask(ReadVHalf(rn, true))
                                  : ImmValue(0, ir::ValueType::U64);
    WriteVHalves(rd, low, high);
}

void A64Decoder::VisitNEON3Same(const Instruction* instr) {
    if (!instr->GetNEONQ()) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }

    auto rd = u8(instr->GetRd());
    auto rn = u8(instr->GetRn());
    auto rm = u8(instr->GetRm());
    if (instr->Mask(NEON3SameLogicalMask) == NEON_BIT) {
        for (bool high : {false, true}) {
            auto dest = ReadVHalf(rd, high);
            auto source = ReadVHalf(rn, high);
            auto mask = ReadVHalf(rm, high);
            auto kept = __ And<ir::U64>(
                    dest,
                    SingleOperand(__ Xor<ir::U64>(
                            mask, SingleOperand(ir::Imm{u64(-1)}))))
                                .SetType(ir::ValueType::U64);
            auto inserted = __ And<ir::U64>(source, SingleOperand(mask))
                                    .SetType(ir::ValueType::U64);
            auto result = __ Or<ir::U64>(kept, SingleOperand(inserted))
                                  .SetType(ir::ValueType::U64);
            auto offset = u32(offsetof(ThreadContext64, v) + rd * sizeof(u128)) +
                          (high ? 8 : 0);
            __ StoreUniform(ir::Uniform{offset, ir::ValueType::U64}, result);
        }
        return;
    }

    if (instr->GetNEONSize() != 0) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }

    if (instr->Mask(NEON3SameMask) == NEON_CMEQ) {
        auto low = __ Xor<ir::U64>(ReadVHalf(rn, false), SingleOperand(ReadVHalf(rm, false)))
                           .SetType(ir::ValueType::U64);
        auto high = __ Xor<ir::U64>(ReadVHalf(rn, true), SingleOperand(ReadVHalf(rm, true)))
                            .SetType(ir::ValueType::U64);
        WriteVHalves(rd, ZeroByteMask(low), ZeroByteMask(high));
        return;
    }

    if (instr->Mask(NEON3SameMask) == NEON_CMHS) {
        auto compare_half = [&](bool high) {
            auto left_half = ReadVHalf(rn, high);
            auto right_half = ReadVHalf(rm, high);
            auto packed = ImmValue(0, ir::ValueType::U64);
            for (u32 i = 0; i < 8; ++i) {
                auto left = __ And<ir::U64>(
                        __ LsrImm<ir::U64>(left_half, ir::Imm{i * 8}),
                        SingleOperand(ir::Imm{0xFF}))
                                    .SetType(ir::ValueType::U64);
                auto right = __ And<ir::U64>(
                        __ LsrImm<ir::U64>(right_half, ir::Imm{i * 8}),
                        SingleOperand(ir::Imm{0xFF}))
                                     .SetType(ir::ValueType::U64);
                auto diff = __ Sub<ir::U64>(left, SingleOperand(right))
                                    .SetType(ir::ValueType::U64);
                __ SaveFlags(diff, ir::Flags::NZCV);
                auto lane = __ Mul<ir::U64>(
                        Widen(CondPassed(cs)), SingleOperand(ir::Imm{0xFF}))
                                    .SetType(ir::ValueType::U64);
                if (i != 0) {
                    lane = __ LslImm<ir::U64>(lane, ir::Imm{i * 8})
                                   .SetType(ir::ValueType::U64);
                }
                packed = __ Or<ir::U64>(packed, SingleOperand(lane))
                                 .SetType(ir::ValueType::U64);
            }
            return packed;
        };
        WriteVHalves(rd, compare_half(false), compare_half(true));
        return;
    }

    const auto op = instr->Mask(NEON3SameMask);
    if (op == NEON_ADDP || op == NEON_UMINP || op == NEON_UMAXP) {
        auto pack_pairs = [&](u8 source_code) {
            auto source_low = ReadVHalf(source_code, false);
            auto source_high = ReadVHalf(source_code, true);
            auto packed = ImmValue(0, ir::ValueType::U64);
            for (u32 i = 0; i < 8; ++i) {
                auto source = i < 4 ? source_low : source_high;
                const u32 byte = (i % 4) * 2;
                auto left = __ LsrImm<ir::U64>(source, ir::Imm{byte * 8})
                                    .SetType(ir::ValueType::U64);
                auto right = __ LsrImm<ir::U64>(source, ir::Imm{(byte + 1) * 8})
                                     .SetType(ir::ValueType::U64);
                left = __ And<ir::U64>(left, SingleOperand(ir::Imm{0xFF}))
                               .SetType(ir::ValueType::U64);
                right = __ And<ir::U64>(right, SingleOperand(ir::Imm{0xFF}))
                                .SetType(ir::ValueType::U64);
                ir::Value lane;
                if (op == NEON_ADDP) {
                    lane = __ Add<ir::U64>(left, SingleOperand(right))
                                   .SetType(ir::ValueType::U64);
                } else {
                    auto diff = __ Sub<ir::U64>(left, SingleOperand(right))
                                        .SetType(ir::ValueType::U64);
                    __ SaveFlags(diff, ir::Flags::NZCV);
                    auto left_ge = CondPassed(cs);
                    lane = op == NEON_UMAXP
                            ? __ Select(left_ge, left, right).SetType(ir::ValueType::U64)
                            : __ Select(left_ge, right, left).SetType(ir::ValueType::U64);
                }
                lane = __ And<ir::U64>(lane, SingleOperand(ir::Imm{0xFF}))
                               .SetType(ir::ValueType::U64);
                if (i != 0) {
                    lane = __ LslImm<ir::U64>(lane, ir::Imm{i * 8})
                                   .SetType(ir::ValueType::U64);
                }
                packed = __ Or<ir::U64>(packed, SingleOperand(lane))
                                 .SetType(ir::ValueType::U64);
            }
            return packed;
        };
        WriteVHalves(rd, pack_pairs(rn), pack_pairs(rm));
        return;
    }

    Interrupt(InterruptReason::FALLBACK, current_pc);
}

void A64Decoder::VisitNEONExtract(const Instruction* instr) {
    if (instr->Mask(NEONExtractMask) != NEON_EXT || !instr->GetNEONQ()) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }

    auto rn_low = ReadVHalf(instr->GetRn(), false);
    auto rn_high = ReadVHalf(instr->GetRn(), true);
    auto rm_low = ReadVHalf(instr->GetRm(), false);
    auto rm_high = ReadVHalf(instr->GetRm(), true);
    const u32 index = instr->GetImmNEONExt();
    if (index == 0) {
        WriteVHalves(instr->GetRd(), rn_low, rn_high);
        return;
    }
    if (index == 8) {
        WriteVHalves(instr->GetRd(), rn_high, rm_low);
        return;
    }

    auto combine = [&](ir::Value low, ir::Value high, u32 bytes) {
        const u32 shift = bytes * 8;
        return __ Or<ir::U64>(
                __ LsrImm<ir::U64>(low, ir::Imm{shift}),
                SingleOperand(__ LslImm<ir::U64>(high, ir::Imm{64 - shift})))
                .SetType(ir::ValueType::U64);
    };
    if (index < 8) {
        WriteVHalves(instr->GetRd(),
                     combine(rn_low, rn_high, index),
                     combine(rn_high, rm_low, index));
    } else {
        const u32 tail = index - 8;
        WriteVHalves(instr->GetRd(),
                     combine(rn_high, rm_low, tail),
                     combine(rm_low, rm_high, tail));
    }
}

void A64Decoder::VisitNEONShiftImmediate(const Instruction* instr) {
    const int highest_bit = vixl::HighestSetBitPosition(instr->GetImmNEONImmh());
    const int right_shift = (16 << highest_bit) - instr->GetImmNEONImmhImmb();
    if (instr->Mask(NEONShiftImmediateMask) != NEON_SHRN || instr->GetNEONQ() ||
        highest_bit != 0 || right_shift != 4) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }

    auto low = ReadVHalf(instr->GetRn(), false);
    auto high = ReadVHalf(instr->GetRn(), true);
    auto packed = ImmValue(0, ir::ValueType::U64);
    for (u32 i = 0; i < 8; ++i) {
        auto source = i < 4 ? low : high;
        auto lane = __ LsrImm<ir::U64>(source, ir::Imm{u32((i % 4) * 16 + 4)})
                            .SetType(ir::ValueType::U64);
        lane = __ And<ir::U64>(lane, SingleOperand(ir::Imm{0xFF}))
                       .SetType(ir::ValueType::U64);
        if (i != 0) {
            lane = __ LslImm<ir::U64>(lane, ir::Imm{i * 8}).SetType(ir::ValueType::U64);
        }
        packed = __ Or<ir::U64>(packed, SingleOperand(lane)).SetType(ir::ValueType::U64);
    }
    WriteVHalves(instr->GetRd(), packed, ImmValue(0, ir::ValueType::U64));
}

void A64Decoder::VisitNEONLoadStoreMultiStruct(const Instruction* instr) {
    if (instr->Mask(NEONLoadStoreMultiStructMask) != NEON_LD1_1v || !instr->GetNEONQ()) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    auto address = ReadXRegister(instr->GetRn(), Reg31IsStackPointer);
    WriteVRegister(instr->GetRt(), ReadMemory(ir::Lambda{address}, ir::ValueType::V128));
}

void A64Decoder::VisitNEONLoadStoreMultiStructPostIndex(const Instruction* instr) {
    if (instr->Mask(NEONLoadStoreMultiStructPostIndexMask) != NEON_LD1_1v_post ||
        !instr->GetNEONQ()) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    auto rn = u8(instr->GetRn());
    auto address = ReadXRegister(rn, Reg31IsStackPointer);
    WriteVRegister(instr->GetRt(), ReadMemory(ir::Lambda{address}, ir::ValueType::V128));
    auto offset = instr->GetRm() == 31
            ? ir::DataClass{ir::Imm{16}}
            : ir::DataClass{ReadXRegister(instr->GetRm())};
    WriteXRegister(rn,
                   __ Add<ir::U64>(address, SingleOperand(offset))
                           .SetType(ir::ValueType::U64),
                   Reg31IsStackPointer);
}

void A64Decoder::VisitFPIntegerConvert(const Instruction* instr) {
    switch (instr->Mask(FPIntegerConvertMask)) {
        case FMOV_xd:
            WriteXRegister(instr->GetRd(), ReadVHalf(instr->GetRn(), false));
            return;
        case FMOV_dx:
            WriteVHalves(instr->GetRd(),
                         ReadXRegister(instr->GetRn()),
                         ImmValue(0, ir::ValueType::U64));
            return;
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }
}

}  // namespace swift::arm64
