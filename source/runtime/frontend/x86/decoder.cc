//
// Created by SwiftGan on 2021/1/1.
//

#include <cmath>
#include "runtime/frontend/x86/cpu.h"
#include "runtime/frontend/x86/decoder.h"

namespace swift::x86 {
#define __ assembler->

constexpr u8 TimesToShift(u32 times) {
    switch (times) {
        case 1:
            return 0;
        case 2:
            return 1;
        case 4:
            return 2;
        case 8:
            return 3;
        default:
            UNREACHABLE();
            break;
    }
}

ir::Uniform ToReg(const X86RegInfo& info) {
    auto base_offset = offsetof(ThreadContext64, regs);
    u32 offset = base_offset + info.index * sizeof(Reg);
    ir::ValueType type = info.type;
    return ir::Uniform{offset, type};
}

ir::Uniform ToVReg(const X86RegInfo& info) {
    auto base_offset = offsetof(ThreadContext64, xmms);
    u32 offset = base_offset + (info.index - X86RegInfo::Xmm0) * sizeof(Reg);
    ir::ValueType type = info.type;
    return ir::Uniform{offset, type};
}

ir::Operand ToOperand(ir::Lambda address) {
    if (address.IsValue()) {
        return ir::Operand{address.GetValue()};
    } else {
        return ir::Operand{address.GetImm()};
    }
}

ir::Operand ToOperand(ir::Value value) { return ir::Operand{value}; }

X64Decoder::X64Decoder(VAddr start, runtime::MemoryInterface* memory, ir::Assembler* visitor)
        : start(start), assembler(visitor), memory(memory) {}

void X64Decoder::Decode() {
    pc = start;
    while (!end_decode) {
        auto code_ptr = reinterpret_cast<u8*>(memory->GetPointer(reinterpret_cast<void*>(pc)));
        if (!code_ptr) {
            Interrupt(InterruptReason::PAGE_FATAL);
            break;
        }
        _DInst insn = DisDecode(code_ptr, 0x10, 1);
        if (insn.opcode == UINT16_MAX) {
            Interrupt(InterruptReason::ILL_CODE);
            break;
        }
        pc += insn.size;
        if (!DecodeSwitch(insn)) {
            Interrupt(InterruptReason::FALLBACK);
            break;
        }
        assembler->AdvancePC(ir::Imm{insn.size});
    }
}

bool X64Decoder::DecodeSwitch(_DInst& insn) {
    switch (insn.opcode) {
        case I_NOP:
            __ Nop();
            break;
        case I_HLT:
            Interrupt(InterruptReason::HLT);
            break;
        case I_INT_3:
            Interrupt(InterruptReason::BRK);
            break;
        case I_SYSCALL:
            Interrupt(InterruptReason::SVC);
            break;
        case I_CALL: {
            Push(__ LoadImm(ir::Imm(pc)));
            __ PushRSB(ir::Lambda(ir::Imm{pc}));
            DecodeCondJump(insn, Cond::AL);
            break;
        }
        case I_RET: {
            auto ret_addr = Pop(_RegisterType::R_RIP, ir::ValueType::U64);
            __ SetLocation(ir::Lambda{ret_addr});
            __ PopRSB();
            __ Return();
            end_decode = true;
            break;
        }
        case I_RETF: {
            auto ret_addr = Pop(_RegisterType::R_RIP, ir::ValueType::U64);
            __ SetLocation(ir::Lambda{ret_addr});
            __ PopRSB();
            __ Return();
            end_decode = true;
            break;
        }
        case I_LEAVE:
            R(_RegisterType::R_RSP, R(_RegisterType::R_RBP));
            Pop(_RegisterType::R_RBP, ir::ValueType::U64);
            break;
        case I_LEA:
            DecodeLea(insn);
            break;
        case I_JMP:
            DecodeCondJump(insn, Cond::AL);
            break;
        case I_JA:
            DecodeCondJump(insn, Cond::AT);
            break;
        case I_JAE:
            DecodeCondJump(insn, Cond::AE);
            break;
        case I_JB:
            DecodeCondJump(insn, Cond::BT);
            break;
        case I_JBE:
            DecodeCondJump(insn, Cond::BE);
            break;
        case I_JZ:
            DecodeCondJump(insn, Cond::EQ);
            break;
        case I_JNZ:
            DecodeCondJump(insn, Cond::NE);
            break;
        case I_JG:
            DecodeCondJump(insn, Cond::GT);
            break;
        case I_JGE:
            DecodeCondJump(insn, Cond::GE);
            break;
        case I_JL:
            DecodeCondJump(insn, Cond::LT);
            break;
        case I_JLE:
            DecodeCondJump(insn, Cond::LE);
            break;
        case I_JS:
            DecodeCondJump(insn, Cond::SN);
            break;
        case I_JNS:
            DecodeCondJump(insn, Cond::NS);
            break;
        case I_JP:
            DecodeCondJump(insn, Cond::PA);
            break;
        case I_JO:
            DecodeCondJump(insn, Cond::OF);
            break;
        case I_JNO:
            DecodeCondJump(insn, Cond::NO);
            break;
        case I_JNP:
            DecodeCondJump(insn, Cond::NP);
            break;
        case I_JECXZ:
            DecodeZeroCheckJump(insn, _RegisterType::R_ECX);
            break;
        case I_JRCXZ:
            DecodeZeroCheckJump(insn, _RegisterType::R_RCX);
            break;
        case I_MOV:
            DecodeMov(insn);
            break;
        case I_CMOVA:
            DecodeCondMov(insn, Cond::AT);
            break;
        case I_CMOVAE:
            DecodeCondMov(insn, Cond::AE);
            break;
        case I_CMOVB:
            DecodeCondMov(insn, Cond::BT);
            break;
        case I_CMOVBE:
            DecodeCondMov(insn, Cond::BE);
            break;
        case I_CMOVZ:
            DecodeCondMov(insn, Cond::EQ);
            break;
        case I_CMOVG:
            DecodeCondMov(insn, Cond::GT);
            break;
        case I_CMOVGE:
            DecodeCondMov(insn, Cond::GE);
            break;
        case I_CMOVL:
            DecodeCondMov(insn, Cond::LT);
            break;
        case I_CMOVLE:
            DecodeCondMov(insn, Cond::LE);
            break;
        case I_CMOVNZ:
            DecodeCondMov(insn, Cond::NE);
            break;
        case I_CMOVNO:
            DecodeCondMov(insn, Cond::NO);
            break;
        case I_CMOVO:
            DecodeCondMov(insn, Cond::OF);
            break;
        case I_CMOVP:
            DecodeCondMov(insn, Cond::PA);
            break;
        case I_CMOVNP:
            DecodeCondMov(insn, Cond::NP);
            break;
        case I_CMOVS:
            DecodeCondMov(insn, Cond::SN);
            break;
        case I_CMOVNS:
            DecodeCondMov(insn, Cond::NS);
            break;
        case I_ADD:
            DecodeAddSub(insn, false);
            break;
        case I_SUB:
            DecodeAddSub(insn, true);
            break;
        case I_CMP:
            DecodeAddSub(insn, true, false);
            break;
        case I_ADC:
            DecodeAddSubWithCarry(insn, false);
            break;
        case I_SBB:
            DecodeAddSubWithCarry(insn, true);
            break;
        case I_INC:
            DecodeIncAndDec(insn, false);
            break;
        case I_DEC:
            DecodeIncAndDec(insn, true);
            break;
        case I_MUL:
            DecodeMulDiv(insn, false);
            break;
        case I_DIV:
            DecodeMulDiv(insn, true);
            break;
        case I_OR:
            DecodeOr(insn);
            break;
        case I_XOR:
            DecodeXor(insn);
            break;
        default:
            return false;
    }
    return true;
}

void X64Decoder::DecodeMov(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto src = Src(insn, op1);
    Dst(insn, op0, src);
}

void X64Decoder::DecodeAddSub(_DInst& insn, bool sub, bool save_res) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = Src(insn, op0);
    auto right = Src(insn, op1);

