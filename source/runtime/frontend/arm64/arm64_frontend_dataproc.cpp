#include "arm64_frontend_internal.h"

namespace swift::arm64 {

#define __ assembler->

void A64Decoder::AddSubHelper(const Instruction* instr, const ir::DataClass& op2) {
    bool is64 = instr->GetSixtyFourBits();
    auto operation = instr->Mask(AddSubOpMask);
    bool is_sub = (operation == SUB) || (operation == SUBS);
    bool set_flags = (operation == ADDS) || (operation == SUBS);

    auto rn = ReadRegister(instr->GetRn(), GPRType(is64), instr->GetRnMode());

    ir::Value result;
    if (is64) {
        result = is_sub ? __ Sub<ir::U64>(rn, SingleOperand(op2)).SetType(ir::ValueType::U64)
                        : __ Add<ir::U64>(rn, SingleOperand(op2)).SetType(ir::ValueType::U64);
    } else {
        result = is_sub ? __ Sub<ir::U32>(rn, SingleOperand(op2)).SetType(ir::ValueType::U32)
                        : __ Add<ir::U32>(rn, SingleOperand(op2)).SetType(ir::ValueType::U32);
    }

    if (set_flags) {
        // AArch64 ADDS/SUBS semantics: N/Z from result, C = carry-out (NOT
        // borrow for SUBS), V = signed overflow. Matches the host ARM64 flags
        // produced by the backend for Add/Sub with a SaveFlags pseudo.
        __ SaveFlags(result, ir::Flags::NZCV);
    }

    if (is64) {
        WriteXRegister(instr->GetRd(), result, instr->GetRdMode());
    } else {
        WriteWRegister(instr->GetRd(), result, instr->GetRdMode());
    }
}

void A64Decoder::VisitAddSubImmediate(const Instruction* instr) {
    u32 imm = instr->GetImmAddSub();
    if (instr->GetShiftAddSub() == 1) {
        imm <<= 12;
    }
    AddSubHelper(instr, ir::DataClass{ir::Imm{imm}});
}

void A64Decoder::VisitAddSubShifted(const Instruction* instr) {
    bool is64 = instr->GetSixtyFourBits();
    auto rm = ReadRegister(instr->GetRm(), GPRType(is64));
    auto op2 = ShiftOperand(is64,
                            rm,
                            static_cast<Shift>(instr->GetShiftDP()),
                            u32(instr->GetImmDPShift()));
    AddSubHelper(instr, ir::DataClass{op2});
}

void A64Decoder::VisitAddSubExtended(const Instruction* instr) {
    auto rm = ReadXRegister(instr->GetRm());
    auto op2 = ExtendOperand(rm,
                             static_cast<Extend>(instr->GetExtendMode()),
                             u32(instr->GetImmExtendShift()));
    AddSubHelper(instr, ir::DataClass{op2});
}

void A64Decoder::VisitAddSubWithCarry(const Instruction* instr) {
    bool is64 = instr->GetSixtyFourBits();
    auto op = instr->Mask(AddSubWithCarryMask);
    bool is_sub = (op == SBC_w) || (op == SBC_x) || (op == SBCS_w) || (op == SBCS_x);
    bool set_flags = (op == ADCS_w) || (op == ADCS_x) || (op == SBCS_w) || (op == SBCS_x);

    auto rn = ReadRegister(instr->GetRn(), GPRType(is64));
    auto rm = ReadRegister(instr->GetRm(), GPRType(is64));

    // SBC = a + ~b + C, so both ADC and SBC map onto Adc with the AArch64
    // carry flag (C = NOT borrow) consumed directly.
    ir::Value operand2 = rm;
    if (is_sub) {
        operand2 = is64
                ? __ Xor<ir::U64>(rm, SingleOperand(ir::Imm{u64(-1)}))
                          .SetType(ir::ValueType::U64)
                : __ Xor<ir::U32>(rm, SingleOperand(ir::Imm{u32(-1)}))
                          .SetType(ir::ValueType::U32);
    }

    ir::Value result = is64 ? __ Adc<ir::U64>(rn, SingleOperand(operand2)).SetType(ir::ValueType::U64)
                            : __ Adc<ir::U32>(rn, SingleOperand(operand2)).SetType(ir::ValueType::U32);
    if (set_flags) {
        __ SaveFlags(result, ir::Flags::NZCV);
    }
    if (is64) {
        WriteXRegister(instr->GetRd(), result);
    } else {
        WriteWRegister(instr->GetRd(), result);
    }
}

void A64Decoder::VisitLogicalImmediate(const Instruction* instr) {
    bool is64 = instr->GetSixtyFourBits();
    u64 imm = instr->GetImmLogical();
    ir::DataClass op2 = is64 ? ir::DataClass{ir::Imm{imm}}
                             : ir::DataClass{ir::Imm{u32(imm)}};

    auto rn = ReadRegister(instr->GetRn(), GPRType(is64));

    auto type = GPRType(is64);
    ir::Value result{};
    bool set_flags = false;
    switch (instr->Mask(LogicalImmediateMask)) {
        case AND_w_imm:
        case AND_x_imm:
            result = is64 ? __ And<ir::U64>(rn, SingleOperand(op2)).SetType(type)
                          : __ And<ir::U32>(rn, SingleOperand(op2)).SetType(type);
            break;
        case ORR_w_imm:
        case ORR_x_imm:
            result = is64 ? __ Or<ir::U64>(rn, SingleOperand(op2)).SetType(type)
                          : __ Or<ir::U32>(rn, SingleOperand(op2)).SetType(type);
            break;
        case EOR_w_imm:
        case EOR_x_imm:
            result = is64 ? __ Xor<ir::U64>(rn, SingleOperand(op2)).SetType(type)
                          : __ Xor<ir::U32>(rn, SingleOperand(op2)).SetType(type);
            break;
        case ANDS_w_imm:
        case ANDS_x_imm:
            result = is64 ? __ And<ir::U64>(rn, SingleOperand(op2)).SetType(type)
                          : __ And<ir::U32>(rn, SingleOperand(op2)).SetType(type);
            set_flags = true;
            break;
        default:
            VIXL_UNREACHABLE();
    }

    if (set_flags) {
        // Host ANDS also yields C = V = 0, matching the AArch64 semantics.
        __ SaveFlags(result, ir::Flags::NZCV);
    }
    if (is64) {
        WriteXRegister(instr->GetRd(), result);
    } else {
        WriteWRegister(instr->GetRd(), result);
    }
}

void A64Decoder::VisitLogicalShifted(const Instruction* instr) {
    bool is64 = instr->GetSixtyFourBits();
    auto type = GPRType(is64);

    auto rn = ReadRegister(instr->GetRn(), type);
    auto op2 = ShiftOperand(is64,
                            ReadRegister(instr->GetRm(), type),
                            static_cast<Shift>(instr->GetShiftDP()),
                            u32(instr->GetImmDPShift()));

    ir::Value result{};
    bool set_flags = false;
    auto and_ = [&] {
        return is64 ? __ And<ir::U64>(rn, SingleOperand(op2)).SetType(type)
                    : __ And<ir::U32>(rn, SingleOperand(op2)).SetType(type);
    };
    auto bic_ = [&] {
        return is64 ? __ AndNot<ir::U64>(rn, SingleOperand(op2)).SetType(type)
                    : __ AndNot<ir::U32>(rn, SingleOperand(op2)).SetType(type);
    };
    auto or_ = [&](const ir::DataClass& o) {
        return is64 ? __ Or<ir::U64>(rn, SingleOperand(o)).SetType(type)
                    : __ Or<ir::U32>(rn, SingleOperand(o)).SetType(type);
    };
    auto xor_ = [&](const ir::DataClass& o) {
        return is64 ? __ Xor<ir::U64>(rn, SingleOperand(o)).SetType(type)
                    : __ Xor<ir::U32>(rn, SingleOperand(o)).SetType(type);
    };
    auto not_ = [&](ir::Value value) {
        return is64 ? __ Xor<ir::U64>(value, SingleOperand(ir::Imm{u64(-1)})).SetType(type)
                    : __ Xor<ir::U32>(value, SingleOperand(ir::Imm{u32(-1)})).SetType(type);
    };

    switch (instr->Mask(LogicalShiftedMask)) {
        case AND_w:
        case AND_x:
            result = and_();
            break;
        case BIC_w:
        case BIC_x:
            result = bic_();
            break;
        case ORR_w:
        case ORR_x:
            result = or_(ir::DataClass{op2});
            break;
        case ORN_w:
        case ORN_x:
            result = or_(ir::DataClass{not_(op2)});
            break;
        case EOR_w:
        case EOR_x:
            result = xor_(ir::DataClass{op2});
            break;
        case EON_w:
        case EON_x:
            result = xor_(ir::DataClass{not_(op2)});
            break;
        case ANDS_w:
        case ANDS_x:
            result = and_();
            set_flags = true;
            break;
        case BICS_w:
        case BICS_x:
            result = bic_();
            set_flags = true;
            break;
        default:
            VIXL_UNREACHABLE();
    }

    if (set_flags) {
        __ SaveFlags(result, ir::Flags::NZCV);
    }
    if (is64) {
        WriteXRegister(instr->GetRd(), result);
    } else {
        WriteWRegister(instr->GetRd(), result);
    }
}

void A64Decoder::VisitMoveWideImmediate(const Instruction* instr) {
    auto mov_op = static_cast<MoveWideImmediateOp>(instr->Mask(MoveWideImmediateMask));

    bool is64 = instr->GetSixtyFourBits() == 1;
    // Shift is limited for W operations.
    VIXL_ASSERT(is64 || (instr->GetShiftMoveWide() < 2));

    auto type = GPRType(is64);
    u32 shift = instr->GetShiftMoveWide() * 16;
    u64 shifted_imm16 = u64(instr->GetImmMoveWide()) << shift;

    ir::Value new_val{};
    switch (mov_op) {
        case MOVN_w:
        case MOVN_x: {
            u64 val = ~shifted_imm16;
            if (!is64) val &= kWRegMask;
            new_val = ImmValue(val, type);
            break;
        }
        case MOVK_w:
        case MOVK_x: {
            auto prev = ReadRegister(instr->GetRd(), type);
            auto imm16 = __ LoadImm<ir::U16>(ir::Imm{u16(instr->GetImmMoveWide())})
                                   .SetType(ir::ValueType::U16);
            new_val = is64 ? __ BitInsert<ir::U64>(prev, imm16, ir::Imm{u8(shift)}, ir::Imm{u8(16)})
                                   .SetType(type)
                           : __ BitInsert<ir::U32>(prev, imm16, ir::Imm{u8(shift)}, ir::Imm{u8(16)})
                                   .SetType(type);
            break;
        }
        case MOVZ_w:
        case MOVZ_x:
            new_val = ImmValue(shifted_imm16, type);
            break;
        default:
            VIXL_UNREACHABLE();
    }

    if (is64) {
        WriteXRegister(instr->GetRd(), new_val);
    } else {
        WriteWRegister(instr->GetRd(), new_val);
    }
}

void A64Decoder::VisitBitfield(const Instruction* instr) {
    bool is64 = instr->GetSixtyFourBits();
    u32 reg_size = is64 ? 64 : 32;
    u64 reg_mask = is64 ? kXRegMask : kWRegMask;
    int R = instr->GetImmR();
    int S = instr->GetImmS();
    int diff = S - R;
    u64 mask;
    if (diff >= 0) {
        mask = ~UINT64_C(0) >> (64 - (diff + 1));
        mask = (static_cast<unsigned>(diff) < (reg_size - 1)) ? mask : reg_mask;
    } else {
        mask = ~UINT64_C(0) >> (64 - (S + 1));
        mask = vixl::RotateRight(mask, R, reg_size);
        diff += reg_size;
    }

    // inzero indicates if the extracted bitfield is inserted into the
    // destination register value or in zero. If extend is true, the sign of
    // the extracted bitfield is extended.
    bool inzero = false;
    bool extend = false;
    switch (instr->Mask(BitfieldMask)) {
        case BFM_x:
        case BFM_w:
            break;
        case SBFM_x:
        case SBFM_w:
            inzero = true;
            extend = true;
            break;
        case UBFM_x:
        case UBFM_w:
            inzero = true;
            break;
        default:
            VIXL_UNIMPLEMENTED();
    }

    auto type = GPRType(is64);
    auto src = Widen(ReadRegister(instr->GetRn(), type));

    // Rotate the source bitfield into place.
    ir::Value rotated;
    if (R == 0) {
        rotated = src;
    } else if (is64) {
        rotated = __ RorImm<ir::U64>(src, ir::Imm{u32(R)}).SetType(ir::ValueType::U64);
    } else {
        // 32 bit rotate of a zero extended value.
        auto lo = __ LsrImm<ir::U64>(src, ir::Imm{u32(R)}).SetType(ir::ValueType::U64);
        auto hi = __ LslImm<ir::U64>(src, ir::Imm{u32(reg_size - R)}).SetType(ir::ValueType::U64);
        rotated = __ Or<ir::U64>(lo, SingleOperand(hi)).SetType(ir::ValueType::U64);
    }

    ir::Value result =
            __ And<ir::U64>(rotated, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);

    if (extend) {
        // Sign bits: replicate bit S of the source across all bits above it,
        // then keep only the bits above the extracted field.
        u64 topbits = (diff == 63) ? 0 : (~UINT64_C(0) << (diff + 1));
        ir::Value sign_extended;
        if (S == 63) {
            sign_extended = src;
        } else {
            auto shl = __ LslImm<ir::U64>(src, ir::Imm{u32(63 - S)}).SetType(ir::ValueType::U64);
            sign_extended =
                    __ AsrImm<ir::U64>(shl, ir::Imm{u32(63 - S)}).SetType(ir::ValueType::U64);
        }
        auto signbits = __ And<ir::U64>(sign_extended, SingleOperand(ir::Imm{topbits}))
                                .SetType(ir::ValueType::U64);
        result = __ Or<ir::U64>(result, SingleOperand(signbits)).SetType(ir::ValueType::U64);
    }

    if (!inzero) {
        auto dst = Widen(ReadRegister(instr->GetRd(), type));
        auto dst_masked = __ And<ir::U64>(dst, SingleOperand(ir::Imm{~mask}))
                                  .SetType(ir::ValueType::U64);
        result = __ Or<ir::U64>(result, SingleOperand(dst_masked)).SetType(ir::ValueType::U64);
    }

    if (is64) {
        WriteXRegister(instr->GetRd(), result);
    } else {
        WriteWRegister(instr->GetRd(), result);
    }
}

void A64Decoder::VisitExtract(const Instruction* instr) {
    bool is64 = instr->Mask(ExtractMask) == EXTR_x;
    auto type = GPRType(is64);
    u32 lsb = instr->GetImmS();

    auto rm = ReadRegister(instr->GetRm(), type);
    auto rn = ReadRegister(instr->GetRn(), type);

    ir::Value result;
    if (lsb == 0) {
        result = rm;
    } else if (instr->GetRn() == instr->GetRm()) {
        // ROR alias.
        result = is64 ? __ RorImm<ir::U64>(rm, ir::Imm{lsb}).SetType(type)
                      : __ RorImm<ir::U32>(rm, ir::Imm{lsb}).SetType(type);
    } else {
        auto lo = is64 ? __ LsrImm<ir::U64>(rm, ir::Imm{lsb}).SetType(type)
                       : __ LsrImm<ir::U32>(rm, ir::Imm{lsb}).SetType(type);
        auto hi = is64 ? __ LslImm<ir::U64>(rn, ir::Imm{64 - lsb}).SetType(type)
                       : __ LslImm<ir::U32>(rn, ir::Imm{32 - lsb}).SetType(type);
        result = is64 ? __ Or<ir::U64>(lo, SingleOperand(hi)).SetType(type)
                      : __ Or<ir::U32>(lo, SingleOperand(hi)).SetType(type);
    }

    if (is64) {
        WriteXRegister(instr->GetRd(), result);
    } else {
        WriteWRegister(instr->GetRd(), result);
    }
}

void A64Decoder::VisitDataProcessing2Source(const Instruction* instr) {
    bool is64 = instr->GetSixtyFourBits();
    auto type = GPRType(is64);
    auto stype = is64 ? ir::ValueType::S64 : ir::ValueType::S32;
    u32 width = is64 ? 64 : 32;

    auto rn = ReadRegister(instr->GetRn(), type);
    auto rm = ReadRegister(instr->GetRm(), type);

    ir::Value result{};
    switch (instr->Mask(DataProcessing2SourceMask)) {
        case UDIV_w:
        case UDIV_x:
            // NOTE: AArch64 returns 0 on division by zero; enforcing that is
            // left to the backend's Div implementation.
            result = is64 ? __ Div<ir::U64>(rn, SingleOperand(rm)).SetType(type)
                          : __ Div<ir::U32>(rn, SingleOperand(rm)).SetType(type);
            break;
        case SDIV_w:
        case SDIV_x: {
            auto l = rn.SetCastType(stype);
            auto r = rm.SetCastType(stype);
            result = is64 ? __ Div<ir::U64>(l, SingleOperand(r)).SetType(type)
                          : __ Div<ir::U32>(l, SingleOperand(r)).SetType(type);
            break;
        }
        case LSLV_w:
        case LSLV_x: {
            auto amount = __ And<ir::U32>(rm, SingleOperand(ir::Imm{width - 1}))
                                  .SetType(ir::ValueType::U32);
            result = is64 ? __ LslValue<ir::U64>(rn, amount).SetType(type)
                          : __ LslValue<ir::U32>(rn, amount).SetType(type);
            break;
        }
        case LSRV_w:
        case LSRV_x: {
            auto amount = __ And<ir::U32>(rm, SingleOperand(ir::Imm{width - 1}))
                                  .SetType(ir::ValueType::U32);
            result = is64 ? __ LsrValue<ir::U64>(rn, amount).SetType(type)
                          : __ LsrValue<ir::U32>(rn, amount).SetType(type);
            break;
        }
        case ASRV_w:
        case ASRV_x: {
            auto amount = __ And<ir::U32>(rm, SingleOperand(ir::Imm{width - 1}))
                                  .SetType(ir::ValueType::U32);
            result = is64 ? __ AsrValue<ir::U64>(rn.SetCastType(stype), amount).SetType(type)
                          : __ AsrValue<ir::U32>(rn.SetCastType(stype), amount).SetType(type);
            break;
        }
        case RORV_w:
        case RORV_x: {
            auto amount = __ And<ir::U32>(rm, SingleOperand(ir::Imm{width - 1}))
                                  .SetType(ir::ValueType::U32);
            result = is64 ? __ RorValue<ir::U64>(rn, amount).SetType(type)
                          : __ RorValue<ir::U32>(rn, amount).SetType(type);
            break;
        }
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }

    if (is64) {
        WriteXRegister(instr->GetRd(), result);
    } else {
        WriteWRegister(instr->GetRd(), result);
    }
}

void A64Decoder::VisitDataProcessing3Source(const Instruction* instr) {
    bool is64 = instr->GetSixtyFourBits();
    auto type = GPRType(is64);

    ir::Value result{};
    switch (instr->Mask(DataProcessing3SourceMask)) {
        case MADD_w:
        case MADD_x: {
            // MUL alias when Ra == XZR.
            auto rn = ReadRegister(instr->GetRn(), type);
            auto rm = ReadRegister(instr->GetRm(), type);
            auto ra = ReadRegister(instr->GetRa(), type);
            auto mul = is64 ? __ Mul<ir::U64>(rn, SingleOperand(rm)).SetType(type)
                            : __ Mul<ir::U32>(rn, SingleOperand(rm)).SetType(type);
            result = is64 ? __ Add<ir::U64>(ra, SingleOperand(mul)).SetType(type)
                          : __ Add<ir::U32>(ra, SingleOperand(mul)).SetType(type);
            if (is64) {
                WriteXRegister(instr->GetRd(), result);
            } else {
                WriteWRegister(instr->GetRd(), result);
            }
            return;
        }
        case MSUB_w:
        case MSUB_x: {
            auto rn = ReadRegister(instr->GetRn(), type);
            auto rm = ReadRegister(instr->GetRm(), type);
            auto ra = ReadRegister(instr->GetRa(), type);
            auto mul = is64 ? __ Mul<ir::U64>(rn, SingleOperand(rm)).SetType(type)
                            : __ Mul<ir::U32>(rn, SingleOperand(rm)).SetType(type);
            result = is64 ? __ Sub<ir::U64>(ra, SingleOperand(mul)).SetType(type)
                          : __ Sub<ir::U32>(ra, SingleOperand(mul)).SetType(type);
            if (is64) {
                WriteXRegister(instr->GetRd(), result);
            } else {
                WriteWRegister(instr->GetRd(), result);
            }
            return;
        }
        case UMADDL_x: {
            // Widening multiply: sources are W registers.
            auto rn = ReadWRegister(instr->GetRn());
            auto rm = ReadWRegister(instr->GetRm());
            auto ra = ReadXRegister(instr->GetRa());
            auto l = __ ZeroExtend64(rn).SetType(ir::ValueType::U64);
            auto r = __ ZeroExtend64(rm).SetType(ir::ValueType::U64);
            auto mul = __ Mul<ir::U64>(l, SingleOperand(r)).SetType(ir::ValueType::U64);
            WriteXRegister(instr->GetRd(),
                           __ Add<ir::U64>(ra, SingleOperand(mul)).SetType(ir::ValueType::U64));
            return;
        }
        case UMSUBL_x: {
            auto rn = ReadWRegister(instr->GetRn());
            auto rm = ReadWRegister(instr->GetRm());
            auto ra = ReadXRegister(instr->GetRa());
            auto l = __ ZeroExtend64(rn).SetType(ir::ValueType::U64);
            auto r = __ ZeroExtend64(rm).SetType(ir::ValueType::U64);
            auto mul = __ Mul<ir::U64>(l, SingleOperand(r)).SetType(ir::ValueType::U64);
            WriteXRegister(instr->GetRd(),
                           __ Sub<ir::U64>(ra, SingleOperand(mul)).SetType(ir::ValueType::U64));
            return;
        }
        case SMADDL_x: {
            auto rn = ReadWRegister(instr->GetRn());
            auto rm = ReadWRegister(instr->GetRm());
            auto ra = ReadXRegister(instr->GetRa());
            auto l = SignExtendValue(rn, 32);
            auto r = SignExtendValue(rm, 32);
            auto mul = __ Mul<ir::U64>(l, SingleOperand(r));
            WriteXRegister(instr->GetRd(),
                           __ Add<ir::U64>(ra, SingleOperand(mul)).SetType(ir::ValueType::U64));
            return;
        }
        case SMSUBL_x: {
            auto rn = ReadWRegister(instr->GetRn());
            auto rm = ReadWRegister(instr->GetRm());
            auto ra = ReadXRegister(instr->GetRa());
            auto l = SignExtendValue(rn, 32);
            auto r = SignExtendValue(rm, 32);
            auto mul = __ Mul<ir::U64>(l, SingleOperand(r));
            WriteXRegister(instr->GetRd(),
                           __ Sub<ir::U64>(ra, SingleOperand(mul)).SetType(ir::ValueType::U64));
            return;
        }
        case SMULH_x:
        case UMULH_x: {
            auto left = ReadXRegister(instr->GetRn());
            auto right = ReadXRegister(instr->GetRm());
            auto low32 = [&](ir::Value value) {
                return __ And<ir::U64>(value, SingleOperand(ir::Imm{0xFFFFFFFF}))
                        .SetType(ir::ValueType::U64);
            };
            auto left_high = __ LsrImm<ir::U64>(left, ir::Imm{32})
                                     .SetType(ir::ValueType::U64);
            auto right_high = __ LsrImm<ir::U64>(right, ir::Imm{32})
                                      .SetType(ir::ValueType::U64);
            auto product00 = __ Mul<ir::U64>(low32(left), SingleOperand(low32(right)))
                                     .SetType(ir::ValueType::U64);
            auto product01 = __ Mul<ir::U64>(low32(left), SingleOperand(right_high))
                                     .SetType(ir::ValueType::U64);
            auto product10 = __ Mul<ir::U64>(left_high, SingleOperand(low32(right)))
                                     .SetType(ir::ValueType::U64);
            auto product11 = __ Mul<ir::U64>(left_high, SingleOperand(right_high))
                                     .SetType(ir::ValueType::U64);
            auto middle = __ Add<ir::U64>(
                    __ LsrImm<ir::U64>(product00, ir::Imm{32}),
                    SingleOperand(low32(product01)))
                                  .SetType(ir::ValueType::U64);
            middle = __ Add<ir::U64>(middle, SingleOperand(low32(product10)))
                             .SetType(ir::ValueType::U64);
            auto high = __ Add<ir::U64>(
                    product11,
                    SingleOperand(__ LsrImm<ir::U64>(product01, ir::Imm{32})))
                                .SetType(ir::ValueType::U64);
            high = __ Add<ir::U64>(
                    high,
                    SingleOperand(__ LsrImm<ir::U64>(product10, ir::Imm{32})))
                           .SetType(ir::ValueType::U64);
            high = __ Add<ir::U64>(
                    high,
                    SingleOperand(__ LsrImm<ir::U64>(middle, ir::Imm{32})))
                           .SetType(ir::ValueType::U64);
            if (instr->Mask(DataProcessing3SourceMask) == SMULH_x) {
                auto left_sign = __ AsrImm<ir::U64>(left, ir::Imm{63})
                                         .SetType(ir::ValueType::U64);
                auto right_sign = __ AsrImm<ir::U64>(right, ir::Imm{63})
                                          .SetType(ir::ValueType::U64);
                high = __ Sub<ir::U64>(
                        high,
                        SingleOperand(__ And<ir::U64>(left_sign, SingleOperand(right))))
                               .SetType(ir::ValueType::U64);
                high = __ Sub<ir::U64>(
                        high,
                        SingleOperand(__ And<ir::U64>(right_sign, SingleOperand(left))))
                               .SetType(ir::ValueType::U64);
            }
            WriteXRegister(instr->GetRd(), high);
            return;
        }
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }
}

void A64Decoder::VisitConditionalSelect(const Instruction* instr) {
    bool is64 = instr->GetSixtyFourBits();
    auto type = GPRType(is64);
    auto cond = static_cast<ir::Cond>(instr->GetCondition());

    auto rn = ReadRegister(instr->GetRn(), type);
    auto rm = ReadRegister(instr->GetRm(), type);

    ir::Value false_val{};
    switch (instr->Mask(ConditionalSelectMask)) {
        case CSEL_w:
        case CSEL_x:
            false_val = rm;
            break;
        case CSINC_w:
        case CSINC_x:
            false_val = is64 ? __ Add<ir::U64>(rm, SingleOperand(ir::Imm{u32(1)})).SetType(type)
                             : __ Add<ir::U32>(rm, SingleOperand(ir::Imm{u32(1)})).SetType(type);
            break;
        case CSINV_w:
        case CSINV_x:
            false_val = is64
                    ? __ Xor<ir::U64>(rm, SingleOperand(ir::Imm{u64(-1)})).SetType(type)
                    : __ Xor<ir::U32>(rm, SingleOperand(ir::Imm{u32(-1)})).SetType(type);
            break;
        case CSNEG_w:
        case CSNEG_x:
            false_val = is64 ? __ Sub<ir::U64>(ImmValue(0, type), SingleOperand(rm)).SetType(type)
                             : __ Sub<ir::U32>(ImmValue(0, type), SingleOperand(rm)).SetType(type);
            break;
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }

    auto result = __ CondSelect(cond, rn, false_val).SetType(type);
    if (is64) {
        WriteXRegister(instr->GetRd(), result);
    } else {
        WriteWRegister(instr->GetRd(), result);
    }
}

void A64Decoder::ConditionalCompareHelper(const Instruction* instr,
                                          const ir::DataClass& op2) {
    bool is64 = instr->GetSixtyFourBits();
    auto type = GPRType(is64);
    auto rn = ReadRegister(instr->GetRn(), type);

    auto compare = __ Goto(CondPassed(static_cast<Condition>(instr->GetCondition())));

    const u32 nzcv = instr->GetNzcv();
    const bool n = (nzcv & 8) != 0;
    const bool z = (nzcv & 4) != 0;
    const u8 c = (nzcv >> 1) & 1;
    const u8 v = nzcv & 1;
    auto fallback = is64
            ? __ Sub<ir::U64>(ImmValue(1, type), SingleOperand(ir::Imm{0}))
                      .SetType(type)
            : __ Sub<ir::U32>(ImmValue(1, type), SingleOperand(ir::Imm{0}))
                      .SetType(type);
    __ SaveFlags(fallback, ir::Flags::NZ);
    __ SetCarry(ImmValue(c, ir::ValueType::U8));
    if (n) {
        auto negative = is64
                ? __ Sub<ir::U64>(
                          ImmValue(0, type), SingleOperand(ir::Imm{u64(1) << 63}))
                          .SetType(type)
                : __ Sub<ir::U32>(
                          ImmValue(0, type), SingleOperand(ir::Imm{u32(1) << 31}))
                          .SetType(type);
        __ SaveFlags(negative, ir::Flags::Negate);
        __ SetCarry(ImmValue(c, ir::ValueType::U8));
    }
    if (z) {
        auto zero = is64
                ? __ Sub<ir::U64>(ImmValue(0, type), SingleOperand(ir::Imm{0}))
                          .SetType(type)
                : __ Sub<ir::U32>(ImmValue(0, type), SingleOperand(ir::Imm{0}))
                          .SetType(type);
        __ SaveFlags(zero, ir::Flags::Zero);
        __ SetCarry(ImmValue(c, ir::ValueType::U8));
    }
    __ SetOverflow(ImmValue(v, ir::ValueType::U8));
    auto done = __ Goto(ir::BOOL{ImmValue(1, ir::ValueType::U8)});

    __ BindLabel(compare);
    const bool is_ccmp = instr->Mask(ConditionalCompareMask) == CCMP;
    auto result = is64
        ? (is_ccmp
                   ? __ Sub<ir::U64>(rn, SingleOperand(op2)).SetType(type)
                   : __ Add<ir::U64>(rn, SingleOperand(op2)).SetType(type))
        : (is_ccmp
                   ? __ Sub<ir::U32>(rn, SingleOperand(op2)).SetType(type)
                   : __ Add<ir::U32>(rn, SingleOperand(op2)).SetType(type));
    __ SaveFlags(result, ir::Flags::NZCV);
    // GetFlags commits the JIT's lazy NZCV before this path rejoins the
    // immediate-NZCV fallback path. Keep its value data-dependent so DCE
    // cannot discard the commit.
    auto committed = __ GetFlags(result, ir::Flags::NZCV).SetType(ir::ValueType::U64);
    auto carry = CondPassed(cs);
    __ SetCarry(__ Select(__ TestNotZero(committed), carry, carry)
                        .SetType(ir::ValueType::U8));
    __ BindLabel(done);
}

void A64Decoder::VisitConditionalCompareImmediate(const Instruction* instr) {
    u32 imm = u32(instr->GetImmCondCmp());
    ConditionalCompareHelper(instr, ir::DataClass{ir::Imm{imm}});
}

void A64Decoder::VisitConditionalCompareRegister(const Instruction* instr) {
    bool is64 = instr->GetSixtyFourBits();
    auto rm = ReadRegister(u8(instr->GetRm()), GPRType(is64));
    ConditionalCompareHelper(instr, ir::DataClass{rm});
}

void A64Decoder::VisitDataProcessing1Source(const Instruction* instr) {
    bool is64 = instr->GetSixtyFourBits();
    auto type = GPRType(is64);
    auto rn = ReadRegister(instr->GetRn(), type);
    auto rd = u8(instr->GetRd());
    u32 opcode = u32(instr->GetInstructionBits() >> 10) & 0x3F;

    // Helper: byte-swap within each 16-bit half-pair (REV16 step).
    auto swap_bytes_in_pairs = [&](ir::Value x) -> ir::Value {
        return __ Or<ir::U64>(
            __ LsrImm<ir::U64>(__ And<ir::U64>(x, SingleOperand(ir::Imm{u64(0xFF00FF00FF00FF00)})).SetType(ir::ValueType::U64), ir::Imm{8}).SetType(ir::ValueType::U64),
            SingleOperand(__ LslImm<ir::U64>(__ And<ir::U64>(x, SingleOperand(ir::Imm{u64(0x00FF00FF00FF00FF)})).SetType(ir::ValueType::U64), ir::Imm{8}).SetType(ir::ValueType::U64))
        ).SetType(ir::ValueType::U64);
    };
    auto swap16 = [&](ir::Value x) -> ir::Value {
        return __ Or<ir::U64>(
            __ LsrImm<ir::U64>(__ And<ir::U64>(x, SingleOperand(ir::Imm{u64(0xFFFF0000FFFF0000)})).SetType(ir::ValueType::U64), ir::Imm{16}).SetType(ir::ValueType::U64),
            SingleOperand(__ LslImm<ir::U64>(__ And<ir::U64>(x, SingleOperand(ir::Imm{u64(0x0000FFFF0000FFFF)})).SetType(ir::ValueType::U64), ir::Imm{16}).SetType(ir::ValueType::U64))
        ).SetType(ir::ValueType::U64);
    };
    auto swap32 = [&](ir::Value x) -> ir::Value {
        return __ Or<ir::U64>(
            __ LsrImm<ir::U64>(x, ir::Imm{32}).SetType(ir::ValueType::U64),
            SingleOperand(__ LslImm<ir::U64>(x, ir::Imm{32}).SetType(ir::ValueType::U64))
        ).SetType(ir::ValueType::U64);
    };

    switch (opcode) {
        case 0x00: {  // RBIT
            auto x = Widen(rn);
            // Swap odd/even bits
            x = __ Or<ir::U64>(
                __ LsrImm<ir::U64>(__ And<ir::U64>(x, SingleOperand(ir::Imm{u64(0xAAAAAAAAAAAAAAAA)})).SetType(ir::ValueType::U64), ir::Imm{1}).SetType(ir::ValueType::U64),
                SingleOperand(__ LslImm<ir::U64>(__ And<ir::U64>(x, SingleOperand(ir::Imm{u64(0x5555555555555555)})).SetType(ir::ValueType::U64), ir::Imm{1}).SetType(ir::ValueType::U64))
            ).SetType(ir::ValueType::U64);
            // Swap pairs
            x = __ Or<ir::U64>(
                __ LsrImm<ir::U64>(__ And<ir::U64>(x, SingleOperand(ir::Imm{u64(0xCCCCCCCCCCCCCCCC)})).SetType(ir::ValueType::U64), ir::Imm{2}).SetType(ir::ValueType::U64),
                SingleOperand(__ LslImm<ir::U64>(__ And<ir::U64>(x, SingleOperand(ir::Imm{u64(0x3333333333333333)})).SetType(ir::ValueType::U64), ir::Imm{2}).SetType(ir::ValueType::U64))
            ).SetType(ir::ValueType::U64);
            // Swap nibbles
            x = __ Or<ir::U64>(
                __ LsrImm<ir::U64>(__ And<ir::U64>(x, SingleOperand(ir::Imm{u64(0xF0F0F0F0F0F0F0F0)})).SetType(ir::ValueType::U64), ir::Imm{4}).SetType(ir::ValueType::U64),
                SingleOperand(__ LslImm<ir::U64>(__ And<ir::U64>(x, SingleOperand(ir::Imm{u64(0x0F0F0F0F0F0F0F0F)})).SetType(ir::ValueType::U64), ir::Imm{4}).SetType(ir::ValueType::U64))
            ).SetType(ir::ValueType::U64);
            x = swap_bytes_in_pairs(x);
            x = swap16(x);
            if (is64) x = swap32(x);
            if (is64) WriteXRegister(rd, x); else WriteWRegister(rd, x);
            break;
        }
        case 0x01: {  // REV16
            auto x = swap_bytes_in_pairs(Widen(rn));
            if (is64) WriteXRegister(rd, x); else WriteWRegister(rd, x);
            break;
        }
        case 0x02: {  // REV (32-bit) / REV32 (64-bit)
            auto x = swap_bytes_in_pairs(Widen(rn));
            x = swap16(x);
            if (is64) WriteXRegister(rd, x); else WriteWRegister(rd, x);
            break;
        }
        case 0x03: {  // REV (64-bit only)
            auto x = swap_bytes_in_pairs(Widen(rn));
            x = swap16(x);
            x = swap32(x);
            WriteXRegister(rd, x);
            break;
        }
        case 0x04: {  // CLZ
            auto x = Widen(rn);
            x = __ Or<ir::U64>(x, SingleOperand(__ LsrImm<ir::U64>(x, ir::Imm{1})))
                        .SetType(ir::ValueType::U64);
            x = __ Or<ir::U64>(x, SingleOperand(__ LsrImm<ir::U64>(x, ir::Imm{2})))
                        .SetType(ir::ValueType::U64);
            x = __ Or<ir::U64>(x, SingleOperand(__ LsrImm<ir::U64>(x, ir::Imm{4})))
                        .SetType(ir::ValueType::U64);
            x = __ Or<ir::U64>(x, SingleOperand(__ LsrImm<ir::U64>(x, ir::Imm{8})))
                        .SetType(ir::ValueType::U64);
            x = __ Or<ir::U64>(x, SingleOperand(__ LsrImm<ir::U64>(x, ir::Imm{16})))
                        .SetType(ir::ValueType::U64);
            if (is64) {
                x = __ Or<ir::U64>(x, SingleOperand(__ LsrImm<ir::U64>(x, ir::Imm{32})))
                            .SetType(ir::ValueType::U64);
            }
            auto pairs = __ And<ir::U64>(
                    __ LsrImm<ir::U64>(x, ir::Imm{1}),
                    SingleOperand(ir::Imm{u64(0x5555555555555555)}))
                                 .SetType(ir::ValueType::U64);
            x = __ Sub<ir::U64>(x, SingleOperand(pairs)).SetType(ir::ValueType::U64);
            auto quarters = __ And<ir::U64>(
                    __ LsrImm<ir::U64>(x, ir::Imm{2}),
                    SingleOperand(ir::Imm{u64(0x3333333333333333)}))
                                    .SetType(ir::ValueType::U64);
            x = __ Add<ir::U64>(
                    __ And<ir::U64>(x, SingleOperand(ir::Imm{u64(0x3333333333333333)})),
                    SingleOperand(quarters))
                        .SetType(ir::ValueType::U64);
            x = __ And<ir::U64>(
                    __ Add<ir::U64>(x, SingleOperand(__ LsrImm<ir::U64>(x, ir::Imm{4}))),
                    SingleOperand(ir::Imm{u64(0x0F0F0F0F0F0F0F0F)}))
                        .SetType(ir::ValueType::U64);
            x = __ Add<ir::U64>(x, SingleOperand(__ LsrImm<ir::U64>(x, ir::Imm{8})))
                        .SetType(ir::ValueType::U64);
            x = __ Add<ir::U64>(x, SingleOperand(__ LsrImm<ir::U64>(x, ir::Imm{16})))
                        .SetType(ir::ValueType::U64);
            if (is64) {
                x = __ Add<ir::U64>(x, SingleOperand(__ LsrImm<ir::U64>(x, ir::Imm{32})))
                            .SetType(ir::ValueType::U64);
            }
            auto pop = __ And<ir::U64>(x, SingleOperand(ir::Imm{is64 ? 0x7F : 0x3F}))
                               .SetType(ir::ValueType::U64);
            auto count = __ Sub<ir::U64>(
                    ImmValue(is64 ? 64 : 32, ir::ValueType::U64),
                    SingleOperand(pop))
                                 .SetType(ir::ValueType::U64);
            if (is64) WriteXRegister(rd, count); else WriteWRegister(rd, count);
            break;
        }
        case 0x05: {  // CLS
            Interrupt(InterruptReason::FALLBACK, current_pc);
            break;
        }
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            return;
    }
}

}  // namespace swift::arm64
