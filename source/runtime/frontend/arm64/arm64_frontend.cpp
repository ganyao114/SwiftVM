//
// Created by 甘尧 on 2024/1/4.
//

#include "arm64_frontend.h"
#include "cpu.h"

namespace swift::arm64 {

#define __ assembler->

A64Decoder::A64Decoder(swift::VAddr start,
                       runtime::MemoryInterface* memory,
                       runtime::ir::Assembler* visitor)
        : current_pc(start), memory(memory), assembler(visitor) {}

void A64Decoder::Decode() {}

ir::Value A64Decoder::ReadRegister(u8 code, ir::ValueType size, Reg31Mode r31mode) {
    if (r31mode == Reg31IsZeroRegister && code == kZeroRegCode) {
        return __ Zero();
    }

    switch (size) {
        case ir::ValueType::U64: {
            break;
        }
        case ir::ValueType::U32: {
            break;
        }
        case ir::ValueType::U16: {
            break;
        }
        case ir::ValueType::U8: {
            break;
        }
        default:
            VIXL_UNREACHABLE();
            return {};
    }
}

void A64Decoder::WriteXRegister(u8 code, ir::Value value, Reg31Mode r31mode) {
    if (r31mode == Reg31IsZeroRegister && code == kZeroRegCode) {
        return;
    }
    auto base_offset = offsetof(ThreadContext64, r);
    u32 offset = base_offset + code * sizeof(u64);
    ir::ValueType type = value.Type();
    ir::Uniform uni{offset, value.Type()};
    __ StoreUniform();
}

void A64Decoder::WriteWRegister(u8 code, ir::Value value, Reg31Mode r31mode) {
    if (r31mode == Reg31IsZeroRegister && code == kZeroRegCode) {
        return;
    }
    auto base_offset = offsetof(ThreadContext64, r);
    u32 offset = base_offset + code * sizeof(u64);
    ir::ValueType type = value.Type();
    ir::Uniform uni{offset, value.Type()};
    __ StoreUniform();
}

void A64Decoder::WriteVRegister(u8 code, ir::Value value) {
    auto base_offset = offsetof(ThreadContext64, v);
    u32 offset = base_offset + code * sizeof(u128);
    ir::ValueType type = value.Type();
    ir::Uniform uni{offset, value.Type()};
    __ StoreUniform();
}

void A64Decoder::WritePC(ir::Lambda new_pc) { assembler->SetLocation(new_pc); }

ir::Value A64Decoder::ReadMemory(ir::Lambda address, ir::ValueType type) { return {}; }

VAddr A64Decoder::CurrentPC() const { return current_pc; }

void A64Decoder::VisitSystem(const Instruction* instr) { DecoderVisitor::VisitSystem(instr); }

void A64Decoder::VisitMoveWideImmediate(const Instruction* instr) {
    auto mov_op = static_cast<MoveWideImmediateOp>(instr->Mask(MoveWideImmediateMask));
    ir::Value new_xn_val{};

    bool is_64_bits = instr->GetSixtyFourBits() == 1;
    // Shift is limited for W operations.
    VIXL_ASSERT(is_64_bits || (instr->GetShiftMoveWide() < 2));

    // Get the shifted immediate.
    s64 shift = instr->GetShiftMoveWide() * 16;
    s64 shifted_imm16 = static_cast<s64>(instr->GetImmMoveWide()) << shift;

    // Compute the new value.
    switch (mov_op) {
        case MOVN_w:
        case MOVN_x: {
            auto new_val = ~shifted_imm16;
            if (!is_64_bits) new_val &= kWRegMask;
            new_xn_val = __ LoadImm(ir::Imm(ForceCast<u64>(new_val)));
            break;
        }
        case MOVK_w:
        case MOVK_x: {
            auto reg_code = instr->GetRd();
            auto prev_xn_val = is_64_bits ? ReadXRegister(reg_code) : ReadWRegister(reg_code);
            new_xn_val = __ BitInsert(prev_xn_val,
                                      __ LoadImm(ir::Imm((u16)instr->GetImmMoveWide())),
                                      ir::Imm{(u8)shift},
                                      ir::Imm{16u});
            break;
        }
        case MOVZ_w:
        case MOVZ_x: {
            new_xn_val = __ LoadImm(ir::Imm(ForceCast<u64>(shifted_imm16)));
            break;
        }
        default:
            VIXL_UNREACHABLE();
    }

    // Update the destination register.
    WriteXRegister(instr->GetRd(), new_xn_val);
}

void A64Decoder::VisitLoadLiteral(const Instruction* instr) {
    unsigned rt = instr->GetRt();
    auto pc = CurrentPC();
    auto address = instr->GetLiteralAddress<VAddr>(pc);

    // Verify that the calculated address is available to the host.
    VIXL_ASSERT(address == static_cast<uintptr_t>(address));

    switch (instr->Mask(LoadLiteralMask)) {
        // Use NoRegLog to suppress the register trace (LOG_REGS, LOG_VREGS), then
        // print a more detailed log.
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
                    __ SignExtend(ReadMemory(ir::Lambda{ir::Imm{address}}, ir::ValueType::U32)));
            break;

        // Ignore prfm hint instructions.
        case PRFM_lit:
            break;

        default:
            VIXL_UNREACHABLE();
    }

    //    local_monitor_.MaybeClear();
}

void A64Decoder::VisitBitfield(const Instruction* instr) {
    u8 reg_size = instr->GetSixtyFourBits() ? kXRegSize : kWRegSize;
    s64 reg_mask = instr->GetSixtyFourBits() ? kXRegMask : kWRegMask;
    int R = instr->GetImmR();
    int S = instr->GetImmS();
    int diff = S - R;
    u64 mask;
    if (diff >= 0) {
        mask = ~UINT64_C(0) >> (64 - (diff + 1));
        mask = (static_cast<unsigned>(diff) < (reg_size - 1)) ? mask : reg_mask;
    } else {
        mask = ~UINT64_C(0) >> (64 - (S + 1));
        mask = RotateRight(mask, R, reg_size);
        diff += reg_size;
    }

    // inzero indicates if the extracted bitfield is inserted into the
    // destination register value or in zero.
    // If extend is true, extend the sign of the extracted bitfield.
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

    auto dst = inzero ? __ Zero() : ReadRegister(reg_size, instr->GetRd());
    auto src = ReadRegister(reg_size, instr->GetRn());
    // Rotate source bitfield into place.
    u64 result = RotateRight(src, R, reg_size);
    // Determine the sign extension.
    u64 topbits = (diff == 63) ? 0 : (~UINT64_C(0) << (diff + 1));
    u64 signbits = extend && ((src >> S) & 1) ? topbits : 0;

    // Merge sign extension, dest/zero and bitfield.
    result = signbits | (result & mask) | (dst & ~mask);

    if (reg_size == kXRegSize) {
        WriteXRegister(instr->GetRd(), result);
    } else {
        WriteWRegister(instr->GetRd(), result);
    }
}

}  // namespace swift::arm64