    auto result = sub ? __ Sub(left, right) : __ Add(left, right);

    auto flags = __ GetFlags(result, ir::Flags::All);

//    SetFlag(CPUFlags::Carry, __ GetCarry(result));
//    SetFlag(CPUFlags::Overflow, __ GetOverFlow(result));
//    SetFlag(CPUFlags::Signed, __ GetSigned(result));
//    SetFlag(CPUFlags::Parity, __ GetParity(result));
//    SetFlag(CPUFlags::Zero, __ GetZero(result));

    if (save_res) {
        Dst(insn, op0, result);
    }
}

void X64Decoder::DecodeCondJump(_DInst& insn, Cond cond) {
    auto& op0 = insn.ops[0];

    auto address = AddrSrc(insn, op0);

    if (cond == Cond::AL) {
        if (address.IsValue()) {
            __ SetLocation(address);
            __ ReturnToDispatcher();
        } else {
            __ LinkBlock(ir::terminal::LinkBlock{address.GetImm().GetValue()});
        }
    } else {
        auto check_result = CheckCond(cond);
        CondGoto(check_result, address, pc + insn.size);
    }
}

void X64Decoder::DecodeZeroCheckJump(_DInst& insn, _RegisterType reg) {
    auto& op0 = insn.ops[0];
    auto value_check = R(reg);
    auto address = AddrSrc(insn, op0);

    CondGoto(__ Not(value_check), address, pc + insn.size);
}

