#include "runtime/frontend/x86/decoder_internal.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

void X64Decoder::DecodeCondJump(_DInst& insn, Cond cond) {
    auto& op0 = insn.ops[0];

    auto address = ir::Lambda{Src(insn, op0)};

    if (cond == Cond::AL) {
        // Direct: constant target, indirect (reg / mem): value target. Both
        // terminate the block and hand the target back to the dispatcher.
        __ SetLocation(address);
        __ ReturnToDispatcher();
    } else {
        auto check_result = CheckCond(cond);
        CondGoto(check_result, address, pc);
    }
}

void X64Decoder::DecodeZeroCheckJump(_DInst& insn, _RegisterType reg) {
    auto& op0 = insn.ops[0];
    auto value_check = R(reg);
    auto address = Src(insn, op0);

    CondGoto(__ TestZero(value_check), address, pc);
}

void X64Decoder::DecodeSetCC(_DInst& insn, Cond cond) {
    auto check_result = CheckCond(cond);
    auto one = __ LoadImm(ir::Imm(u8(1)));
    auto zero = __ LoadImm(ir::Imm(u8(0)));
    auto result = __ Select(check_result, one, zero);
    Dst(insn, insn.ops[0], result);
}

void X64Decoder::DecodeCondMov(_DInst& insn, Cond cond) {
    // 32 bit cmov clears the upper half of the destination even when the
    // condition is false (modern x86 behaviour, and what Unicorn models), so
    // this cannot be a skip-around branch: select between source and the
    // current destination and let Dst apply the usual width rules.
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    auto check_result = CheckCond(cond);
    auto dst_val = ToValue(Src(insn, op0));
    auto src_val = ToValue(Src(insn, op1));
    auto result = __ Select(check_result, src_val, dst_val).SetType(GetSize(op0.size));
    Dst(insn, op0, result);
}

ir::Value X64Decoder::Pop(ir::ValueType type) {
    auto size_byte = ir::GetValueSizeByte(type);
    auto sp = _RegisterType::R_RSP;
    auto address = R(sp);
    // Stack accesses stay Relaxed even in AcqRel mode: each guest thread owns
    // its stack, so no cross-thread ordering is observable (cross86 does the
    // same relaxation for RSP/RBP-relative accesses).
    auto value = __ LoadMemory(ir::Operand{address}).SetType(type);
    R(sp, __ Add(address, ir::Operand{ir::Imm(u64(size_byte))}));
    return value;
}

void X64Decoder::Push(ir::Value value, ir::ValueType type) {
    auto size_byte = ir::GetValueSizeByte(type);
    auto sp = _RegisterType::R_RSP;
    auto address = R(sp);
    auto new_sp = __ Sub(address, ir::Operand{ir::Imm(u64(size_byte))});
    __ StoreMemory(ir::Operand{new_sp}, value.SetType(type));
    R(sp, new_sp);
}

void X64Decoder::DecodePush(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto width = op0.size == 16 ? 16 : (is_64bit ? 64 : 32);
    auto type = GetSize(width);
    if (op0.type == O_IMM || op0.type == O_IMM1) {
        // distorm keeps the immediate sign extended in imm.sqword.
        auto value = __ LoadImm(ir::Imm(insn.imm.sqword)).SetType(GetSignedContainer(width));
        Push(value, type);
        return;
    }
    auto value = ToValue(Src(insn, op0));
    Push(value, type);
}

void X64Decoder::DecodePop(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto width = op0.size == 16 ? 16 : (is_64bit ? 64 : 32);
    auto value = Pop(GetSize(width));
    Dst(insn, op0, value);
}

