#include "arm64_frontend_internal.h"
#include "fp_helpers.h"
#include <cstring>

namespace swift::arm64 {

#define __ assembler->

static u32 VRegOff(u8 code) {
    return u32(offsetof(ThreadContext64, v) + code * sizeof(u128));
}

void A64Decoder::VisitNEONCopy(const Instruction* instr) {
    const u32 imm5 = instr->GetImmNEON5();
    if ((imm5 & 15) == 0) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const u32 size = vixl::CountTrailingZeros(imm5);
    const u32 bits = 8u << size;
    const u32 index = imm5 >> (size + 1);
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    const bool is_q = instr->GetNEONQ() != 0;
    const u8 rn = instr->GetRn();
    const u8 rd = instr->GetRd();
    auto extract = [&](u32 lane) {
        auto value = ReadVHalf(rn, lane * bits >= 64);
        const u32 shift = (lane * bits) % 64;
        if (shift) {
            value = __ LsrImm<ir::U64>(value, ir::Imm{shift}).SetType(ir::ValueType::U64);
        }
        return __ And<ir::U64>(value, SingleOperand(ir::Imm{mask}))
                .SetType(ir::ValueType::U64);
    };

    const bool ins_element = instr->Mask(NEONCopyInsElementMask) == NEON_INS_ELEMENT;
    const bool ins_general = instr->Mask(NEONCopyInsGeneralMask) == NEON_INS_GENERAL;
    if (ins_element || ins_general) {
        const u32 source_index = (instr->GetInstructionBits() >> 11 & 15) >> size;
        auto source = ins_element ? extract(source_index) : ReadXRegister(rn);
        source = __ And<ir::U64>(source, SingleOperand(ir::Imm{mask}))
                         .SetType(ir::ValueType::U64);
        const u32 shift = (index * bits) % 64;
        if (shift) {
            source = __ LslImm<ir::U64>(source, ir::Imm{shift}).SetType(ir::ValueType::U64);
        }
        const bool high = index * bits >= 64;
        auto old = ReadVHalf(rd, high);
        auto kept = __ And<ir::U64>(old, SingleOperand(ir::Imm{~(mask << shift)}))
                            .SetType(ir::ValueType::U64);
        auto value = __ Or<ir::U64>(kept, SingleOperand(source)).SetType(ir::ValueType::U64);
        __ StoreUniform(ir::Uniform{VRegOff(rd) + (high ? 8u : 0u), ir::ValueType::U64}, value);
        return;
    }

    const auto op = instr->Mask(NEONCopyDupElementMask);
    if (op == NEON_DUP_ELEMENT || op == NEON_DUP_GENERAL) {
        if (!is_q && bits == 64) {
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return;
        }
        auto half = op == NEON_DUP_ELEMENT ? extract(index) : ReadXRegister(rn);
        half = __ And<ir::U64>(half, SingleOperand(ir::Imm{mask}))
                       .SetType(ir::ValueType::U64);
        for (u32 shift = bits; shift < 64; shift *= 2) {
            auto shifted = __ LslImm<ir::U64>(half, ir::Imm{shift}).SetType(ir::ValueType::U64);
            half = __ Or<ir::U64>(half, SingleOperand(shifted)).SetType(ir::ValueType::U64);
        }
        WriteVHalves(rd, half, is_q ? half : ImmValue(0, ir::ValueType::U64));
        return;
    }
    if (op == NEON_UMOV || op == NEON_SMOV) {
        if ((op == NEON_UMOV && (is_q ? bits != 64 : bits == 64)) ||
            (op == NEON_SMOV && bits >= (is_q ? 64u : 32u))) {
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return;
        }
        auto value = extract(index);
        if (op == NEON_SMOV) value = SignExtendValue(value, bits);
        if (is_q) WriteXRegister(rd, value);
        else WriteWRegister(rd, value);
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
            if (cmode == 14 && op_bit == 0) {
                lane_bits = 8;
                lane = imm8;
            } else if (cmode == 14) {
                lane_bits = 64;
                for (u32 i = 0; i < 8; ++i) {
                    if (imm8 & (u64{1} << i)) lane |= u64{0xFF} << (i * 8);
                }
            } else if (op_bit == 0) {
                lane_bits = 32;
                lane = ((imm8 & 0x80) << 24) | (((imm8 ^ 0x40) & 0x40) << 24) |
                       ((imm8 & 0x40) ? 0x3E000000 : 0) | ((imm8 & 0x3F) << 19);
            } else if (instr->GetNEONQ()) {
                lane_bits = 64;
                lane = ((imm8 & 0x80) << 56) | (((imm8 ^ 0x40) & 0x40) << 56) |
                       ((imm8 & 0x40) ? u64{0x3FC0000000000000} : 0) |
                       ((imm8 & 0x3F) << 48);
            } else {
                Interrupt(InterruptReason::ILL_CODE, current_pc);
                return;
            }
            break;
        default:
            PANIC();
    }

    u64 fill;
    switch (lane_bits) {
        case 8: fill = lane * u64(0x0101010101010101); break;
        case 16: fill = lane * u64(0x0001000100010001); break;
        case 32: fill = lane | (lane << 32); break;
        case 64: fill = lane; break;
        default: PANIC();
    }

    const bool is_logic = cmode < 12 && (cmode & 1) != 0;
    const bool invert_fill = cmode < 14 && op_bit;
    auto apply = [&](bool high) {
        ir::Value result;
        if (!is_logic) {
            result = ImmValue(invert_fill ? ~fill : fill, ir::ValueType::U64);
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
    if (TryNEONMixedSaturating(instr,false)) return;
    if (TryNEONPairwiseWiden(instr)) return;
    if (TryFRINTRange(instr)) return;
    if (TryNEONUnsignedEstimate(instr)) return;
    if (TryNEONFPEstimate(instr, false)) return;
    if (TryNEONFPPrecision(instr)) return;
    if (TryNEONFPInteger(instr, false)) return;
    if (TryNEONFPCompare(instr, false, true)) return;
    if (TryNEONVectorFPUnary(instr)) return;
    if (TryNEONBitMisc(instr)) return;
    if (TryNEONInteger(instr, true)) return;
    if (instr->Mask(NEON2RegMiscMask) == NEON_RBIT_NOT && instr->GetNEONSize() == 0) {
        auto invert = [&](bool high) {
            return __ Xor<ir::U64>(ReadVHalf(instr->GetRn(), high),
                                   SingleOperand(ir::Imm{~u64{0}}))
                    .SetType(ir::ValueType::U64);
        };
        auto low = invert(false);
        auto high = instr->GetNEONQ() ? invert(true) : ImmValue(0, ir::ValueType::U64);
        WriteVHalves(instr->GetRd(), low, high);
        return;
    }
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
    if (TryNEONHalving(instr)) return;
    if (TryNEONAbsoluteNarrow(instr)) return;
    if (TryNEONPolynomial(instr,false)) return;
    if (TryNEONFPReduction(instr, false, false)) return;
    if (TryNEONFPCompare(instr, false, false)) return;
    if (TryNEONExtendedFP(instr, false)) return;
    if (TryNEONVectorFMA(instr, false)) return;
    if (TryNEONVectorFPBinary(instr)) return;
    if (TryNEONSaturatingMultiplyHigh(instr, false, false, false)) return;
    if (TryNEONSaturatingArithmetic(instr)) return;
    if (TryNEONVariableShift(instr)) return;
    if (TryNEONInteger(instr, false)) return;
    const auto logical_op = instr->Mask(NEON3SameLogicalMask);
    switch (logical_op) {
        case NEON_AND:
        case NEON_ORR:
        case NEON_ORN:
        case NEON_EOR:
        case NEON_BIC:
        case NEON_BIF:
        case NEON_BIT:
        case NEON_BSL: {
            auto apply = [&](bool high) {
                auto left = ReadVHalf(instr->GetRn(), high);
                auto right = ReadVHalf(instr->GetRm(), high);
                auto invert = [&](ir::Value value) {
                    return __ Xor<ir::U64>(value, SingleOperand(ir::Imm{~u64{0}}))
                            .SetType(ir::ValueType::U64);
                };
                if (logical_op == NEON_AND || logical_op == NEON_BIC) {
                    return __ And<ir::U64>(left, SingleOperand(
                            logical_op == NEON_BIC ? invert(right) : right))
                            .SetType(ir::ValueType::U64);
                }
                if (logical_op == NEON_ORR || logical_op == NEON_ORN) {
                    return __ Or<ir::U64>(left, SingleOperand(
                            logical_op == NEON_ORN ? invert(right) : right))
                            .SetType(ir::ValueType::U64);
                }
                if (logical_op == NEON_EOR) {
                    return __ Xor<ir::U64>(left, SingleOperand(right))
                            .SetType(ir::ValueType::U64);
                }
                auto old = ReadVHalf(instr->GetRd(), high);
                auto mask = logical_op == NEON_BSL ? old : right;
                if (logical_op == NEON_BIF) mask = invert(mask);
                auto alternate = logical_op == NEON_BSL ? right : old;
                auto selected = __ And<ir::U64>(left, SingleOperand(mask))
                                        .SetType(ir::ValueType::U64);
                auto kept = __ And<ir::U64>(alternate, SingleOperand(invert(mask)))
                                    .SetType(ir::ValueType::U64);
                return __ Or<ir::U64>(selected, SingleOperand(kept))
                        .SetType(ir::ValueType::U64);
            };
            // Capture both source halves before publishing an aliased destination.
            auto low = apply(false);
            auto high = instr->GetNEONQ() ? apply(true) : ImmValue(0, ir::ValueType::U64);
            WriteVHalves(instr->GetRd(), low, high);
            return;
        }
        default: break;
    }
    const auto arithmetic_op = instr->Mask(NEON3SameMask);
    if (arithmetic_op == NEON_ADD || arithmetic_op == NEON_SUB || arithmetic_op == NEON_MUL) {
        const u32 bits = 8u << instr->GetNEONSize();
        if (bits == 64 && (!instr->GetNEONQ() || arithmetic_op == NEON_MUL)) {
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return;
        }
        const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
        auto apply = [&](bool high) {
            auto left_half = ReadVHalf(instr->GetRn(), high);
            auto right_half = ReadVHalf(instr->GetRm(), high);
            auto packed = ImmValue(0, ir::ValueType::U64);
            for (u32 shift = 0; shift < 64; shift += bits) {
                auto left = __ LsrImm<ir::U64>(left_half, ir::Imm{shift}).SetType(ir::ValueType::U64);
                auto right = __ LsrImm<ir::U64>(right_half, ir::Imm{shift}).SetType(ir::ValueType::U64);
                left = __ And<ir::U64>(left, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
                right = __ And<ir::U64>(right, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
                auto lane = arithmetic_op == NEON_ADD ? __ Add<ir::U64>(left, SingleOperand(right))
                          : arithmetic_op == NEON_SUB ? __ Sub<ir::U64>(left, SingleOperand(right))
                                                     : __ Mul<ir::U64>(left, SingleOperand(right));
                lane.SetType(ir::ValueType::U64);
                lane = __ And<ir::U64>(lane, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
                if (shift) lane = __ LslImm<ir::U64>(lane, ir::Imm{shift}).SetType(ir::ValueType::U64);
                packed = __ Or<ir::U64>(packed, SingleOperand(lane)).SetType(ir::ValueType::U64);
            }
            return packed;
        };
        auto low = apply(false);
        auto high = instr->GetNEONQ() ? apply(true) : ImmValue(0, ir::ValueType::U64);
        WriteVHalves(instr->GetRd(), low, high);
        return;
    }
    if (!instr->GetNEONQ()) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }

    auto rd = u8(instr->GetRd());
    auto rn = u8(instr->GetRn());
    auto rm = u8(instr->GetRm());
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
                // Byte operands cannot overflow signed 64-bit subtraction.
                // Derive the comparison without publishing guest NZCV.
                auto negative = __ LsrImm<ir::U64>(diff, ir::Imm{63u})
                                        .SetType(ir::ValueType::U64);
                auto ge = __ Xor<ir::U64>(negative, SingleOperand(ir::Imm{u64{1}}))
                                  .SetType(ir::ValueType::U64);
                auto lane = __ Mul<ir::U64>(
                        ge, SingleOperand(ir::Imm{0xFF}))
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
                    auto negative = __ LsrImm<ir::U64>(diff, ir::Imm{63u})
                                            .SetType(ir::ValueType::U64);
                    auto left_ge = __ TestZero(negative);
                    left_ge.SetType(ir::ValueType::U8);
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
    if (instr->Mask(NEONExtractMask) != NEON_EXT) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }

    auto rn_low = ReadVHalf(instr->GetRn(), false);
    const u32 index = instr->GetImmNEONExt();
    if (!instr->GetNEONQ()) {
        if (index >= 8) {
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return;
        }
        auto low = rn_low;
        if (index != 0) {
            const u32 shift = index * 8;
            auto rm_low = ReadVHalf(instr->GetRm(), false);
            low = __ Or<ir::U64>(__ LsrImm<ir::U64>(rn_low, ir::Imm{shift}),
                                 SingleOperand(__ LslImm<ir::U64>(rm_low, ir::Imm{64 - shift})))
                          .SetType(ir::ValueType::U64);
        }
        WriteVHalves(instr->GetRd(), low, ImmValue(0, ir::ValueType::U64));
        return;
    }
    auto rn_high = ReadVHalf(instr->GetRn(), true);
    auto rm_low = ReadVHalf(instr->GetRm(), false);
    auto rm_high = ReadVHalf(instr->GetRm(), true);
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
    if (TryNEONFPFixedSIMD(instr)) return;
    if (TryNEONVectorSaturatingShift(instr)) return;
    if (TryNEONShiftImmediate(instr)) return;
    const int highest_bit = vixl::HighestSetBitPosition(instr->GetImmNEONImmh());
    const int right_shift = (16 << highest_bit) - instr->GetImmNEONImmhImmb();
    if (instr->Mask(NEONShiftImmediateMask) != NEON_SHRN) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    if (highest_bit < 0 || highest_bit > 2) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }

    auto low = ReadVHalf(instr->GetRn(), false);
    auto high = ReadVHalf(instr->GetRn(), true);
    const u32 lane_bits = 8u << highest_bit;
    const u32 source_bits = lane_bits * 2;
    const u32 source_lanes_per_half = 64 / source_bits;
    auto packed = ImmValue(0, ir::ValueType::U64);
    for (u32 i = 0; i < 64 / lane_bits; ++i) {
        auto source = i < source_lanes_per_half ? low : high;
        auto lane = __ LsrImm<ir::U64>(source, ir::Imm{
                u32((i % source_lanes_per_half) * source_bits + right_shift)})
                            .SetType(ir::ValueType::U64);
        lane = __ And<ir::U64>(lane, SingleOperand(ir::Imm{(u64{1} << lane_bits) - 1}))
                       .SetType(ir::ValueType::U64);
        if (i != 0) {
            lane = __ LslImm<ir::U64>(lane, ir::Imm{i * lane_bits}).SetType(ir::ValueType::U64);
        }
        packed = __ Or<ir::U64>(packed, SingleOperand(lane)).SetType(ir::ValueType::U64);
    }
    if (instr->GetNEONQ()) {
        WriteVHalves(instr->GetRd(), ReadVHalf(instr->GetRd(), false), packed);
    } else {
        WriteVHalves(instr->GetRd(), packed, ImmValue(0, ir::ValueType::U64));
    }
}

void A64Decoder::VisitNEONLoadStoreMultiStruct(const Instruction* instr) {
    NEONMultiMemory(instr, false);
}

void A64Decoder::VisitNEONLoadStoreMultiStructPostIndex(const Instruction* instr) {
    NEONMultiMemory(instr, true);
}

void A64Decoder::VisitFPIntegerConvert(const Instruction* instr) {
    switch (instr->Mask(FPIntegerConvertMask)) {
        case FMOV_wh:
        case FMOV_xh: {
            auto value = __ And<ir::U64>(ReadVHalf(instr->GetRn(), false),
                                        SingleOperand(ir::Imm{u64{0xFFFF}}))
                                 .SetType(ir::ValueType::U64);
            WriteXRegister(instr->GetRd(), value);
            return;
        }
        case FMOV_ws:
            WriteWRegister(instr->GetRd(), ReadVHalf(instr->GetRn(), false));
            return;
        case FMOV_hw:
        case FMOV_hx: {
            auto value = __ And<ir::U64>(ReadXRegister(instr->GetRn()),
                                        SingleOperand(ir::Imm{u64{0xFFFF}}))
                                 .SetType(ir::ValueType::U64);
            WriteVHalves(instr->GetRd(), value, ImmValue(0, ir::ValueType::U64));
            return;
        }
        case FMOV_sw:
            WriteVHalves(instr->GetRd(), Widen(ReadWRegister(instr->GetRn())),
                         ImmValue(0, ir::ValueType::U64));
            return;
        case FMOV_xd:
            WriteXRegister(instr->GetRd(), ReadVHalf(instr->GetRn(), false));
            return;
        case FMOV_x_d1:
            WriteXRegister(instr->GetRd(), ReadVHalf(instr->GetRn(), true));
            return;
        case FMOV_d1_x: {
            auto low = ReadVHalf(instr->GetRd(), false);
            auto high = ReadXRegister(instr->GetRn());
            WriteVHalves(instr->GetRd(), low, high);
            return;
        }
        case FMOV_dx:
            WriteVHalves(instr->GetRd(),
                         ReadXRegister(instr->GetRn()),
                         ImmValue(0, ir::ValueType::U64));
            return;
        default:
            ScalarFPHelper(instr);
            return;
    }
}

void A64Decoder::VisitFPImmediate(const Instruction* instr) {
    u64 bits;
    switch (instr->Mask(FPImmediateMask)) {
        case FMOV_h_imm: {
            const u64 imm8 = (instr->GetInstructionBits() >> 13) & 0xFF;
            bits = ((imm8 & 0x80) << 8) | (((imm8 ^ 0x40) & 0x40) << 8) |
                   ((imm8 & 0x40) ? 0x3000 : 0) | ((imm8 & 0x3F) << 6);
            break;
        }
        case FMOV_s_imm: {
            const float value = instr->GetImmFP32();
            u32 raw;
            std::memcpy(&raw, &value, sizeof(raw));
            bits = raw;
            break;
        }
        case FMOV_d_imm: {
            const double value = instr->GetImmFP64();
            std::memcpy(&bits, &value, sizeof(bits));
            break;
        }
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }
    WriteVHalves(instr->GetRd(), ImmValue(bits, ir::ValueType::U64),
                 ImmValue(0, ir::ValueType::U64));
}

void A64Decoder::VisitFPDataProcessing1Source(const Instruction* instr) {
    if (TryFRINTRange(instr)) return;
    const auto op = instr->Mask(FPDataProcessing1SourceMask);
    u32 bits;
    switch (op) {
        case FMOV_h: case FABS_h: case FNEG_h: bits = 16; break;
        case FMOV_s: case FABS_s: case FNEG_s: bits = 32; break;
        case FMOV_d: case FABS_d: case FNEG_d: bits = 64; break;
        default:
            ScalarFPHelper(instr);
            return;
    }
    const u64 lane_mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    const u64 sign = u64{1} << (bits - 1);
    const bool is_abs = op == FABS_h || op == FABS_s || op == FABS_d;
    const bool is_neg = op == FNEG_h || op == FNEG_s || op == FNEG_d;
    auto value = __ And<ir::U64>(ReadVHalf(instr->GetRn(), false),
                                SingleOperand(ir::Imm{is_abs ? lane_mask & ~sign : lane_mask}))
                         .SetType(ir::ValueType::U64);
    if (is_neg) {
        value = __ Xor<ir::U64>(value, SingleOperand(ir::Imm{sign}))
                        .SetType(ir::ValueType::U64);
    }
    if (is_abs || is_neg) {
        // FEAT_AFP alternate handling leaves NaN signs unchanged. These
        // bit operations preserve signaling payloads and do not raise FPSR
        // exceptions or apply DN/FZ/rounding, just like the vector forms.
        auto original = __ And<ir::U64>(ReadVHalf(instr->GetRn(), false),
                                       SingleOperand(ir::Imm{lane_mask})).SetType(ir::ValueType::U64);
        const u64 exponent = bits == 16 ? 0x7C00 : bits == 32 ? 0x7F800000 : 0x7FF0000000000000;
        const u64 fraction = lane_mask & ~(sign | exponent);
        auto encoded_exponent = __ And<ir::U64>(original, SingleOperand(ir::Imm{exponent}))
                                    .SetType(ir::ValueType::U64);
        auto exponent_diff = __ Xor<ir::U64>(encoded_exponent, SingleOperand(ir::Imm{exponent}))
                                 .SetType(ir::ValueType::U64);
        auto all_exponent = __ TestZero(exponent_diff).SetType(ir::ValueType::U8);
        auto payload = __ And<ir::U64>(original, SingleOperand(ir::Imm{fraction})).SetType(ir::ValueType::U64);
        auto nonzero_payload = __ TestNotZero(payload).SetType(ir::ValueType::U8);
        auto nan = __ And<ir::U8>(all_exponent, SingleOperand(nonzero_payload)).SetType(ir::ValueType::U8);
        const ir::Uniform control{u32(offsetof(ThreadContext64, fpcr)), ir::ValueType::U32};
        auto fpcr = __ LoadUniform<ir::U32>(control);
        auto alternate = __ TestBit(fpcr, ir::Imm{1}).SetType(ir::ValueType::U8);
        auto preserve = __ And<ir::U8>(alternate, SingleOperand(nan)).SetType(ir::ValueType::U8);
        value = __ Select(preserve, original, value).SetType(ir::ValueType::U64);
    }
    WriteVHalves(instr->GetRd(), value, ImmValue(0, ir::ValueType::U64));
}

void A64Decoder::WriteNZCV(ir::Value source) {
    auto bit = [&](u32 position) {
        auto shifted = __ LsrImm<ir::U64>(source, ir::Imm{position}).SetType(ir::ValueType::U64);
        return __ And<ir::U64>(shifted, SingleOperand(ir::Imm{u64{1}})).SetType(ir::ValueType::U64);
    };
    auto n = __ LslImm<ir::U64>(bit(3), ir::Imm{63u}).SetType(ir::ValueType::U64);
    n = __ Sub<ir::U64>(ImmValue(0, ir::ValueType::U64), SingleOperand(n)).SetType(ir::ValueType::U64);
    auto z = __ Xor<ir::U64>(bit(2), SingleOperand(ir::Imm{u64{1}})).SetType(ir::ValueType::U64);
    z = __ Sub<ir::U64>(z, SingleOperand(ir::Imm{u64{0}})).SetType(ir::ValueType::U64);
    __ SaveFlags(n, ir::Flags::Negate);
    __ SaveFlags(z, ir::Flags::Zero);
    __ SetCarry(bit(1));
    __ SetOverflow(bit(0));
}

void A64Decoder::ScalarFPHelper(const Instruction* instr, bool comparison) {
    if (!IsSupportedScalarFP(instr->GetInstructionBits())) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    auto context = __ GetUniformAddress(ir::Imm{u32{0}}).SetType(ir::ValueType::U64);
    auto result = __ CallHostFPFree(&ExecuteScalarFP, context,
                                  ImmValue(instr->GetInstructionBits(), ir::ValueType::U64),
                                  ImmValue(current_pc, ir::ValueType::U64))
                          .SetType(ir::ValueType::U64);
    if (comparison) WriteNZCV(result);
}

void A64Decoder::VisitFPDataProcessing2Source(const Instruction* instr) { ScalarFPHelper(instr); }
void A64Decoder::VisitFPFixedPointConvert(const Instruction* instr) { ScalarFPHelper(instr); }
void A64Decoder::VisitFPDataProcessing3Source(const Instruction* instr) { ScalarFPHelper(instr); }
void A64Decoder::VisitFPCompare(const Instruction* instr) { ScalarFPHelper(instr, true); }

void A64Decoder::VisitFPConditionalCompare(const Instruction* instr) {
    auto compare = __ Goto(CondPassed(static_cast<Condition>(instr->GetCondition())));
    WriteNZCV(ImmValue(instr->GetNzcv(), ir::ValueType::U64));
    auto done = __ Goto(ir::BOOL{ImmValue(1, ir::ValueType::U8)});
    __ BindLabel(compare);
    ScalarFPHelper(instr, true);
    __ BindLabel(done);
}

void A64Decoder::VisitFPConditionalSelect(const Instruction* instr) {
    const u32 type = instr->GetFPType();
    if (type != 0 && type != 1 && type != 3) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const u32 bits = type == 0 ? 32 : type == 1 ? 64 : 16;
    auto condition = CondPassed(static_cast<Condition>(instr->GetCondition()));
    auto value = __ Select(condition, ReadVHalf(instr->GetRn(), false),
                           ReadVHalf(instr->GetRm(), false)).SetType(ir::ValueType::U64);
    if (bits < 64) {
        value = __ And<ir::U64>(value, SingleOperand(ir::Imm{(u64{1} << bits) - 1}))
                        .SetType(ir::ValueType::U64);
    }
    WriteVHalves(instr->GetRd(), value, ImmValue(0, ir::ValueType::U64));
}

void A64Decoder::VisitNEONScalarCopy(const Instruction* instr) {
    const u32 imm5 = instr->GetImmNEON5();
    if ((imm5 & 15) == 0) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const u32 bits = 8u << vixl::CountTrailingZeros(imm5);
    const u32 index = imm5 >> (vixl::CountTrailingZeros(imm5) + 1);
    auto value = ReadVHalf(instr->GetRn(), index * bits >= 64);
    const u32 shift = (index * bits) % 64;
    if (shift) {
        value = __ LsrImm<ir::U64>(value, ir::Imm{shift}).SetType(ir::ValueType::U64);
    }
    if (bits < 64) {
        value = __ And<ir::U64>(value, SingleOperand(ir::Imm{(u64{1} << bits) - 1}))
                        .SetType(ir::ValueType::U64);
    }
    WriteVHalves(instr->GetRd(), value, ImmValue(0, ir::ValueType::U64));
}

}  // namespace swift::arm64
