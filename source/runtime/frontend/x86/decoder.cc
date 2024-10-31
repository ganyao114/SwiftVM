//
// Created by SwiftGan on 2021/1/1.
//

#include <array>
#include <cmath>
#include "runtime/frontend/x86/cpu.h"
#include "runtime/frontend/x86/decoder.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

static std::array<ABIRegUniform, 1> general_return_x86{{offsetof(ThreadContext64, rax), 4}};

static std::array<ABIRegUniform, 1> float_return_x86{{offsetof(ThreadContext64, xmm0), 16}};

static std::array<ABIRegUniform, 8> general_params_x64{
        ABIRegUniform{offsetof(ThreadContext64, rdi), 8},
        ABIRegUniform{offsetof(ThreadContext64, rsi), 8},
        ABIRegUniform{offsetof(ThreadContext64, rdx), 8},
        ABIRegUniform{offsetof(ThreadContext64, rcx), 8},
        ABIRegUniform{offsetof(ThreadContext64, r8), 8},
        ABIRegUniform{offsetof(ThreadContext64, r9), 8},
        ABIRegUniform{offsetof(ThreadContext64, rax), 8},
        ABIRegUniform{offsetof(ThreadContext64, rbx), 8}};

static std::array<ABIRegUniform, 8> float_params_x64{
        ABIRegUniform{offsetof(ThreadContext64, xmm0), 16},
        ABIRegUniform{offsetof(ThreadContext64, xmm1), 16},
        ABIRegUniform{offsetof(ThreadContext64, xmm2), 16},
        ABIRegUniform{offsetof(ThreadContext64, xmm3), 16},
        ABIRegUniform{offsetof(ThreadContext64, xmm4), 16},
        ABIRegUniform{offsetof(ThreadContext64, xmm5), 16},
        ABIRegUniform{offsetof(ThreadContext64, xmm6), 16},
        ABIRegUniform{offsetof(ThreadContext64, xmm7), 16}};

static std::array<ABIRegUniform, 2> general_return_x64{
        ABIRegUniform{offsetof(ThreadContext64, rdi), 8},
        ABIRegUniform{offsetof(ThreadContext64, rsi), 8}};

static std::array<ABIRegUniform, 2> float_return_x64{
        ABIRegUniform{offsetof(ThreadContext64, xmm0), 16},
        ABIRegUniform{offsetof(ThreadContext64, xmm1), 16}};

ABIDescriptor GetABIDescriptor32() {
    return {{}, {}, general_return_x86, float_return_x86};
}

ABIDescriptor GetABIDescriptor64() {
    return {general_params_x64, float_params_x64, general_return_x64, float_return_x64};
}

void FromHost(backend::State* state, ThreadContext64* ctx) {
    ctx->pc.qword = *state->current_loc;
    // TODO Flags
}

void ToHost(backend::State* state, ThreadContext64* ctx) {
    state->current_loc = ctx->pc.qword;
    // TODO Flags
}

#define __ assembler->

