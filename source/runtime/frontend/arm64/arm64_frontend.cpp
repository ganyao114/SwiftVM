//
// Created by 甘尧 on 2024/1/4.
//

#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

A64Decoder::A64Decoder(swift::VAddr start,
                       runtime::MemoryInterface* memory,
                       runtime::compile::IrBuilder* visitor)
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
        runtime::compile::IrBuilder::GuestInstructionScope instruction{*assembler};
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
    switch (type) {
        case ir::ValueType::U8:
            return __ LoadImm(type, ir::Imm{u8(imm)});
        case ir::ValueType::U16:
            return __ LoadImm(type, ir::Imm{u16(imm)});
        case ir::ValueType::U32:
            return __ LoadImm(type, ir::Imm{u32(imm)});
        default:
            return __ LoadImm(ir::ValueType::U64, ir::Imm{imm});
    }
}

ir::Value A64Decoder::Widen(ir::Value value) {
    if (value.Type() == ir::ValueType::U64) {
        return value;
    }
    return __ ZeroExtend64(ir::ValueType::U64, value);
}

ir::Value A64Decoder::ReadRegister(u8 code, ir::ValueType size, Reg31Mode r31mode) {
    if (r31mode == Reg31IsZeroRegister && code == kZeroRegCode) {
        return ImmValue(0, size);
    }
    return __ LoadUniform(size, ir::Uniform{GPROffset(code), size});
}

ir::Value A64Decoder::ReadVRegister(u8 code, ir::ValueType type) {
    auto offset = u32(offsetof(ThreadContext64, v) + code * sizeof(u128));
    return __ LoadUniform(type, ir::Uniform{offset, type});
}

ir::Value A64Decoder::ReadVHalf(u8 code, bool high) {
    auto offset = u32(offsetof(ThreadContext64, v) + code * sizeof(u128)) + (high ? 8 : 0);
    return __ LoadUniform(ir::ValueType::U64, ir::Uniform{offset, ir::ValueType::U64});
}

void A64Decoder::WriteVHalves(u8 code, ir::Value low, ir::Value high) {
    auto offset = u32(offsetof(ThreadContext64, v) + code * sizeof(u128));
    __ StoreUniform(ir::Uniform{offset, ir::ValueType::U64}, low);
    __ StoreUniform(ir::Uniform{offset + 8, ir::ValueType::U64}, high);
}

ir::Value A64Decoder::ZeroByteMask(ir::Value value) {
    constexpr u64 low_bits = 0x7F7F7F7F7F7F7F7F;
    constexpr u64 high_bits = 0x8080808080808080;
    auto low = __ And(ir::ValueType::U64, value, SingleOperand(ir::Imm{low_bits}));
    auto sum = __ Add(ir::ValueType::U64, low, SingleOperand(ir::Imm{low_bits}));
    auto set = __ Or(ir::ValueType::U64, sum, SingleOperand(value));
    set = __ Or(ir::ValueType::U64, set, SingleOperand(ir::Imm{low_bits}));
    auto inverted = __ Xor(ir::ValueType::U64, set, SingleOperand(ir::Imm{u64(-1)}));
    auto msb = __ And(ir::ValueType::U64, inverted, SingleOperand(ir::Imm{high_bits}));
    auto bits = __ LsrImm(ir::ValueType::U64, msb, ir::Imm{7});
    return __ Mul(ir::ValueType::U64, bits, SingleOperand(ir::Imm{0xFF}));
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
            wide = __ ZeroExtend64(ir::ValueType::U64, value);
            break;
        default:
            // 64 bit (or untyped) value: explicitly clear the top half.
            wide = __ And(ir::ValueType::U64, value, SingleOperand(ir::Imm{u64(0xFFFFFFFF)}));
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
    auto shifted = __ LslImm(ir::ValueType::U64, wide, ir::Imm{shift});
    return __ AsrImm(ir::ValueType::U64, shifted, ir::Imm{shift});
}

