//
// Created by 甘尧 on 2024/1/4.
//

#include "arm64_frontend.h"
#include "cpu.h"

namespace swift::arm64 {

#define __ assembler->

namespace {

constexpr ir::ValueType GPRType(bool is64) {
    return is64 ? ir::ValueType::U64 : ir::ValueType::U32;
}

constexpr auto DCZID_EL0 = SystemRegisterEncoder<3, 3, 0, 0, 7>::value;

// Single-sided operand: right side is Void (Null) so EmitOperand takes the
// fast path (register or materialized immediate, no composite operation).
// DataClass::ToArgClass() handles Void correctly (maps to ArgClass{}).
ir::Operand SingleOperand(const ir::DataClass& data) {
    return ir::Operand{data};
}

}  // namespace

A64Decoder::A64Decoder(swift::VAddr start,
                       runtime::MemoryInterface* memory,
                       runtime::ir::Assembler* visitor)
        : current_pc(start), memory(memory), assembler(visitor) {}

void A64Decoder::Decode() {
    vixl::aarch64::Decoder decoder;
    decoder.AppendVisitor(this);
    while (!end_decode_ && !assembler->EndCommit()) {
        auto code_ptr = memory->GetPointer(reinterpret_cast<void*>(current_pc));
        if (!code_ptr) {
            Interrupt(InterruptReason::PAGE_FATAL, current_pc);
            break;
        }
        auto instr = reinterpret_cast<const Instruction*>(code_ptr);
        decoder.Decode(instr);
        current_pc += kInstructionSize;
        if (!end_decode_ && !assembler->EndCommit()) {
            __ AdvancePC(ir::Imm{u32(kInstructionSize)});
        }
    }
}

// ---------------------------------------------------------------------------
// Register access
// ---------------------------------------------------------------------------

u32 A64Decoder::GPROffset(u8 code) {
    switch (code) {
        case 29:
            return u32(offsetof(ThreadContext64, fp));
        case 30:
            return u32(offsetof(ThreadContext64, lr));
        case 31:
            return u32(offsetof(ThreadContext64, sp));
        default:
            return u32(offsetof(ThreadContext64, r)) + code * sizeof(u64);
    }
}

ir::Value A64Decoder::ImmValue(u64 imm, ir::ValueType type) {
    // NOTE: SetType() is applied to every value built by this decoder because
    // the Assembler INST helper drops the RetType template argument, leaving
    // instruction return types VOID (see report).
    switch (type) {
        case ir::ValueType::U8:
            return __ LoadImm<ir::U8>(ir::Imm{u8(imm)}).SetType(type);
        case ir::ValueType::U16:
            return __ LoadImm<ir::U16>(ir::Imm{u16(imm)}).SetType(type);
        case ir::ValueType::U32:
            return __ LoadImm<ir::U32>(ir::Imm{u32(imm)}).SetType(type);
        default:
            return __ LoadImm<ir::U64>(ir::Imm{imm}).SetType(ir::ValueType::U64);
    }
}

ir::Value A64Decoder::Widen(ir::Value value) {
    if (value.Type() == ir::ValueType::U64) {
        return value;
    }
    return __ ZeroExtend64(value).SetType(ir::ValueType::U64);
}

ir::Value A64Decoder::ReadRegister(u8 code, ir::ValueType size, Reg31Mode r31mode) {
    if (r31mode == Reg31IsZeroRegister && code == kZeroRegCode) {
        return ImmValue(0, size);
    }
    return __ LoadUniform(ir::Uniform{GPROffset(code), size}).SetType(size);
}

ir::Value A64Decoder::ReadVRegister(u8 code, ir::ValueType type) {
    auto offset = u32(offsetof(ThreadContext64, v) + code * sizeof(u128));
    return __ LoadUniform(ir::Uniform{offset, type}).SetType(type);
}

ir::Value A64Decoder::ReadVHalf(u8 code, bool high) {
    auto offset = u32(offsetof(ThreadContext64, v) + code * sizeof(u128)) + (high ? 8 : 0);
    return __ LoadUniform<ir::U64>(ir::Uniform{offset, ir::ValueType::U64})
            .SetType(ir::ValueType::U64);
}

void A64Decoder::WriteVHalves(u8 code, ir::Value low, ir::Value high) {
    auto offset = u32(offsetof(ThreadContext64, v) + code * sizeof(u128));
    __ StoreUniform(ir::Uniform{offset, ir::ValueType::U64}, low);
    __ StoreUniform(ir::Uniform{offset + 8, ir::ValueType::U64}, high);
}

ir::Value A64Decoder::ZeroByteMask(ir::Value value) {
    constexpr u64 low_bits = 0x7F7F7F7F7F7F7F7F;
    constexpr u64 high_bits = 0x8080808080808080;
    auto low = __ And<ir::U64>(value, SingleOperand(ir::Imm{low_bits}))
                       .SetType(ir::ValueType::U64);
    auto sum = __ Add<ir::U64>(low, SingleOperand(ir::Imm{low_bits}))
                       .SetType(ir::ValueType::U64);
    auto set = __ Or<ir::U64>(sum, SingleOperand(value)).SetType(ir::ValueType::U64);
    set = __ Or<ir::U64>(set, SingleOperand(ir::Imm{low_bits}))
                  .SetType(ir::ValueType::U64);
    auto inverted = __ Xor<ir::U64>(set, SingleOperand(ir::Imm{u64(-1)}))
                            .SetType(ir::ValueType::U64);
    auto msb = __ And<ir::U64>(inverted, SingleOperand(ir::Imm{high_bits}))
                       .SetType(ir::ValueType::U64);
    auto bits = __ LsrImm<ir::U64>(msb, ir::Imm{7}).SetType(ir::ValueType::U64);
    return __ Mul<ir::U64>(bits, SingleOperand(ir::Imm{0xFF}))
            .SetType(ir::ValueType::U64);
}

void A64Decoder::WriteXRegister(u8 code, ir::Value value, Reg31Mode r31mode) {
    if (r31mode == Reg31IsZeroRegister && code == kZeroRegCode) {
        return;
    }
    __ StoreUniform(ir::Uniform{GPROffset(code), ir::ValueType::U64}, Widen(value));
}

void A64Decoder::WriteWRegister(u8 code, ir::Value value, Reg31Mode r31mode) {
    if (r31mode == Reg31IsZeroRegister && code == kZeroRegCode) {
        return;
    }
    ir::Value wide;
    switch (value.Type()) {
        case ir::ValueType::U8:
        case ir::ValueType::U16:
        case ir::ValueType::U32:
            wide = __ ZeroExtend64(value).SetType(ir::ValueType::U64);
            break;
        default:
            // 64 bit (or untyped) value: explicitly clear the top half.
            wide = __ And<ir::U64>(value, SingleOperand(ir::Imm{u64(0xFFFFFFFF)}))
                           .SetType(ir::ValueType::U64);
            break;
    }
    __ StoreUniform(ir::Uniform{GPROffset(code), ir::ValueType::U64}, wide);
}

void A64Decoder::WriteVRegister(u8 code, ir::Value value) {
    auto offset = u32(offsetof(ThreadContext64, v) + code * sizeof(u128));
    __ StoreUniform(ir::Uniform{offset, value.Type()}, value);
}

void A64Decoder::WritePC(ir::Lambda new_pc) { assembler->SetLocation(new_pc); }

VAddr A64Decoder::CurrentPC() const { return current_pc; }

// ---------------------------------------------------------------------------
// Scalar helpers
// ---------------------------------------------------------------------------

ir::Value A64Decoder::SignExtendValue(ir::Value value, u32 from_bits) {
    auto wide = Widen(value);
    u32 shift = 64 - from_bits;
    if (shift == 0) {
        return wide;
    }
    auto shifted = __ LslImm<ir::U64>(wide, ir::Imm{shift}).SetType(ir::ValueType::U64);
    return __ AsrImm<ir::U64>(shifted, ir::Imm{shift}).SetType(ir::ValueType::U64);
}

ir::Value A64Decoder::ShiftOperand(bool is64, ir::Value value, Shift shift, u32 amount) {
    if (is64) {
        switch (shift) {
            case LSL:
                return amount ? __ LslImm<ir::U64>(value, ir::Imm{amount}).SetType(ir::ValueType::U64) : value;
            case LSR:
                return amount ? __ LsrImm<ir::U64>(value, ir::Imm{amount}).SetType(ir::ValueType::U64) : value;
            case ASR:
                return amount ? __ AsrImm<ir::U64>(value, ir::Imm{amount}).SetType(ir::ValueType::U64) : value;
            case ROR:
                return amount ? __ RorImm<ir::U64>(value, ir::Imm{amount}).SetType(ir::ValueType::U64) : value;
            default:
                VIXL_UNREACHABLE();
        }
    } else {
        switch (shift) {
            case LSL:
                return amount ? __ LslImm<ir::U32>(value, ir::Imm{amount}).SetType(ir::ValueType::U32) : value;
            case LSR:
                return amount ? __ LsrImm<ir::U32>(value, ir::Imm{amount}).SetType(ir::ValueType::U32) : value;
            case ASR:
                return amount ? __ AsrImm<ir::U32>(value, ir::Imm{amount}).SetType(ir::ValueType::U32) : value;
            case ROR: {
                // 32 bit rotate of a zero extended value.
                if (!amount) return value;
                auto lo = __ LsrImm<ir::U32>(value, ir::Imm{amount}).SetType(ir::ValueType::U32);
                auto hi = __ LslImm<ir::U32>(value, ir::Imm{32 - amount}).SetType(ir::ValueType::U32);
                return __ Or<ir::U32>(lo, SingleOperand(hi)).SetType(ir::ValueType::U32);
            }
            default:
                VIXL_UNREACHABLE();
        }
    }
    return {};
}

ir::Value A64Decoder::ExtendOperand(ir::Value value, Extend extend, u32 shift) {
    ir::Value extended;
    switch (extend) {
        case UXTB:
            extended = __ And<ir::U64>(Widen(value), SingleOperand(ir::Imm{u64(0xFF)}))
                             .SetType(ir::ValueType::U64);
            break;
        case UXTH:
            extended = __ And<ir::U64>(Widen(value), SingleOperand(ir::Imm{u64(0xFFFF)}))
                             .SetType(ir::ValueType::U64);
            break;
        case UXTW:
            extended = __ And<ir::U64>(Widen(value), SingleOperand(ir::Imm{u64(0xFFFFFFFF)}))
                             .SetType(ir::ValueType::U64);
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
        extended = __ LslImm<ir::U64>(extended, ir::Imm{shift}).SetType(ir::ValueType::U64);
    }
    return extended;
}

// ---------------------------------------------------------------------------
// Flags
// ---------------------------------------------------------------------------

ir::BOOL A64Decoder::BoolAnd(ir::BOOL a, ir::BOOL b) {
    return ir::BOOL{__ And<ir::U8>(a, SingleOperand(b)).SetType(ir::ValueType::U8)};
}

ir::BOOL A64Decoder::BoolOr(ir::BOOL a, ir::BOOL b) {
    return ir::BOOL{__ Or<ir::U8>(a, SingleOperand(b)).SetType(ir::ValueType::U8)};
}

ir::BOOL A64Decoder::BoolXor(ir::BOOL a, ir::BOOL b) {
    return ir::BOOL{__ Xor<ir::U8>(a, SingleOperand(b)).SetType(ir::ValueType::U8)};
}

ir::BOOL A64Decoder::CondPassed(Condition cond) {
    auto n = [&] { return ir::BOOL{__ TestFlags(ir::Flags::Negate)}; };
    auto z = [&] { return ir::BOOL{__ TestFlags(ir::Flags::Zero)}; };
    auto c = [&] { return ir::BOOL{__ TestFlags(ir::Flags::Carry)}; };
    auto v = [&] { return ir::BOOL{__ TestFlags(ir::Flags::Overflow)}; };
    auto not_n = [&] { return ir::BOOL{__ TestNotFlags(ir::Flags::Negate)}; };
    auto not_z = [&] { return ir::BOOL{__ TestNotFlags(ir::Flags::Zero)}; };
    auto not_c = [&] { return ir::BOOL{__ TestNotFlags(ir::Flags::Carry)}; };
    auto not_v = [&] { return ir::BOOL{__ TestNotFlags(ir::Flags::Overflow)}; };
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
            return __ TestZero(BoolXor(n(), v()));
        case lt:
            // N != V
            return __ TestNotZero(BoolXor(n(), v()));
        case gt:
            // !Z && (N == V)
            return BoolAnd(__ TestZero(BoolXor(n(), v())), not_z());
        case le:
            // Z || (N != V)
            return BoolOr(__ TestNotZero(BoolXor(n(), v())), z());
        case al:
        case nv:
        default:
            return ir::BOOL{__ LoadImm<ir::U8>(ir::Imm{true})};
    }
}

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------