static ir::ValueType GetSize(u32 bits) {
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

static ir::ValueType GetSignedSize(u32 bits) {
    switch (bits) {
        case 0:
            return ir::ValueType::VOID;
        case 8:
            return ir::ValueType::S8;
        case 16:
            return ir::ValueType::S16;
        case 32:
            return ir::ValueType::S32;
        case 64:
            return ir::ValueType::S64;
        default:
            PANIC();
            return ir::ValueType::VOID;
    }
}

static ir::ValueType GetVecSize(u32 bits) {
    switch (bits) {
        case 0:
            return ir::ValueType::VOID;
        case 8:
            return ir::ValueType::V8;
        case 16:
            return ir::ValueType::V16;
        case 32:
            return ir::ValueType::V32;
        case 64:
            return ir::ValueType::V64;
        case 128:
            return ir::ValueType::V128;
        case 256:
            return ir::ValueType::V256;
        default:
            PANIC();
            return ir::ValueType::VOID;
    }
}

ir::Uniform ToReg(const X86RegInfo& info) {
    u32 offset{};
    if (info.index >= X86RegInfo::Rax && info.index <= X86RegInfo::R15) {
        // gprs
        offset = offsetof(ThreadContext64, regs) + (info.index - X86RegInfo::Rax) * sizeof(Reg);
    } else if (info.index >= X86RegInfo::ES && info.index <= X86RegInfo::GS) {
        // segments
        offset = offsetof(ThreadContext64, segs) + (info.index - X86RegInfo::ES) * sizeof(Seg);
    } else if (info.index == X86RegInfo::Rip) {
        // pc
        offset = offsetof(ThreadContext64, pc);
    } else {
        PANIC("Invalid GPR {}!", info.index);
    }
    return ir::Uniform{offset, info.type};
}

ir::Uniform ToVReg(const X86RegInfo& info) {
    u32 offset{};
    if (info.index >= X86RegInfo::Xmm0 && info.index <= X86RegInfo::Xmm15) {
        offset = offsetof(ThreadContext64, xmms) + (info.index - X86RegInfo::Xmm0) * sizeof(Xmm);
    } else if (info.index >= X86RegInfo::Ymm0 && info.index <= X86RegInfo::Ymm15) {
        // AVX Regs
        offset = offsetof(ThreadContext64, xmms) + (info.index - X86RegInfo::Ymm0) * sizeof(Ymm);
    } else {
        PANIC("Invalid FPR {}!", info.index);
    }
    return ir::Uniform{offset, info.type};
}

X64Decoder::X64Decoder(VAddr start,
                       runtime::MemoryInterface* memory,
                       ir::Assembler* visitor,
                       bool is_64bit)
        : start(start), pc(start), assembler(visitor), memory(memory), is_64bit(is_64bit) {
    addr_mask = is_64bit ? UINT64_MAX : UINT32_MAX;
}

void X64Decoder::Decode() {
    pc = start;
    while (!end_decode) {
        auto code_ptr = reinterpret_cast<u8*>(memory->GetPointer(reinterpret_cast<void*>(pc)));
        if (!code_ptr) {
            Interrupt(InterruptReason::PAGE_FATAL);
            break;
        }
        _DInst insn = DisDecode(code_ptr, 0x10, is_64bit);
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
        end_decode = assembler->EndCommit();
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
            break;
        }
        case I_RETF: {
            auto ret_addr = Pop(_RegisterType::R_RIP, ir::ValueType::U64);
            __ SetLocation(ir::Lambda{ret_addr});
            __ PopRSB();
            __ Return();
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
        case I_XADD:
            DecodeAddSub(insn, false, true, true);
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
        case I_IMUL:
            DecodeMulDiv(insn, false, true);
            break;
        case I_IDIV:
            DecodeMulDiv(insn, true, true);
            break;
        case I_OR:
            DecodeOr(insn);
            break;
        case I_AND:
            DecodeAnd(insn, true);
            break;
        case I_TEST:
            DecodeAnd(insn, false);
            break;
        case I_XOR:
            DecodeXor(insn);
            break;
        case I_PUSH:
            DecodePush(insn);
            break;
        case I_POP:
            DecodePop(insn);
            break;
        case I_PUSHA:
            DecodePushA(insn);
            break;
        case I_POPA:
            DecodePopA(insn);
            break;
        default:
            return false;
    }
    return true;
}

ir::Value X64Decoder::R(_RegisterType reg) {
    ASSERT(reg <= _RegisterType::R_RIP);
    return __ LoadUniform(ToReg(x86_regs_table[reg]));
}

ir::Value X64Decoder::V(_RegisterType reg) {
    ASSERT(reg <= _RegisterType::R_YMM15);
    return __ LoadUniform(ToVReg(x86_regs_table[reg]));
}

void X64Decoder::R(_RegisterType reg, ir::Value value) {
    __ StoreUniform(ToReg(x86_regs_table[reg]), value);
}

void X64Decoder::V(_RegisterType reg, ir::Value value) {
    __ StoreUniform(ToVReg(x86_regs_table[reg]), value);
}

void X64Decoder::Interrupt(InterruptReason reason) {
    ir::Uniform uni_interrupt{offsetof(ThreadContext64, interrupt), ir::ValueType::U32};
    __ SetLocation(ir::Lambda{ir::Imm{pc}});
    __ StoreUniform(uni_interrupt, __ LoadImm(ir::Imm(static_cast<u32>(reason))));
    __ ReturnToHost();
}

ir::BOOL X64Decoder::CheckCond(Cond cond) {
    switch (cond) {
        case Cond::AL:
            return __ LoadImm(ir::Imm(true));
        case Cond::EQ:
            return __ TestFlags(ir::Flags::Zero);
        case Cond::NE:
            return __ TestNotFlags(ir::Flags::Zero);
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
                               ir::terminal::LinkBlock{then_.GetImm().Get()},
                               ir::terminal::LinkBlock{else_}});
    }
}

