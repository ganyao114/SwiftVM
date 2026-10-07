#include "translator.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/riscv64/defines.h"
#include "runtime/common/backedge_control.h"
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;
using O = ir::OpCode;

namespace {

constexpr std::array saved_fprs{fs0, fs1, fs2, fs3, fs4, fs5, fs6, fs7, fs8, fs9, fs10, fs11};

u64 MemoryAddress(State* state, u64 guest, u64 size);

void ExecuteInstruction(State* state, ir::Block* block, ir::Inst* inst,
                        u64* slots, u64 count) noexcept {
    try {
        const auto scalar = [&](ir::Value value) { return slots[size_t(value.Def()->Id()) * 2]; };
        const auto lambda = [&](const ir::Lambda& value) {
            return value.IsValue() ? scalar(value.GetValue()) : value.GetImm().Get();
        };
        // Atomic and copy interpreter entrypoints assume a validated pointer.
        // Validate the complete guest range before any lock or memcpy begins.
        switch (inst->GetOp()) {
            case O::CompareAndSwap128:
                MemoryAddress(state, scalar(inst->GetArg<ir::Value>(0)), 16); break;
            case O::CompareAndSwap: case O::AtomicExchange: case O::AtomicFetchAdd:
                MemoryAddress(state, scalar(inst->GetArg<ir::Value>(0)),
                              ir::GetValueSizeByte(inst->GetArg<ir::Value>(1).Type())); break;
            case O::AtomicRMW:
                MemoryAddress(state, scalar(inst->GetArg<ir::Value>(1)),
                              ir::GetValueSizeByte(inst->GetArg<ir::Value>(2).Type())); break;
            case O::MemoryCopy: case O::MemoryCopyTSO: {
                const auto size = inst->GetArg<ir::Imm>(2).Get();
                if (size) {
                    MemoryAddress(state, lambda(inst->GetArg<ir::Lambda>(0)), size);
                    MemoryAddress(state, lambda(inst->GetArg<ir::Lambda>(1)), size);
                }
                break;
            }
            default: break;
        }
        if (state->halt_reason != HaltReason::None) return;
        interp::InterpStack stack{slots, static_cast<size_t>(count)};
        interp::Interpreter interpreter{*state, block};
        interpreter.Run(inst, stack);
    } catch (...) {
        // Generated frames have no C++ unwind tables. Never let a helper's
        // exception cross them; return through the normal block epilogue.
        state->halt_reason = HaltReason::IllegalCode;
    }
}

// Validate before native accesses, including range-end overflow and bounded
// windows. The actual access stays in generated code, with a fault recovery
// entry for PROT_NONE / protection faults that mapping checks cannot predict.
u64 MemoryAddress(State* state, u64 guest, u64 size) {
    guest &= state->guest_addr_mask;
    if (!size || guest >= state->guest_addr_limit ||
        size > state->guest_addr_limit - guest ||
        (state->guest_addr_mask != UINT64_MAX && size - 1 > state->guest_addr_mask - guest) ||
        (state->interp_range_check &&
         !state->interp_range_check(state->interp_range_check_ctx, guest, size))) {
        state->halt_reason = HaltReason::PageFatal;
        return 0;
    }
    return guest + reinterpret_cast<u64>(state->pt);
}

u32 Bits(ir::ValueType type) {
    return ir::GetValueSizeByte(type) * 8;
}

}  // namespace