void X64Decoder::DecodeAddSubWithCarry(_DInst& insn, bool sub) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = Src(insn, op0);
    auto right = Src(insn, op1);

    auto result = sub ? __ Sub(left, right) : __ Adc(left, right);

    auto flags = __ GetFlags(result, ir::Flags::All);

//    SetFlag(CPUFlags::Carry, __ GetCarry(result));
//    SetFlag(CPUFlags::Overflow, __ GetOverFlow(result));
//    SetFlag(CPUFlags::Signed, __ GetSigned(result));
//    SetFlag(CPUFlags::Parity, __ GetParity(result));
//    SetFlag(CPUFlags::Zero, __ GetZero(result));

    Dst(insn, op0, result);
}

void X64Decoder::DecodeIncAndDec(_DInst& insn, bool dec) {
    auto& op0 = insn.ops[0];
    auto src = Src(insn, op0);
    auto result = dec ? __ Sub(src, ir::Imm(1u)) : __ Add(src, ir::Imm(1u));

    auto flags = __ GetFlags(result, ir::Flags::Overflow | ir::Flags::Negate | ir::Flags::Parity | ir::Flags::Zero);

//    SetFlag(CPUFlags::Overflow, __ GetOverFlow(result));
//    SetFlag(CPUFlags::Signed, __ GetSigned(result));
//    SetFlag(CPUFlags::Parity, __ GetParity(result));
//    SetFlag(CPUFlags::Zero, __ GetZero(result));

    Dst(insn, op0, result);
}

void X64Decoder::DecodeMulDiv(_DInst& insn, bool div) {
    auto& op0 = insn.ops[0];
    auto left = Src(insn, op0);
    auto right = R(div ? _RegisterType::R_RAX : _RegisterType::R_RAX);

    ClearFlags(CPUFlags::FlagsAll);
}

void X64Decoder::DecodeLea(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto address = GetAddress(insn, op1);
    if (address.IsValue()) {
        Dst(insn, op0, address.GetValue());
    } else {
        Dst(insn, op0, __ LoadImm(address.GetImm()));
    }
}