ir::Value X64Decoder::ToValue(const ir::DataClass& data) {
    return data.IsImm() ? __ LoadImm(data.imm) : data.value;
}

ir::DataClass X64Decoder::Src(_DInst& insn, _Operand& op) {
    ir::DataClass result{};
    switch (op.type) {
        case O_PC:
            result = ir::Imm(pc + insn.imm.sqword);
            break;
        case O_REG:
            if (op.index == R_RIP) {
                result = ir::Imm(pc + insn.imm.qword & addr_mask);
            } else if (IsV(static_cast<_RegisterType>(op.index))) {
                result = V(static_cast<_RegisterType>(op.index));
            } else {
                result = R(static_cast<_RegisterType>(op.index));
            }
            break;
        case O_IMM: {
            /* Special fix for negative sign extended immediates. */
            if ((insn.flags & FLAG_IMM_SIGNED) && (op.size == 8)) {
                if (insn.imm.sbyte < 0) {
                    result = ir::Imm{insn.imm.sbyte};
                    break;
                }
            }

            if (op.size == 64) {
                result = ir::Imm{insn.imm.qword};
            } else {
                result = ir::Imm{insn.imm.dword};
            }
            break;
        }
        case O_IMM1:
            result = ir::Imm{insn.imm.ex.i1};
            break;
        case O_IMM2:
            result = ir::Imm{insn.imm.ex.i2};
            break;
        case O_SMEM:
        case O_MEM:
        case O_DISP: {
            auto size = GetSize(op.size);
            auto address_operand = GetAddress(insn, op);
            result = __ LoadMemoryTSO(address_operand.ToIROperand()).SetType(size);
            break;
        }
        case O_PTR: {
            auto mem_segment = insn.imm.ptr.seg;
            auto seg_offset = insn.imm.ptr.off;
            auto address = (u32(mem_segment) << 4) + seg_offset;
            result = ir::Imm{address};
            break;
        }
        default:
            PANIC();
    }

    return result;
}

void X64Decoder::Dst(_DInst& insn, _Operand& operand, const ir::DataClass& data) {
    auto value = ToValue(data);
    switch (operand.type) {
        case O_REG:
            if (IsV(static_cast<_RegisterType>(operand.index))) {
                V(static_cast<_RegisterType>(operand.index), value);
            } else {
                R(static_cast<_RegisterType>(operand.index), value);
            }
            break;
        case O_DISP:
        case O_SMEM:
        case O_MEM: {
            auto address = GetAddress(insn, operand);
            __ StoreMemoryTSO(address.ToIROperand(), value);
            break;
        }
        default:
            PANIC();
    }
}

bool X64Decoder::IsV(_RegisterType reg) { return reg >= R_ST0; }