void JitTranslator::Translate(ir::Block* input) {
    block = input;
    context.DiscardValues();
    context.EnsureSpace();
    // Helpers refer to this IR for the lifetime of its code allocation.
    // Reject ARM64 register-rewritten IR instead of silently yielding zero.
    for (auto& inst : block->GetInstList()) {
        if (inst.ReturnType() != ir::ValueType::VOID && Bits(inst.ReturnType()) > 128)
            throw std::runtime_error("RV64 semantic slots support up to V128; V256 must be split by the frontend");
        if (inst.GetOp() == O::GetHostGPR || inst.GetOp() == O::SetHostGPR ||
            inst.GetOp() == O::GetHostFPR || inst.GetOp() == O::SetHostFPR ||
            inst.GetOp() == O::AddPhi)
            throw std::runtime_error("RV64 requires canonical block IR without host-register rewrites or phi nodes");
        slot_count = std::max(slot_count, u32(inst.Id()) + 1);
        if (inst.GetOp() == O::BindLabel)
            if (!labels.try_emplace(inst.GetArg<ir::Value>(0).Def()).second)
                throw std::runtime_error("RV64 goto has multiple bound targets");
    }
    for (auto& inst : block->GetInstList()) {
        if ((inst.GetOp() == O::Goto || inst.GetOp() == O::NotGoto) && !labels.contains(&inst))
            throw std::runtime_error("RV64 goto has no bound target");
    }
    slot_count = std::max(slot_count, u32{1});
    auto& as = context.GetMasm();
    as.ADDI(sp, sp, -static_cast<s32>(kBlockSavedFrameSize));
    as.SD(ra, 0, sp);
    as.SD(frame, 8, sp);
    as.SD(state, 16, sp);
    as.SD(values, 24, sp);
    for (size_t i = 0; i < scalar_registers.size(); ++i) as.SD(scalar_registers[i], 32 + i * 8, sp);
    for (size_t i = 0; i < saved_fprs.size(); ++i) as.FSD(saved_fprs[i], 104 + i * 8, sp);
    as.MV(frame, sp);
    as.MV(state, a0);
    as.LI(t0, u64(slot_count) * kValueStride);
    as.SUB(sp, sp, t0);
    as.MV(values, sp);
    // Clear high halves too: scalar -> vector BitCast must see deterministic
    // zeros, and pair-result helpers may define a later pseudo's home.
    as.MV(t1, values);
    Label clear;
    as.Bind(&clear);
    as.SD(x0, 0, t1);
    as.ADDI(t1, t1, 8);
    as.ADDI(t0, t0, -8);
    as.BNE(t0, x0, &clear);
    as.LILabel(t0, &epilogue);
    context.Store(t0, state, state_offset_riscv_recovery_pc);
    context.Store(frame, state, state_offset_riscv_recovery_frame);
    Poll();

    for (auto& inst : block->GetInstList()) {
        context.EnsureSpace();
        if (inst.GetOp() == O::BindLabel) {
            auto& label = labels.at(inst.GetArg<ir::Value>(0).Def());
            context.FlushValues();
            as.Bind(&label);
            Poll();
        } else if (inst.GetOp() == O::Goto || inst.GetOp() == O::NotGoto) {
            auto target = labels.find(&inst);
            if (target == labels.end()) throw std::runtime_error("RV64 goto has no bound target");
            context.Read(t0, inst.GetArg<ir::Value>(0));
            context.FlushValues();
            context.BranchZero(t0, target->second, inst.GetOp() == O::NotGoto);
        } else if (EmitScalar(&inst)) {
            ++stats.direct;
        } else {
            EmitHelper(&inst);
            ++stats.helpers;
        }
    }
    EmitTerminal(block->GetTerminal());
    context.EnsureSpace();
    recovery_offset = context.CurrentBufferSize();
    as.Bind(&epilogue);
    context.Load(a0, state, state_offset_halt_reason, 4);
    context.Store(x0, state, state_offset_riscv_recovery_pc);
    context.Store(x0, state, state_offset_riscv_recovery_frame);
    as.MV(sp, frame);
    as.LD(ra, 0, sp);
    as.LD(frame, 8, sp);
    as.LD(state, 16, sp);
    as.LD(values, 24, sp);
    for (size_t i = 0; i < scalar_registers.size(); ++i) as.LD(scalar_registers[i], 32 + i * 8, sp);
    for (size_t i = 0; i < saved_fprs.size(); ++i) as.FLD(saved_fprs[i], 104 + i * 8, sp);
    as.ADDI(sp, sp, kBlockSavedFrameSize);
    as.RET();
}

void JitTranslator::Poll() {
    auto& as = context.GetMasm();
    context.Load(t0, state, state_offset_exit_request);
    as.FENCE(FenceOrder::R, FenceOrder::RW);
    Label resume;
    as.BEQ(t0, x0, &resume);
    as.SRLI(t0, t0, 63);
    as.LI(t1, static_cast<u32>(HaltReason::CodeMiss));
    Label smc;
    as.BEQ(t0, x0, &smc);
    as.LI(t1, static_cast<u32>(HaltReason::Signal));
    as.Bind(&smc);
    context.Store(t1, state, state_offset_halt_reason, 4);
    context.Jump(epilogue);
    as.Bind(&resume);
}

void JitTranslator::Return(HaltReason reason) {
    auto& as = context.GetMasm();
    if (reason != HaltReason::None) {
        as.LI(t0, static_cast<u32>(reason));
        context.Store(t0, state, state_offset_halt_reason, 4);
    }
    context.Jump(epilogue);
}