ir::Value A64Decoder::AddressAdd(ir::Value base, s64 offset) {
    if (offset == 0) {
        return base;
    } else if (offset > 0) {
        return __ Add<ir::U64>(base, SingleOperand(ir::Imm{u64(offset)}))
                .SetType(ir::ValueType::U64);
    } else {
        return __ Sub<ir::U64>(base, SingleOperand(ir::Imm{u64(-offset)}))
                .SetType(ir::ValueType::U64);
    }
}

ir::Value A64Decoder::ReadMemory(ir::Lambda address, ir::ValueType type) {
    auto operand = address.IsValue() ? SingleOperand(address.GetValue())
                                     : SingleOperand(address.GetImm());
    return __ LoadMemory(operand).SetType(type);
}

void A64Decoder::WriteMemory(ir::Value address, ir::Value value) {
    __ StoreMemory(SingleOperand(address), value);
}

// ---------------------------------------------------------------------------
// Block control
// ---------------------------------------------------------------------------

void A64Decoder::BranchImm(VAddr target) {
    assembler->LinkBlock(ir::terminal::LinkBlock{target});
    end_decode_ = true;
}

void A64Decoder::Interrupt(InterruptReason reason, VAddr resume_pc) {
    __ SetLocation(ir::Lambda{ir::Imm{resume_pc}});
    __ StoreUniform(ir::Uniform{kInterruptUniformOffset, ir::ValueType::U32},
                    __ LoadImm<ir::U32>(ir::Imm{static_cast<u32>(reason)})
                            .SetType(ir::ValueType::U32));
    __ ReturnToHost();
    end_decode_ = true;
}

