#include "translator.h"
#include <stdexcept>
#include "helpers.h"
#include "x87_helper.h"

namespace swift::runtime::backend::x64 {
using O = ir::OpCode;
using namespace Xbyak;

void Translator::EmitNop(ir::Inst*) {}

void Translator::EmitUniformBarrier(ir::Inst* inst) { EmitNop(inst); }

void Translator::EmitXchgBarrier(ir::Inst* inst) { EmitNop(inst); }

void Translator::EmitBranchOnlyEdges(ir::Inst* inst) { EmitNop(inst); }

void Translator::EmitPushRSB(ir::Inst* inst) { EmitNop(inst); }

void Translator::EmitPopRSB(ir::Inst* inst) { EmitNop(inst); }

void Translator::EmitCallReturn(ir::Inst* inst) { EmitNop(inst); }

void Translator::EmitAdvancePC(ir::Inst* inst) {
    guest_pc += Immediate(inst, 0); location_dirty = location_from_metadata = true; return;
}

void Translator::EmitSetLocation(ir::Inst* inst) {
    const auto lambda = inst->GetArg<ir::Lambda>(0);
    if (lambda.IsValue()) Read(rax, lambda.GetValue()); else mov(rax, lambda.GetImm().Get());
    mov(qword[r12 + state_offset_current_loc], rax);
    location_dirty = location_from_metadata = false; return;
}

void Translator::EmitGetLocation(ir::Inst* inst) {
    mov(rax, qword[r12 + state_offset_current_loc]); Write(inst, rax); return;
}

void Translator::EmitSelectValue(ir::Inst* inst, ir::OpCode op) {
    if (op == O::CondSelect) Condition(inst->GetArg<ir::Cond>(0)); else ReadArgument(rax, inst, 0);
    Label alternative, done; test(rax, rax);
    if (op == O::SelectZero) jnz(alternative); else jz(alternative);
    ReadVector(xmm0, inst->GetArg<ir::Value>(1)); jmp(done);
    L(alternative); ReadVector(xmm0, inst->GetArg<ir::Value>(2)); L(done);
    WriteVector(inst, xmm0); return;
}

void Translator::EmitSelect(ir::Inst* inst) { EmitSelectValue(inst, O::Select); }

void Translator::EmitSelectZero(ir::Inst* inst) { EmitSelectValue(inst, O::SelectZero); }

void Translator::EmitCondSelect(ir::Inst* inst) { EmitSelectValue(inst, O::CondSelect); }

void Translator::EmitConditionalGoto(ir::Inst* inst, ir::OpCode op) {
    cached_vector = nullptr;
    auto* target = BranchTarget(inst);
    if (!FallsThrough(inst)) {
        if (auto edge = taken_phis.find(inst); edge != taken_phis.end()) EmitPhiMoves(edge->second);
        CommitGuestEdge(inst);
        jmp(labels.at(target), T_NEAR);
        return;
    }
    if (execution_mode == ExecutionMode::Ordinary && !guest_instructions && !function_plan) {
        PublishLocation(); PollInterrupt();
    }
    ReadArgument(rax, inst, 0); test(rax, rax);
    Label skip;
    if (op == O::Goto) jz(skip, T_NEAR); else jnz(skip, T_NEAR);
    if (auto edge = taken_phis.find(inst); edge != taken_phis.end()) EmitPhiMoves(edge->second);
    CommitGuestEdge(inst);
    jmp(labels.at(target), T_NEAR);
    L(skip);
}

void Translator::EmitGoto(ir::Inst* inst) { EmitConditionalGoto(inst, O::Goto); }

void Translator::EmitNotGoto(ir::Inst* inst) { EmitConditionalGoto(inst, O::NotGoto); }

void Translator::EmitBindLabel(ir::Inst* inst) {
    cached_vector = nullptr;
    L(labels.at(inst->GetArg<ir::Value>(0).Def()));
    if (function_plan && function_plan->locations.contains(inst)) {
        guest_pc = function_plan->locations.at(inst).Value();
        location_dirty = location_from_metadata = false;
        PollInterrupt();
    }
    // A jump may skip the earlier store that published this PC.
    location_dirty |= location_from_metadata; return;
}

void Translator::EmitX87Op(ir::Inst* inst) {
    const auto command = ValidateX87RegisterOp(inst);
    PublishLocation();
    mov(rdi, r12); ReadArgument(rsi, inst, 0); mov(rdx, command);
    Call(reinterpret_cast<u64>(&EvaluateX87Register));
    CheckHalt(false);
    Label valid;
    bt(rax, 63); jnc(valid);
    Return(HaltReason::PageFatal);
    L(valid);
    // Inspect the full helper result before FNSTSW's optional U16
    // write masks it; fault reporting must never lose bit 63.
    Write(inst, rax); CheckHalt(); return;
}

void Translator::EmitCallLambda(ir::Inst* inst) {
    PublishLocation();
    const auto lambda = inst->GetArg<ir::Lambda>(0);
    if (lambda.IsValue()) Read(r10, lambda.GetValue()); else mov(r10, lambda.GetImm().Get());
    const Reg64 regs[]{rdi, rsi, rdx};
    for (u32 index = 1; index < 4; ++index) {
        if (inst->ArgAt(index).IsValue()) ReadArgument(regs[index - 1], inst, index);
        else xor_(regs[index - 1], regs[index - 1]);
    }
    call(r10);
    CheckHalt(false);
    Write(inst, rax); CheckHalt(); return;
}

void Translator::EmitCallDynamic(ir::Inst* inst) {
    PublishLocation();
    const auto lambda = inst->GetArg<ir::Lambda>(0);
    if (lambda.IsValue()) Read(r10, lambda.GetValue()); else mov(r10, lambda.GetImm().Get());
    const Reg64 regs[]{rdi, rsi, rdx, rcx, r8, r9};
    u32 index{};
    for (const auto& param : inst->GetArg<ir::Params>(1)) {
        if (index >= 6) throw std::runtime_error("x64 JIT host calls support at most six integer arguments");
        Data(regs[index++], param.data);
    }
    call(r10); CheckHalt(false);
    Write(inst, rax); CheckHalt(); return;
}

void Translator::EmitCallLocation(ir::Inst* inst) { EmitCallDynamic(inst); }

void Translator::EmitAddPhi(ir::Inst*) {
    // Incoming edges already assigned the destination in parallel.
}

}  // namespace swift::runtime::backend::x64