void JitTranslator::EmitHelper(ir::Inst* inst) {
    // Helpers read canonical homes and can write pair-result pseudo homes.
    context.FlushValues();
    auto& as = context.GetMasm();
    const bool ordered = inst->GetOp() == O::LoadMemoryTSO ||
            inst->GetOp() == O::StoreMemoryTSO || inst->GetOp() == O::MemoryCopyTSO;
    // The interpreter's TSO entrypoints provide value semantics only. When
    // used as native RV64 helpers they must participate in guest ordering.
    if (ordered) as.FENCE();
    as.MV(a0, state);
    as.LI(a1, reinterpret_cast<u64>(block));
    as.LI(a2, reinterpret_cast<u64>(inst));
    as.MV(a3, values);
    as.LI(a4, u64(slot_count) * 2);
    as.LI(t0, reinterpret_cast<u64>(&ExecuteInstruction));
    as.JALR(t0);
    if (ordered) as.FENCE();
    context.Load(t0, state, state_offset_halt_reason, 4);
    context.BranchZero(t0, epilogue, false);
}

bool JitTranslator::EmitScalar(ir::Inst* inst) {
    auto& as = context.GetMasm();
    const auto op = inst->GetOp();
    if (ir::IsFloatValueType(inst->ReturnType())) return false;
    const auto result = context.ResultRegister(inst);
    switch (op) {
        case O::Nop: case O::AdvancePC: case O::UniformBarrier:
        case O::XchgBarrier: case O::BranchOnlyEdges:
        case O::PushRSB: case O::PopRSB: case O::CallReturn:
            return true;
        case O::LoadImm: as.LI(result, inst->GetArg<ir::Imm>(0).Get()); break;
        case O::Zero: as.MV(result, x0); break;
        case O::GetLocation: context.Load(result, state, state_offset_current_loc); break;
        case O::SetLocation: {
            auto target = inst->GetArg<ir::Lambda>(0);
            if (target.IsValue()) context.Read(result, target.GetValue());
            else as.LI(result, target.GetImm().Get());
            context.Store(result, state, state_offset_current_loc);
            return true;
        }
        case O::GetUniformAddress:
            context.Address(result, state, state_offset_uniform_buffer + inst->GetArg<ir::Imm>(0).Get());
            break;
        case O::LoadUniform: {
            auto uniform = inst->GetArg<ir::Uniform>(0);
            context.Load(result, state, state_offset_uniform_buffer + uniform.GetOffset(),
                         ir::GetValueSizeByte(inst->ReturnType()));
            break;
        }
        case O::StoreUniform: {
            auto value = inst->GetArg<ir::Value>(1);
            if (ir::IsFloatValueType(value.Type())) return false;
            context.Read(result, value);
            context.Store(result, state, state_offset_uniform_buffer + inst->GetArg<ir::Uniform>(0).GetOffset(),
                          ir::GetValueSizeByte(value.Type()));
            return true;
        }
        case O::LoadMemory: case O::LoadMemoryTSO:
            EmitMemory(inst, false, op == O::LoadMemoryTSO, result); return true;
        case O::StoreMemory: case O::StoreMemoryTSO:
            if (ir::IsFloatValueType(inst->GetArg<ir::Value>(1).Type())) return false;
            EmitMemory(inst, true, op == O::StoreMemoryTSO, result); return true;
        case O::MemoryBarrierTSO: as.FENCE(); return true;
        case O::GetOperand: context.Operand(result, inst->GetArg<ir::Operand>(0)); break;
        case O::BitCast: case O::GetResult:
            if (ir::IsFloatValueType(inst->GetArg<ir::Value>(0).Type())) return false;
            context.Read(result, inst->GetArg<ir::Value>(0)); break;
        case O::ZeroExtend32: case O::ZeroExtend32To64: case O::ZeroExtend64:
        case O::SignExtend: case O::Neg: case O::TestZero: case O::TestNotZero:
        case O::TestBit:
            context.Read(result, inst->GetArg<ir::Value>(0));
            if (op == O::ZeroExtend32 || op == O::ZeroExtend32To64) context.Mask(result, 32);
            if (op == O::SignExtend) context.SignExtend(result, Bits(inst->GetArg<ir::Value>(0).Type()));
            if (op == O::Neg) as.SUB(result, x0, result);
            if (op == O::TestZero) as.SEQZ(result, result);
            if (op == O::TestNotZero) as.SNEZ(result, result);
            if (op == O::TestBit) {
                as.SRLI(result, result, inst->GetArg<ir::Imm>(1).Get() & 63);
                as.ANDI(result, result, 1);
            }
            break;
        case O::CondSet: case O::LocalCondSet:
            context.Condition(result, inst->GetArg<ir::Cond>(0)); break;
        case O::Select: case O::SelectZero: case O::CondSelect: {
            if (ir::IsFloatValueType(inst->GetArg<ir::Value>(1).Type())) return false;
            if (op == O::CondSelect) context.Condition(result, inst->GetArg<ir::Cond>(0));
            else context.Read(result, inst->GetArg<ir::Value>(0));
            Label other, done;
            if (op == O::SelectZero) as.BNE(result, x0, &other);
            else as.BEQ(result, x0, &other);
            context.Read(result, inst->GetArg<ir::Value>(1));
            as.J(&done);
            as.Bind(&other);
            context.Read(result, inst->GetArg<ir::Value>(2));
            as.Bind(&done);
            break;
        }
        case O::Add: case O::Sub: case O::Adc: case O::Sbb:
        case O::Mul: case O::Div: case O::And: case O::Or:
        case O::Xor: case O::AndNot:
            context.Read(result, inst->GetArg<ir::Value>(0));
            context.Operand(t1, inst->GetArg<ir::Operand>(1));
            if (op == O::Add || op == O::Adc) as.ADD(result, result, t1);
            if (op == O::Sub || op == O::Sbb) as.SUB(result, result, t1);
            if (op == O::Adc || op == O::Sbb) {
                context.Load(t2, state, state_offset_host_flags);
                as.SRLI(t2, t2, kCarryBit); as.ANDI(t2, t2, 1);
                if (op == O::Adc) as.ADD(result, result, t2);
                else { as.XORI(t2, t2, 1); as.SUB(result, result, t2); }
            }
            if (op == O::Mul) as.MUL(result, result, t1);
            if (op == O::Div) {
                const auto type = inst->GetArg<ir::Value>(0).Type();
                context.Mask(result, Bits(type)); context.Mask(t1, Bits(type));
                Label nonzero, done;
                as.BNE(t1, x0, &nonzero);
                as.MV(result, x0); as.J(&done);
                as.Bind(&nonzero);
                if (ir::IsSignValueType(type)) {
                    context.SignExtend(result, Bits(type)); context.SignExtend(t1, Bits(type));
                    as.DIV(result, result, t1);
                } else as.DIVU(result, result, t1);
                as.Bind(&done);
            }
            if (op == O::And) as.AND(result, result, t1);
            if (op == O::Or) as.OR(result, result, t1);
            if (op == O::Xor) as.XOR(result, result, t1);
            if (op == O::AndNot) { as.NOT(t1, t1); as.AND(result, result, t1); }
            break;
        case O::Not:
            if (inst->ArgAt(1).IsVoid()) {
                context.Read(result, inst->GetArg<ir::Value>(0)); as.SEQZ(result, result);
            } else { context.Operand(result, inst->GetArg<ir::Operand>(1)); as.NOT(result, result); }
            break;
        case O::MulHigh: case O::MulSub: case O::SignedDiv64:
            context.Read(result, inst->GetArg<ir::Value>(0));
            context.Read(t1, inst->GetArg<ir::Value>(1));
            if (op == O::MulHigh) {
                if (inst->GetArg<ir::Imm>(2).Get()) as.MULH(result, result, t1);
                else as.MULHU(result, result, t1);
            } else if (op == O::MulSub) {
                context.Read(t2, inst->GetArg<ir::Value>(2));
                as.MUL(result, result, t1); as.SUB(result, t2, result);
            } else {
                Label nonzero, done;
                as.BNE(t1, x0, &nonzero); as.MV(result, x0); as.J(&done);
                as.Bind(&nonzero); as.DIV(result, result, t1); as.Bind(&done);
            }
            break;
        case O::LslImm: case O::LslValue: case O::LsrImm: case O::LsrValue:
        case O::AsrImm: case O::AsrValue: case O::RorImm: case O::RorValue: {
            const auto value = inst->GetArg<ir::Value>(0);
            const auto bits = Bits(inst->ReturnType());
            context.Read(result, value);
            if (inst->ArgAt(1).IsImm()) as.LI(t1, inst->GetArg<ir::Imm>(1).Get());
            else context.Read(t1, inst->GetArg<ir::Value>(1));
            as.ANDI(t1, t1, bits - 1);
            if (op == O::AsrImm || op == O::AsrValue) {
                context.SignExtend(result, Bits(value.Type())); as.SRA(result, result, t1);
            } else {
                context.Mask(result, bits);
                if (op == O::LslImm || op == O::LslValue) as.SLL(result, result, t1);
                else if (op == O::LsrImm || op == O::LsrValue) as.SRL(result, result, t1);
                else {
                    as.SUB(t2, x0, t1); as.ANDI(t2, t2, bits - 1);
                    as.SLL(t2, result, t2); as.SRL(result, result, t1); as.OR(result, result, t2);
                }
            }
            break;
        }
        default: return false;
    }
    context.Write(inst, result);
    return true;
}