// ---------------------------------------------------------------------------
// Add / Sub
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Logical
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Move wide
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Bitfield / extract
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Multiply / divide / variable shifts
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// PC relative addressing
// ---------------------------------------------------------------------------

void A64Decoder::VisitPCRelAddressing(const Instruction* instr) {
    VAddr address;
    if (instr->Mask(PCRelAddressingMask) == ADRP) {
        address = (current_pc & ~VAddr(0xFFF)) + s64(instr->GetImmPCRel()) * s64(kPageSize);
    } else {
        VIXL_ASSERT(instr->Mask(PCRelAddressingMask) == ADR);
        address = current_pc + s64(instr->GetImmPCRel());
    }
    WriteXRegister(instr->GetRd(),
                   __ LoadImm<ir::U64>(ir::Imm{address}).SetType(ir::ValueType::U64));
}

// ---------------------------------------------------------------------------
// Branches
// ---------------------------------------------------------------------------

void A64Decoder::VisitUnconditionalBranch(const Instruction* instr) {
    VAddr target = current_pc + s64(instr->GetImmUncondBranch()) * kInstructionSize;
    if (instr->Mask(UnconditionalBranchMask) == BL) {
        WriteXRegister(kLinkRegCode,
                       __ LoadImm<ir::U64>(ir::Imm{NextPC()}).SetType(ir::ValueType::U64));
        __ PushRSB(ir::Lambda{ir::Imm{NextPC()}});
    }
    BranchImm(target);
}

