//
// Created by 甘尧 on 2024/1/4.
//

#include "arm64_frontend.h"

namespace swift::arm64 {

#define __ assembler->

A64Decoder::A64Decoder(swift::VAddr start, runtime::MemoryInterface* memory, runtime::ir::Assembler* visitor) : assembler(visitor) {}

void A64Decoder::Decode() {

}

void A64Decoder::WriteXRegister(u8 code, ir::Value value, Reg31Mode r31mode) {
    switch (value.Type()) {
        case ir::ValueType::U64: {
            if (r31mode == Reg31IsZeroRegister) {

            } else {

            }
            break;
        }
        case ir::ValueType::U32: {
            if (r31mode == Reg31IsZeroRegister) {

            } else {

            }
            break;
        }
        case ir::ValueType::U16: {
            if (r31mode == Reg31IsZeroRegister) {

            } else {

            }
            break;
        }
        case ir::ValueType::U8: {
            if (r31mode == Reg31IsZeroRegister) {

            } else {

            }
            break;
        }
        case ir::ValueType::V128: {

            break;
        }
        case ir::ValueType::V64: {

            break;
        }
        default:
            VIXL_UNREACHABLE();
    }
}

void A64Decoder::WriteWRegister(u8 code, ir::Value value, Reg31Mode r31mode) {
    switch (value.Type()) {
        case ir::ValueType::U64: {
            if (r31mode == Reg31IsZeroRegister) {

            } else {

            }
            break;
        }
        case ir::ValueType::U32: {
            if (r31mode == Reg31IsZeroRegister) {

            } else {

            }
            break;
        }
        case ir::ValueType::U16: {
            if (r31mode == Reg31IsZeroRegister) {

            } else {

            }
            break;
        }
        case ir::ValueType::U8: {
            if (r31mode == Reg31IsZeroRegister) {

            } else {

            }
            break;
        }
        case ir::ValueType::V128: {

            break;
        }
        case ir::ValueType::V64: {

            break;
        }
        default:
            VIXL_UNREACHABLE();
    }
}

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
            new_xn_val = __ BitInsert(prev_xn_val, __ LoadImm(ir::Imm((u16) instr->GetImmMoveWide())), ir::Imm{(u8) shift},  ir::Imm{16u});
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

}