void JitTranslator::EmitMemory(ir::Inst* inst, bool store, bool ordered, GPR result) {
    auto& as = context.GetMasm();
    const auto type = store ? inst->GetArg<ir::Value>(1).Type() : inst->ReturnType();
    const u32 size = ir::GetValueSizeByte(type);
    context.Operand(a1, inst->GetArg<ir::Operand>(0));
    as.MV(a0, state); as.LI(a2, size);
    as.LI(t0, reinterpret_cast<u64>(&MemoryAddress)); as.JALR(t0);
    context.Load(t0, state, state_offset_halt_reason, 4);
    context.BranchZero(t0, epilogue, false);
    // Naturally aligned scalar accesses must remain a single memory
    // operation: decomposing them into bytes would tear under guest threads.
    Label unaligned, done;
    if (size > 1) {
        as.ANDI(t2, a0, size - 1);
        as.BNE(t2, x0, &unaligned);
    }
    if (ordered) as.FENCE();
    if (store) {
        context.Read(t0, inst->GetArg<ir::Value>(1));
        context.Store(t0, a0, 0, size);
    } else {
        context.Load(result, a0, 0, size);
    }
    if (ordered) as.FENCE();
    if (size == 1) {
        if (!store) context.Write(inst, result);
        return;
    }
    as.J(&done);
    as.Bind(&unaligned);
    if (store) {
        context.Read(t0, inst->GetArg<ir::Value>(1));
        if (ordered) as.FENCE();
        // Byte accesses make unaligned guest addresses portable to RV64
        // machines that do not implement misaligned word accesses in hardware.
        for (u32 byte = 0; byte < size; ++byte) {
            as.SB(t0, byte, a0);
            if (byte + 1 != size) as.SRLI(t0, t0, 8);
        }
        if (ordered) as.FENCE();
    } else {
        if (ordered) as.FENCE();
        as.MV(result, x0);
        for (u32 byte = 0; byte < size; ++byte) {
            as.LBU(t1, byte, a0);
            if (byte) as.SLLI(t1, t1, byte * 8);
            as.OR(result, result, t1);
        }
        if (ordered) as.FENCE();
    }
    as.Bind(&done);
    // Both alignment paths define the same reserved scalar register.
    if (!store) context.Write(inst, result);
}