ir::Value A64Decoder::ShiftOperand(bool is64, ir::Value value, Shift shift, u32 amount) {
    if (is64) {
        switch (shift) {
            case LSL:
                return amount ? __ LslImm(ir::ValueType::U64, value, ir::Imm{amount}) : value;
            case LSR:
                return amount ? __ LsrImm(ir::ValueType::U64, value, ir::Imm{amount}) : value;
            case ASR:
                return amount ? __ AsrImm(ir::ValueType::U64, value, ir::Imm{amount}) : value;
            case ROR:
                return amount ? __ RorImm(ir::ValueType::U64, value, ir::Imm{amount}) : value;
            default:
                VIXL_UNREACHABLE();
        }
    } else {
        switch (shift) {
            case LSL:
                return amount ? __ LslImm(ir::ValueType::U32, value, ir::Imm{amount}) : value;
            case LSR:
                return amount ? __ LsrImm(ir::ValueType::U32, value, ir::Imm{amount}) : value;
            case ASR:
                return amount ? __ AsrImm(ir::ValueType::U32, value, ir::Imm{amount}) : value;
            case ROR: {
                // 32 bit rotate of a zero extended value.
                if (!amount) return value;
                auto lo = __ LsrImm(ir::ValueType::U32, value, ir::Imm{amount});
                auto hi = __ LslImm(ir::ValueType::U32, value, ir::Imm{32 - amount});
                return __ Or(ir::ValueType::U32, lo, SingleOperand(hi));
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
            extended = __ And(ir::ValueType::U64, Widen(value), SingleOperand(ir::Imm{u64(0xFF)}));
            break;
        case UXTH:
            extended =
                    __ And(ir::ValueType::U64, Widen(value), SingleOperand(ir::Imm{u64(0xFFFF)}));
            break;
        case UXTW:
            extended = __ And(
                    ir::ValueType::U64, Widen(value), SingleOperand(ir::Imm{u64(0xFFFFFFFF)}));
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
        extended = __ LslImm(ir::ValueType::U64, extended, ir::Imm{shift});
    }
    return extended;
}

// ---------------------------------------------------------------------------
// Flags
// ---------------------------------------------------------------------------

ir::BOOL A64Decoder::BoolAnd(ir::BOOL a, ir::BOOL b) {
    return ir::BOOL{__ And(ir::ValueType::U8, a, SingleOperand(b))};
}

ir::BOOL A64Decoder::BoolOr(ir::BOOL a, ir::BOOL b) {
    return ir::BOOL{__ Or(ir::ValueType::U8, a, SingleOperand(b))};
}

ir::BOOL A64Decoder::BoolXor(ir::BOOL a, ir::BOOL b) {
    return ir::BOOL{__ Xor(ir::ValueType::U8, a, SingleOperand(b))};
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
        return __ Add(ir::ValueType::U64, base, SingleOperand(ir::Imm{u64(offset)}));
    } else {
        return __ Sub(ir::ValueType::U64, base, SingleOperand(ir::Imm{u64(-offset)}));
    }
}

ir::Value A64Decoder::ReadMemory(ir::Lambda address, ir::ValueType type) {
    auto operand =
            address.IsValue() ? SingleOperand(address.GetValue()) : SingleOperand(address.GetImm());
    return __ LoadMemory(type, operand);
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
                    __ LoadImm(ir::ValueType::U32, ir::Imm{static_cast<u32>(reason)}));
    __ ReturnToHost();
    end_decode_ = true;
}

// ---------------------------------------------------------------------------
// Add / Sub
// ---------------------------------------------------------------------------






// ---------------------------------------------------------------------------
// Logical
// ---------------------------------------------------------------------------



// ---------------------------------------------------------------------------
// Move wide
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Bitfield / extract
// ---------------------------------------------------------------------------



// ---------------------------------------------------------------------------
// Multiply / divide / variable shifts
// ---------------------------------------------------------------------------




// ---------------------------------------------------------------------------
// PC relative addressing
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Branches
// ---------------------------------------------------------------------------






// ---------------------------------------------------------------------------
// Loads / stores
// ---------------------------------------------------------------------------














// ---------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------



// ---------------------------------------------------------------------------
// NEON copy (DUP scalar, UMOV)
// ---------------------------------------------------------------------------










// ---------------------------------------------------------------------------
// Conditional compare (CCMN / CCMP)
// ---------------------------------------------------------------------------




// ---------------------------------------------------------------------------
// Data processing (1 source): RBIT, REV16, REV32, REV, CLZ, CLS
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Load/store exclusive (LDXR, STXR, LDAXR, STLXR, CAS, STLR, LDAR)
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Atomic memory operations (LDADD, SWP, LDSET, LDCLR, LDEOR, etc.)
// ---------------------------------------------------------------------------


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