void X64Decoder::DecodeCondMov(_DInst& insn, Cond cond) {
    auto check_result = CheckCond(cond);
    auto label = __ NotGoto(check_result);
    DecodeMov(insn);
    __ BindLabel(label);
}

void X64Decoder::DecodeAnd(_DInst& insn) {}

ir::Value X64Decoder::R(_RegisterType reg) {
    assert(reg <= _RegisterType::R_XMM15);
    return __ LoadUniform(ToReg(x86_regs_table[reg]));
}

ir::Value X64Decoder::V(_RegisterType reg) {
    assert(reg <= _RegisterType::R_XMM15);
    return __ LoadUniform(ToVReg(x86_regs_table[reg]));
}

void X64Decoder::R(_RegisterType reg, ir::Value value) {
    __ StoreUniform(ToReg(x86_regs_table[reg]), value);
}

void X64Decoder::V(_RegisterType reg, ir::Value value) {
    __ StoreUniform(ToVReg(x86_regs_table[reg]), value);
}

ir::Value X64Decoder::GetFlag(CPUFlags flag) {
    u32 ef_offset = offsetof(ThreadContext64, ef);
    auto ef_value = __ LoadUniform(ir::Uniform{ef_offset, ir::ValueType::U32});
    return __ BitExtract(ef_value, ir::Imm(GetEFlagBit(flag)), ir::Imm(1u));
}

void X64Decoder::SetFlag(CPUFlags flag, ir::BOOL value) {
    u32 ef_offset = offsetof(ThreadContext64, ef);
    ir::Uniform ef_uni{ef_offset, ir::ValueType::U32};
    auto old_ef = __ LoadUniform(ef_uni);
    auto new_ef = __ BitInsert(old_ef, value, ir::Imm(GetEFlagBit(flag)), ir::Imm(1u));
    __ StoreUniform(ef_uni, new_ef);
}

void X64Decoder::ClearFlags(CPUFlags flags) {
    u32 ef_offset = offsetof(ThreadContext64, ef);
    ir::Uniform ef_uni{ef_offset, ir::ValueType::U32};
    auto old_ef = __ LoadUniform(ef_uni);
    auto new_ef = __ AndImm(old_ef, ir::Imm(~static_cast<u32>(flags)));
    __ StoreUniform(ef_uni, new_ef);
}

void X64Decoder::Interrupt(InterruptReason reason) {
    ir::Uniform uni_interrupt{offsetof(ThreadContext64, interrupt), ir::ValueType::U32};
    __ SetLocation(ir::Lambda{ir::Imm{pc}});
    __ StoreUniform(uni_interrupt, __ LoadImm(ir::Imm(static_cast<u32>(reason))));
    __ ReturnToHost();
    end_decode = true;
}

ir::BOOL X64Decoder::CheckCond(Cond cond) {
    switch (cond) {
        case Cond::AL:
            return __ LoadImm(ir::Imm(true));
        case Cond::EQ:
            return GetFlag(CPUFlags::Zero);
        case Cond::NE:
            return __ Not(GetFlag(CPUFlags::Zero));
        case Cond::CS:
            break;
        case Cond::CC:
            break;
        case Cond::MI:
            break;
        case Cond::PL:
            break;
        case Cond::VS:
            break;
        case Cond::VC:
            break;
        case Cond::HI:
            break;
        case Cond::LS:
            break;
        case Cond::GE:
            break;
        case Cond::LT:
            break;
        case Cond::GT:
            break;
        case Cond::LE:
            break;
        case Cond::NV:
            break;
        case Cond::AT:
            break;
        case Cond::AE:
            break;
        case Cond::BT:
            break;
        case Cond::BE:
            break;
        case Cond::SN:
            break;
        case Cond::NS:
            break;
        case Cond::PA:
            break;
        case Cond::NP:
            break;
    }
}