void X64Decoder::DecodePushA(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto type = GetSize(op0.size);
    ASSERT(type == runtime::ir::ValueType::U32);
    auto sp = R(_RegisterType::R_ESP);
    __ StoreMemory(ir::Operand{sp, -4, ir::OperandPlus}, R(R_EAX));
    __ StoreMemory(ir::Operand{sp, -8, ir::OperandPlus}, R(R_ECX));
    __ StoreMemory(ir::Operand{sp, -12, ir::OperandPlus}, R(R_EDX));
    __ StoreMemory(ir::Operand{sp, -16, ir::OperandPlus}, R(R_EBX));
    __ StoreMemory(ir::Operand{sp, -20, ir::OperandPlus}, R(R_ESP));
    __ StoreMemory(ir::Operand{sp, -24, ir::OperandPlus}, R(R_EBP));
    __ StoreMemory(ir::Operand{sp, -28, ir::OperandPlus}, R(R_RSI));
    __ StoreMemory(ir::Operand{sp, -32, ir::OperandPlus}, R(R_RDI));
    auto new_sp = __ Sub(sp, ir::Operand{32u});
    R(_RegisterType::R_ESP, new_sp);
}

void X64Decoder::DecodePopA(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto type = GetSize(op0.size);
    ASSERT(type == runtime::ir::ValueType::U32);
    auto sp = R(_RegisterType::R_ESP);
    auto edi = __ LoadMemory(ir::Operand{sp});
    R(R_EDI, edi);
    auto esi = __ LoadMemory(ir::Operand{sp, 4u});
    R(R_ESI, esi);
    auto ebp = __ LoadMemory(ir::Operand{sp, 8u});
    R(R_EBP, ebp);
    auto ebx = __ LoadMemory(ir::Operand{sp, 16u});
    R(R_EBX, ebx);
    auto edx = __ LoadMemory(ir::Operand{sp, 20u});
    R(R_EDX, edx);
    auto ecx = __ LoadMemory(ir::Operand{sp, 24u});
    R(R_ECX, ecx);
    auto eax = __ LoadMemory(ir::Operand{sp, 28u});
    R(R_EAX, eax);
    auto new_sp = __ Add(sp, ir::Operand{32u});
    R(_RegisterType::R_ESP, new_sp);
}

void X64Decoder::DecodeLoop(_DInst& insn) {
    // loop/loopz/loopnz: decrement RCX/ECX, jump rel8 if RCX != 0 (and ZF condition).
    auto cx_reg = is_64bit ? _RegisterType::R_RCX : _RegisterType::R_ECX;
    auto cx_dec = __ Sub(R(cx_reg), ir::Operand{ir::Imm(u64(1))});
    R(cx_reg, cx_dec);
    ir::BOOL take = __ TestNotZero(cx_dec);
    if (insn.opcode != I_LOOP) {
        auto zf = CheckCond(Cond::EQ);
        take = insn.opcode == I_LOOPZ
                     ? __ And(take, ir::Operand{zf})
                     : __ And(take, ir::Operand{
                               __ Xor(zf, ir::Operand{ir::Imm(u64(1))}).SetType(ir::ValueType::U8)});
    }
    auto skip = __ NotGoto(take);
    __ SetLocation(ir::Lambda{ir::Imm{u64(pc + insn.imm.sqword)}});
    __ Return();
    __ BindLabel(skip);
}

void X64Decoder::DecodeEnter(_DInst& insn) {
    // ENTER imm16, imm8: push rbp; mov rbp, rsp; sub rsp, imm16.
    // Level > 0 (nested frames) approximated as level 0 (rare in practice).
    u64 alloc_size = insn.imm.ex.i1;
    auto bp_reg = is_64bit ? _RegisterType::R_RBP : _RegisterType::R_EBP;
    auto sp_reg = is_64bit ? _RegisterType::R_RSP : _RegisterType::R_ESP;
    auto ptr_type = is_64bit ? ir::ValueType::U64 : ir::ValueType::U32;
    Push(R(bp_reg), ptr_type);
    R(bp_reg, R(sp_reg));
    if (alloc_size > 0) {
        R(sp_reg, __ Sub(R(sp_reg), ir::Operand{ir::Imm(alloc_size)}));
    }
}

}  // namespace swift::x86