X64Decoder::Operand X64Decoder::GetAddress(_DInst& insn, _Operand& op) {
    Operand address_operand{};
    switch (op.type) {
        case O_SMEM: {
            auto segment = SEGMENT_GET(insn.segment);
            bool is_default = SEGMENT_IS_DEFAULT(insn.segment);
            switch (insn.opcode) {
                case I_MOVS:
                    is_default = false;
                    if (&op == &insn.ops[0]) segment = R_ES;
                    break;
                case I_CMPS:
                    is_default = false;
                    if (&op == &insn.ops[1]) segment = R_ES;
                    break;
                case I_INS:
                case I_LODS:
                case I_STOS:
                case I_SCAS:
                    is_default = false;
                    break;
            }

            address_operand.left = R(static_cast<_RegisterType>(op.index));

            if (!is_default && (segment != R_NONE)) {
                address_operand.right = R(static_cast<_RegisterType>(segment));
                address_operand.ext = 4;
            }

            if (insn.dispSize) {
                s64 disp = ForceCast<s64>(insn.disp);
                if (address_operand.right.Null() && !address_operand.ext) {
                    address_operand.right = ir::Imm(disp & addr_mask);
                } else {
                    ir::Imm imm{std::abs<s64>(disp) & addr_mask};
                    address_operand.left =
                            disp > 0 ? __ Add(address_operand.left.value, ir::Operand(imm))
                                     : __ Sub(address_operand.left.value, ir::Operand{imm});
                }
            }
            break;
        }
        case O_MEM: {
            if ((SEGMENT_GET(insn.segment) != R_NONE) && !SEGMENT_IS_DEFAULT(insn.segment)) {
                address_operand.left = __ LslImm(
                        R(static_cast<_RegisterType>(SEGMENT_GET(insn.segment))), ir::Imm(4u));
            }
            if (insn.base != R_NONE) {
                ASSERT(address_operand.left.Null());
                if (insn.base == R_RIP) {
                    address_operand.left = ir::Imm(pc & addr_mask);
                } else {
                    address_operand.left = R(static_cast<_RegisterType>(insn.base));
                }
                address_operand.right = R(static_cast<_RegisterType>(op.index));
            } else {
                address_operand.left = R(static_cast<_RegisterType>(op.index));
            }
            if (insn.scale != 0) {
                if (insn.scale == 2)
                    address_operand.ext = 1;
                else if (insn.scale == 4)
                    address_operand.ext = 2;
                else if (insn.scale == 8)
                    address_operand.ext = 3;
                else {
                    PANIC("Invalid scale");
                }
            }
            if (insn.dispSize && insn.disp) {
                s64 disp = ForceCast<s64>(insn.disp);
                if (address_operand.right.Null() && !address_operand.ext) {
                    address_operand.right = ir::Imm(disp & addr_mask);
                } else {
                    ir::Imm imm{std::abs<s64>(disp) & addr_mask};
                    address_operand.left =
                            disp > 0 ? __ Add(address_operand.left.value, ir::Operand(imm))
                                     : __ Sub(address_operand.left.value, ir::Operand{imm});
                }
            }
            break;
        }
        case O_DISP: {
            if ((SEGMENT_GET(insn.segment) != R_NONE) && !SEGMENT_IS_DEFAULT(insn.segment)) {
                auto segment = R(static_cast<_RegisterType>(SEGMENT_GET(insn.segment)));
                address_operand.left = __ LoadImm(ir::Imm(insn.disp & addr_mask));
                address_operand.right = segment;
                address_operand.ext = 4;
            } else {
                address_operand.left = ir::Imm(insn.disp & addr_mask);
            }
            break;
        }
        default:
            PANIC();
    }
    return address_operand;
}

void X64Decoder::DecodeMov(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto src = Src(insn, op1);
    Dst(insn, op0, src);
}

void X64Decoder::DecodeAddSub(_DInst& insn, bool sub, bool save_res, bool exchange) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = ToValue(Src(insn, op0));
    auto right = Src(insn, op1);

    auto result = sub ? __ Sub(left, ir::Operand{right}) : __ Add(left, ir::Operand{right});

    __ SaveFlags(result, ir::Flags::All);

    if (exchange) {
        Dst(insn, op1, left);
    }

    if (save_res) {
        Dst(insn, op0, result);
    }
}

void X64Decoder::DecodeCondJump(_DInst& insn, Cond cond) {
    auto& op0 = insn.ops[0];

    auto address = ir::Lambda{Src(insn, op0)};

    if (cond == Cond::AL) {
        if (!address.IsValue()) {
            __ SetLocation(address);
            __ ReturnToDispatcher();
        } else {
            __ LinkBlock(ir::terminal::LinkBlock{address.GetImm().Get()});
        }
    } else {
        auto check_result = CheckCond(cond);
        CondGoto(check_result, address, pc);
    }
}