void X64Decoder::CondGoto(ir::BOOL cond, ir::Lambda then_, ir::Location else_) {
    if (then_.IsValue()) {
        auto label = __ NotGoto(cond);
        __ SetLocation(then_);
        __ BindLabel(label);
        __ If(ir::terminal::If{
                cond, ir::terminal::ReturnToDispatch{}, ir::terminal::LinkBlock{else_}});
    } else {
        __ If(ir::terminal::If{cond,
                               ir::terminal::LinkBlock{then_.GetImm().GetValue()},
                               ir::terminal::LinkBlock{else_}});
    }
}

ir::ValueType X64Decoder::GetSize(u32 bits) {
    switch (bits) {
        case 0:
            return ir::ValueType::VOID;
        case 8:
            return ir::ValueType::U8;
        case 16:
            return ir::ValueType::U16;
        case 32:
            return ir::ValueType::U32;
        case 64:
            return ir::ValueType::U64;
        default:
            PANIC();
            return ir::ValueType::VOID;
    }
}

ir::Value X64Decoder::Src(_DInst& insn, _Operand& operand) {
    ir::Value value{};
    auto size = GetSize(operand.size);
    switch (operand.type) {
        case O_PC:
            value = __ LoadImm(ir::Imm(pc + insn.imm.qword));
            break;
        case O_REG:
            if (operand.index == R_RIP) {
                value = __ LoadImm(ir::Imm(pc + insn.imm.qword));
            } else if (IsV(static_cast<_RegisterType>(operand.index))) {
                value = V(static_cast<_RegisterType>(operand.index));
            } else {
                value = R(static_cast<_RegisterType>(operand.index));
            }
            break;
        case O_IMM:
            value = __ LoadImm(ir::Imm{insn.imm.qword});
            break;
        case O_SMEM:
        case O_MEM:
            auto address = GetAddress(insn, operand);
            value = __ LoadMemory(address).SetType(size);
            break;
    }

    return value;
}

ir::Lambda X64Decoder::AddrSrc(_DInst& insn, _Operand& operand) {
    ir::Lambda value{};
    switch (operand.type) {
        case O_PC:
            value = ir::Imm(pc + insn.imm.qword);
            break;
        case O_REG:
            if (operand.index == R_RIP) {
                value = ir::Imm(pc + insn.imm.qword);
            } else if (IsV(static_cast<_RegisterType>(operand.index))) {
                value = V(static_cast<_RegisterType>(operand.index));
            } else {
                value = R(static_cast<_RegisterType>(operand.index));
            }
            break;
        case O_IMM:
            value = ir::Imm{insn.imm.qword, GetSize(operand.size)};
            break;
        case O_SMEM:
        case O_MEM:
            auto address = GetAddress(insn, operand);
            value = __ LoadMemory(ToOperand(address)).SetType(ir::ValueType::U64);
            break;
    }
    return value;
}

void X64Decoder::Dst(_DInst& insn, _Operand& operand, ir::Value value) {
    switch (operand.type) {
        case O_REG:
            if (IsV(static_cast<_RegisterType>(operand.index))) {
                V(static_cast<_RegisterType>(operand.index), value);
            } else {
                R(static_cast<_RegisterType>(operand.index), value);
            }
            break;
        case O_SMEM:
        case O_MEM:
            auto address = GetAddress(insn, operand);
            __ StoreMemory(ToOperand(address), value);
            break;
    }
}

bool X64Decoder::IsV(_RegisterType reg) { return reg >= R_ST0; }