void A64Decoder::VisitConditionalBranch(const Instruction* instr) {
    VIXL_ASSERT(instr->Mask(ConditionalBranchMask) == B_cond);
    VAddr target = current_pc + s64(instr->GetImmCondBranch()) * kInstructionSize;
    auto cond = CondPassed(static_cast<Condition>(instr->GetConditionBranch()));
    assembler->If(ir::terminal::If{
            cond, ir::terminal::LinkBlock{target}, ir::terminal::LinkBlock{NextPC()}});
    end_decode_ = true;
}

void A64Decoder::VisitCompareBranch(const Instruction* instr) {
    bool is64 = false;
    bool non_zero = false;
    switch (instr->Mask(CompareBranchMask)) {
        case CBZ_w:
            break;
        case CBZ_x:
            is64 = true;
            break;
        case CBNZ_w:
            non_zero = true;
            break;
        case CBNZ_x:
            is64 = true;
            non_zero = true;
            break;
        default:
            VIXL_UNREACHABLE();
    }
    auto value = ReadRegister(instr->GetRt(), GPRType(is64));
    ir::BOOL cond = non_zero ? __ TestNotZero(value) : __ TestZero(value);
    VAddr target = current_pc + s64(instr->GetImmCmpBranch()) * kInstructionSize;
    assembler->If(ir::terminal::If{
            cond, ir::terminal::LinkBlock{target}, ir::terminal::LinkBlock{NextPC()}});
    end_decode_ = true;
}