void X64Decoder::DecodeZeroCheckJump(_DInst& insn, _RegisterType reg) {
    auto& op0 = insn.ops[0];
    auto value_check = R(reg);
    auto address = Src(insn, op0);

    CondGoto(__ Not(value_check), address, pc);
}

void X64Decoder::DecodeAddSubWithCarry(_DInst& insn, bool sub) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = ToValue(Src(insn, op0));
    auto right = Src(insn, op1);

    auto result = sub ? __ Sub(left, ir::Operand{right}) : __ Adc(left, ir::Operand{right});

    __ SaveFlags(result, ir::Flags::All);

    Dst(insn, op0, result);
}

void X64Decoder::DecodeIncAndDec(_DInst& insn, bool dec) {
    auto& op0 = insn.ops[0];
    auto src = ToValue(Src(insn, op0));
    auto result =
            dec ? __ Sub(src, ir::Operand{ir::Imm(1u)}) : __ Add(src, ir::Operand{ir::Imm(1u)});

    __ SaveFlags(result,
                 ir::Flags::Overflow | ir::Flags::Negate | ir::Flags::Parity | ir::Flags::Zero |
                         ir::Flags::AuxiliaryCarry);

    Dst(insn, op0, result);
}

void X64Decoder::DecodeMulDiv(_DInst& insn, bool div, bool sign) {
    auto& op0 = insn.ops[0];
    auto left = ToValue(Src(insn, op0));
    auto right = R(_RegisterType::R_RAX);

    if (sign && op0.size != 64) {
        left = __ SignExtend(left).SetType(GetSignedSize(op0.size));
        right = __ SignExtend(right).SetType(GetSignedSize(op0.size));
    }

    auto result = div ? __ Div(left, ir::Operand{right}) : __ Mul(left, ir::Operand{right});

    __ SaveFlags(result, ir::Flags::Overflow | ir::Flags::Carry);

    Dst(insn, op0, result);
}

void X64Decoder::DecodeLea(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto address = GetAddress(insn, op1);
    Dst(insn, op0, __ GetOperand(address.ToIROperand()));
}

void X64Decoder::DecodeCondMov(_DInst& insn, Cond cond) {
    auto check_result = CheckCond(cond);
    auto label = __ NotGoto(check_result);
    DecodeMov(insn);
    __ BindLabel(label);
}

void X64Decoder::DecodeAnd(_DInst& insn, bool save_result) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = ToValue(Src(insn, op0));
    auto right = Src(insn, op1);

    auto result = __ And(left, ir::Operand{right});

    __ SaveFlags(result,
                 ir::Flags::Zero | ir::Flags::Negate | ir::Flags::Carry | ir::Flags::Overflow |
                         ir::Flags::Parity);

    if (save_result) {
        Dst(insn, op0, result);
    }
}

ir::Value X64Decoder::Pop(_RegisterType reg, ir::ValueType type) {
    auto size_byte = ir::GetValueSizeByte(type);
    auto sp = _RegisterType::R_RSP;
    auto address = R(sp);
    auto value = __ LoadMemory(ir::Operand{address}).SetType(type);
    if (IsV(reg)) {
        V(reg, value);
    } else {
        R(reg, value);
    }
    R(sp, __ Add(address, ir::Operand{ir::Imm(size_byte)}));
    return value;
}

void X64Decoder::Push(ir::Value value) {
    auto size = value.Type();
    auto size_byte = ir::GetValueSizeByte(size);
    auto sp = _RegisterType::R_RSP;
    auto address = R(sp);
    __ StoreMemory(ir::Operand{address}, value);
    R(sp, __ Sub(address, ir::Operand{size_byte}));
}

void X64Decoder::DecodeOr(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = Src(insn, op0);
    auto right = Src(insn, op1);

    auto result = __ Or(ToValue(left), ir::Operand{right});
    __ ClearFlags(ir::Flags::Carry | ir::Flags::Overflow | ir::Flags::AuxiliaryCarry);
    __ SaveFlags(result, ir::Flags::Negate | ir::Flags::Parity | ir::Flags::Zero);

    Dst(insn, op0, result);
}