void JitTranslator::EmitTerminal(const ir::Terminal& terminal) {
    context.EnsureSpace();
    auto& as = context.GetMasm();
    VisitVariant<void>(terminal, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::ReturnToHost>) {
            Return(HaltReason::CallHost);
        } else if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                             std::is_same_v<T, ir::terminal::LinkBlockFast> ||
                             std::is_same_v<T, ir::terminal::ExternalLinkBlock>) {
            context.Load(t0, state, state_offset_current_loc);
            context.Store(t0, state, state_offset_prev_loc);
            as.LI(t0, term.next.Value());
            context.Store(t0, state, state_offset_current_loc);
            Return(HaltReason::None);
        } else if constexpr (std::is_same_v<T, ir::terminal::If> ||
                             std::is_same_v<T, ir::terminal::Condition>) {
            if constexpr (std::is_same_v<T, ir::terminal::If>) context.Read(t0, term.cond);
            else context.Condition(t0, term.cond);
            Label other;
            context.BranchZero(t0, other);
            EmitTerminal(term.then_);
            as.Bind(&other);
            EmitTerminal(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            for (const auto& item : term.cases) {
                context.EnsureSpace();
                context.Read(t0, term.value); as.LI(t1, item.case_value.Get());
                as.XOR(t0, t0, t1);
                Label next;
                context.BranchZero(t0, next, false);
                EmitTerminal(item.then);
                as.Bind(&next);
            }
            Return(HaltReason::None);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            context.Load(t0, state, state_offset_halt_reason, 4);
            context.BranchZero(t0, epilogue, false);
            EmitTerminal(term.else_);
        } else {
            Return(HaltReason::None);
        }
    });
}

}  // namespace swift::runtime::backend::riscv64