ir::Lambda X64Decoder::GetAddress(_DInst& insn, _Operand& operand) {
    ir::Lambda value{};
    switch (operand.type) {
        case O_MEM: {
            u8 segment{};
            if (insn.segment == R_NONE) {
                switch (insn.base) {
                    case R_BP:
                    case R_EBP:
                    case R_RBP:
                        segment = R_SS;
                        break;
                    case R_RIP:
                        segment = R_CS;
                        break;
                    default:
                        segment = R_NONE;
                }
            }
            ir::Imm offset{insn.disp};
            if (insn.base <= R_DR7) {
                auto value_base = R(static_cast<_RegisterType>(insn.base));
                auto value_rn = R(static_cast<_RegisterType>(operand.index));
                auto value_base_index =
                        insn.scale ? __ Add(value_base, __ LslImm(value_rn, ir::Imm(insn.scale)))
                                   : __ Add(value_base, value_rn);
                value = insn.disp ? __ Add(value_base_index, offset) : value_base_index;
            } else {
                auto value_rn = R(static_cast<_RegisterType>(operand.index));
                auto value_base_index =
                        insn.scale ? __ LslImm(value_rn, ir::Imm(insn.scale)) : value_rn;
                value = insn.disp ? __ Add(value_base_index, offset) : value_base_index;
            }
            break;
        }
        case O_SMEM:
            if (operand.index == R_RIP) {
                value = ir::Lambda(ir::Imm{pc + insn.disp});
            } else {
                auto base_reg = R(static_cast<_RegisterType>(operand.index));
                value = __ Add(base_reg, ir::Imm(insn.disp));
            }
            break;
        default:
            PANIC();
    }
    if (value.IsValue() && value.GetValue().Type() != ir::ValueType::U64) {
        value = __ ZeroExtend64(value.GetValue());
    }
    return value;
}

ir::Value X64Decoder::Pop(_RegisterType reg, ir::ValueType size) {
    auto size_byte = ir::GetValueSizeByte(size);
    auto sp = _RegisterType::R_RSP;
    ir::Lambda address = R(sp);
    auto value = __ LoadMemory(address).SetType(size);
    if (IsV(reg)) {
        V(reg, value);
    } else {
        R(reg, value);
    }
    R(sp, __ Add(R(sp), ir::Imm((u8)size_byte)));
    return value;
}

void X64Decoder::Push(ir::Value value) {
    auto size = value.Type();
    auto size_byte = ir::GetValueSizeByte(size);
    auto sp = _RegisterType::R_RSP;
    auto address = R(sp);
    __ StoreMemory(R(sp), ToOperand(value));
    R(sp, __ Sub(R(sp), ir::Operand(ir::Imm(size_byte))));
}

void X64Decoder::DecodeOr(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = Src(insn, op0);
    auto right = Src(insn, op1);

    auto result = __ OrValue(left, right);
    ClearFlags(CPUFlags::Carry | CPUFlags::Overflow | CPUFlags::FlagAF);
    auto flags = __ GetFlags(result, ir::Flags::Negate | ir::Flags::Parity | ir::Flags::Zero);
    SetFlag(CPUFlags::Signed, __ TestBit(flags, ir::Imm{ir::FlagsBit::Negate}));
    SetFlag(CPUFlags::Parity, __ TestBit(flags, ir::Imm{ir::FlagsBit::Parity}));
    SetFlag(CPUFlags::Zero, __ TestBit(flags, ir::Imm{ir::FlagsBit::Zero}));
    Dst(insn, op0, result);
}

void X64Decoder::DecodeXor(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = Src(insn, op0);
    auto right = Src(insn, op1);

    auto result = __ XorValue(left, right);
    ClearFlags(CPUFlags::Carry | CPUFlags::Overflow | CPUFlags::FlagAF);
    auto flags = __ GetFlags(result, ir::Flags::Negate | ir::Flags::Parity | ir::Flags::Zero);
    SetFlag(CPUFlags::Signed, __ TestBit(flags, ir::Imm{ir::FlagsBit::Negate}));
    SetFlag(CPUFlags::Parity, __ TestBit(flags, ir::Imm{ir::FlagsBit::Parity}));
    SetFlag(CPUFlags::Zero, __ TestBit(flags, ir::Imm{ir::FlagsBit::Zero}));
    Dst(insn, op0, result);
}

}  // namespace swift::x86