void X64Decoder::DecodeXor(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = Src(insn, op0);
    auto right = Src(insn, op1);

    auto result = __ Xor(ToValue(left), ir::Operand{right});
    __ ClearFlags(ir::Flags::Carry | ir::Flags::Overflow | ir::Flags::AuxiliaryCarry);
    __ SaveFlags(result, ir::Flags::Negate | ir::Flags::Parity | ir::Flags::Zero);

    Dst(insn, op0, result);
}

void X64Decoder::DecodePush(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto value = Src(insn, op0);
    Push(ToValue(value));
}

void X64Decoder::DecodePop(_DInst& insn) {
    auto& op0 = insn.ops[0];
    Pop(static_cast<_RegisterType>(op0.index), GetSize(op0.size));
}

void X64Decoder::DecodePushA(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto type = GetSize(op0.size);
    ASSERT(type == runtime::ir::ValueType::U32);
    auto sp = R(_RegisterType::R_ESP);
    __ StoreMemory(ir::Operand{sp}, R(R_EAX));
    __ StoreMemory(ir::Operand{sp, -4, ir::OperandPlus}, R(R_ECX));
    __ StoreMemory(ir::Operand{sp, -8, ir::OperandPlus}, R(R_EDX));
    __ StoreMemory(ir::Operand{sp, -12, ir::OperandPlus}, R(R_EBX));
    __ StoreMemory(ir::Operand{sp, -16, ir::OperandPlus}, R(R_ESP));
    __ StoreMemory(ir::Operand{sp, -20, ir::OperandPlus}, R(R_EBP));
    __ StoreMemory(ir::Operand{sp, -24, ir::OperandPlus}, R(R_RSI));
    __ StoreMemory(ir::Operand{sp, -28, ir::OperandPlus}, R(R_RDI));
    auto new_sp = __ Sub(sp, ir::Operand{28u});
    R(_RegisterType::R_ESP, new_sp);
}

void X64Decoder::DecodePopA(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto type = GetSize(op0.size);
    ASSERT(type == runtime::ir::ValueType::U32);
    auto sp = R(_RegisterType::R_ESP);
    auto eax = __ LoadMemory(ir::Operand{sp});
    R(R_EAX, eax);
    auto ecx = __ LoadMemory(ir::Operand{sp, 4u});
    R(R_ECX, ecx);
    auto edx = __ LoadMemory(ir::Operand{sp, 8u});
    R(R_EDX, edx);
    auto ebx = __ LoadMemory(ir::Operand{sp, 16u});
    R(R_EBX, ebx);
    auto ebp = __ LoadMemory(ir::Operand{sp, 20u});
    R(R_EBP, ebp);
    auto esi = __ LoadMemory(ir::Operand{sp, 24u});
    R(R_ESI, esi);
    auto edi = __ LoadMemory(ir::Operand{sp, 28u});
    R(R_EDI, edi);
    auto new_sp = __ Add(sp, ir::Operand{28u});
    R(_RegisterType::R_ESP, new_sp);
}

void X64Decoder::DecodeShlShr(_DInst& insn, bool shr) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = ToValue(Src(insn, op0));
    auto right = ToValue(Src(insn, op1));

    auto result = shr ? __ LsrValue(left, right) : __ LslValue(left, right);
    __ ClearFlags(ir::Flags::Carry | ir::Flags::Overflow | ir::Flags::AuxiliaryCarry);
    __ SaveFlags(result, ir::Flags::Negate | ir::Flags::Parity | ir::Flags::Zero);
    Dst(insn, op0, result);
}

void X64Decoder::DecodeCmp(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = ToValue(Src(insn, op0));
    auto right = Src(insn, op1);

    auto result = __ Sub(left, ir::Operand{right});
    __ SaveFlags(result, ir::Flags::All);
}

void X64Decoder::DecodeAndNot(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = ToValue(Src(insn, op0));
    auto right = Src(insn, op1);

    auto result = __ AndNot(left, ir::Operand{right});
    __ ClearFlags(ir::Flags::Carry | ir::Flags::Overflow | ir::Flags::All);
    __ SaveFlags(result, ir::Flags::Negate | ir::Flags::Parity | ir::Flags::Zero);

    Dst(insn, op0, result);
}

}  // namespace swift::x86