void A64Decoder::VisitTestBranch(const Instruction* instr) {
    u32 bit_pos = (instr->GetImmTestBranchBit5() << 5) | instr->GetImmTestBranchBit40();
    auto value = ReadXRegister(instr->GetRt());
    ir::BOOL bit_set = __ TestBit(value, ir::Imm{u8(bit_pos)});
    ir::BOOL cond = bit_set;
    if (instr->Mask(TestBranchMask) == TBZ) {
        cond = ir::BOOL{__ Xor<ir::U8>(bit_set, SingleOperand(ir::Imm{true}))};
    }
    VAddr target = current_pc + s64(instr->GetImmTestBranch()) * kInstructionSize;
    assembler->If(ir::terminal::If{
            cond, ir::terminal::LinkBlock{target}, ir::terminal::LinkBlock{NextPC()}});
    end_decode_ = true;
}

void A64Decoder::VisitUnconditionalBranchToRegister(const Instruction* instr) {
    switch (instr->Mask(UnconditionalBranchToRegisterMask)) {
        case RET: {
            WritePC(ir::Lambda{ReadXRegister(instr->GetRn())});
            // NOTE: PopRSB is not emitted: zero-argument IR instructions
            // currently crash Inst::Validate (IR issue, see report).
            __ Return();
            end_decode_ = true;
            break;
        }
        case BR: {
            WritePC(ir::Lambda{ReadXRegister(instr->GetRn())});
            __ ReturnToDispatcher();
            end_decode_ = true;
            break;
        }
        case BLR: {
            auto target = ReadXRegister(instr->GetRn());
            WriteXRegister(kLinkRegCode,
                       __ LoadImm<ir::U64>(ir::Imm{NextPC()}).SetType(ir::ValueType::U64));
            __ PushRSB(ir::Lambda{ir::Imm{NextPC()}});
            WritePC(ir::Lambda{target});
            __ ReturnToDispatcher();
            end_decode_ = true;
            break;
        }
        default:
            // Pointer authentication branches are not supported yet.
            Interrupt(InterruptReason::FALLBACK, current_pc);
            break;
    }
}

// ---------------------------------------------------------------------------
// Loads / stores
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------

void A64Decoder::VisitSystem(const Instruction* instr) {
    if (instr->Mask(SystemHintFMask) == SystemHintFixed) {
        // NOP, YIELD, WFE, WFI, SEV(L), BTI, ... are all treated as NOPs.
        // NOTE: nothing is emitted: the zero-argument Nop IR instruction
        // currently crashes Inst::Validate (IR issue, see report).
        return;
    }

    if (instr->Mask(MemBarrierFMask) == MemBarrierFixed) {
        // DMB / DSB / ISB: no reordering is performed by this IR, so they are
        // dropped here as well.
        return;
    }

    if (instr->Mask(SystemSysRegFMask) == SystemSysRegFixed) {
        auto sysreg = instr->GetImmSystemRegister();
        bool is_read = instr->Mask(SystemSysRegMask) == MRS;
        auto rt = instr->GetRt();
        switch (sysreg) {
            case TPIDR_EL0:
            case TPIDRRO_EL0: {
                ir::Uniform uni{u32(offsetof(ThreadContext64, tpidr)), ir::ValueType::U64};
                if (is_read) {
                    WriteXRegister(rt, __ LoadUniform<ir::U64>(uni));
                } else {
                    __ StoreUniform(uni, ReadXRegister(rt));
                }
                return;
            }
            case FPCR: {
                ir::Uniform uni{u32(offsetof(ThreadContext64, fpcr)), ir::ValueType::U32};
                if (is_read) {
                    WriteWRegister(rt, __ LoadUniform<ir::U32>(uni));
                } else {
                    __ StoreUniform(uni, ReadRegister(rt, ir::ValueType::U32));
                }
                return;
            }
            case FPSR: {
                ir::Uniform uni{u32(offsetof(ThreadContext64, fpsr)), ir::ValueType::U32};
                if (is_read) {
                    WriteWRegister(rt, __ LoadUniform<ir::U32>(uni));
                } else {
                    __ StoreUniform(uni, ReadRegister(rt, ir::ValueType::U32));
                }
                return;
            }
            case DCZID_EL0:
                if (is_read) {
                    // DZP=1: DC ZVA is prohibited. Returning zero would
                    // advertise a supported four-byte zeroing block.
                    WriteXRegister(rt, ImmValue(1u << 4, ir::ValueType::U64));
                }
                return;
            default:
                // Unknown system registers (NZCV, MIDR_EL1, …): MRS reads
                // return 0; MSR writes are ignored.
                if (is_read) {
                    WriteXRegister(rt, ImmValue(0, ir::ValueType::U64));
                }
                return;
        }
    }

    Interrupt(InterruptReason::FALLBACK, current_pc);
}

void A64Decoder::VisitException(const Instruction* instr) {
    switch (instr->Mask(ExceptionMask)) {
        case SVC:
            // System call: hand over to the host, resuming at the next
            // instruction once the syscall has been serviced. The syscall
            // number / arguments are read from the guest context (x8, x0-x5).
            Interrupt(InterruptReason::SVC, NextPC());
            break;
        case BRK:
            Interrupt(InterruptReason::BRK, current_pc);
            break;
        case HLT:
            Interrupt(InterruptReason::HLT, current_pc);
            break;
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            break;
    }
}

// ---------------------------------------------------------------------------
// NEON copy (DUP scalar, UMOV)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Conditional compare (CCMN / CCMP)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Data processing (1 source): RBIT, REV16, REV32, REV, CLZ, CLS
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Load/store exclusive (LDXR, STXR, LDAXR, STLXR, CAS, STLR, LDAR)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Atomic memory operations (LDADD, SWP, LDSET, LDCLR, LDEOR, etc.)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Fallbacks
// ---------------------------------------------------------------------------

void A64Decoder::VisitUnimplemented(const Instruction* instr) {
    Interrupt(InterruptReason::FALLBACK, current_pc);
}

void A64Decoder::VisitUnallocated(const Instruction* instr) {
    Interrupt(InterruptReason::ILL_CODE, current_pc);
}

void A64Decoder::VisitReserved(const Instruction* instr) {
    Interrupt(InterruptReason::ILL_CODE, current_pc);
}

}  // namespace swift::arm64
