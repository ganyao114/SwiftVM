#include "translator.h"

#include <stdexcept>

#include "runtime/backend/context.h"
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::loongarch64 {
void JitTranslator::PublishPC(bool commit) {
    if (execution_mode != ExecutionMode::Ordinary || !pc_pending) return;
    auto& as = context.GetMasm();
    context.Load(t0, state, state_offset_current_loc);
    as.LI(t1, previous_pc_delta); as.add_d(t1, t0, t1);
    context.Store(t1, state, state_offset_prev_loc);
    as.LI(t1, pc_delta); as.add_d(t0, t0, t1);
    context.Store(t0, state, state_offset_current_loc);
    stats.pc_stores += 2;
    if (commit) {
        stats.hot_pc_stores += 2;
        pc_delta = previous_pc_delta = 0; pc_pending = false;
    }
}

void JitTranslator::Return(HaltReason reason) {
    PublishPC();
    auto& as = context.GetMasm();
    if (reason == HaltReason::None && execution_mode == ExecutionMode::SingleInstruction) {
        as.J(&single_retirement);
        return;
    }
    if (reason == HaltReason::None) context.Load(a0, state, state_offset_halt_reason, 4);
    else {
        as.LI(a0, static_cast<u32>(reason));
        context.Store(a0, state, state_offset_halt_reason, 4);
    }
    as.J(&epilogue);
}

void JitTranslator::CheckHalt() {
    auto& as = context.GetMasm();
    Label resume;
    context.Load(t0, state, state_offset_halt_reason, 4);
    as.BEQ(t0, zero, &resume);
    as.MV(a0, t0); as.J(&epilogue);
    as.Bind(&resume);
}

void JitTranslator::Poll() {
    if (execution_mode == ExecutionMode::SingleInstruction) return;
    auto& as = context.GetMasm();
    Label resume, requested;
    context.Load(t0, state, state_offset_exit_request); as.dbar(0);
    as.BEQ(t0, zero, &resume);
    as.srli_d(t0, t0, 63);
    as.LI(a0, static_cast<u32>(HaltReason::CodeMiss));
    as.BEQ(t0, zero, &requested);
    as.LI(a0, static_cast<u32>(HaltReason::Signal));
    as.Bind(&requested);
    context.Store(a0, state, state_offset_halt_reason, 4); as.J(&epilogue);
    as.Bind(&resume);
}

void JitTranslator::EmitGoto(ir::Inst* inst) {
    PublishPC();
    auto& as = context.GetMasm();
    context.FlushValues(); context.DiscardValues();
    if (!guest_instructions) Poll();
    context.Read(t0, inst->GetArg<ir::Value>(0));
    context.Write(inst, t0); context.FlushValues(); context.DiscardValues();
    Label skip;
    as.BEQ(t0, zero, &skip);
    if (auto moves = taken_phis.find(inst); moves != taken_phis.end()) EmitPhiMoves(moves->second);
    as.J(&labels.at(context.BranchTarget(inst)));
    as.Bind(&skip); context.DiscardValues();
}

void JitTranslator::EmitNotGoto(ir::Inst* inst) {
    PublishPC();
    auto& as = context.GetMasm();
    context.FlushValues(); context.DiscardValues();
    if (!guest_instructions) Poll();
    context.Read(t0, inst->GetArg<ir::Value>(0));
    context.Write(inst, t0); context.FlushValues(); context.DiscardValues();
    Label skip;
    as.BNE(t0, zero, &skip);
    if (auto moves = taken_phis.find(inst); moves != taken_phis.end()) EmitPhiMoves(moves->second);
    as.J(&labels.at(context.BranchTarget(inst)));
    as.Bind(&skip); context.DiscardValues();
}

void JitTranslator::EmitBindLabel(ir::Inst* inst) {
    // Fallthrough commits before the label. Taken edges committed at their
    // branch and skip this code, so both arrive with a canonical cursor.
    PublishPC();
    context.FlushValues(); context.DiscardValues();
    auto& as = context.GetMasm();
    as.Bind(&labels.at(inst->GetArg<ir::Value>(0).Def()));
    if (auto location = context.LocationAt(inst)) {
        context.Load(t0, state, state_offset_current_loc); context.Store(t0, state, state_offset_prev_loc);
        as.LI(t0, location->Value()); context.Store(t0, state, state_offset_current_loc);
    }
    if (!context.IsSyntheticLabel(inst) && (!guest_instructions || context.LocationAt(inst))) Poll();
}

void JitTranslator::EmitAdvancePC(ir::Inst* inst) {
    if (execution_mode == ExecutionMode::SingleInstruction)
        throw std::invalid_argument("LoongArch Step cannot contain block retirement markers");
    previous_pc_delta = pc_delta;
    pc_delta += inst->GetArg<ir::Imm>(0).Get(); pc_pending = true;
}

void JitTranslator::EmitCheckpoint(ir::Inst* inst) {
    if (execution_mode != ExecutionMode::Ordinary)
        throw std::invalid_argument("LoongArch Step cannot contain retirement checkpoints");
    ++stats.checkpoints;
    context.FlushValues(); context.DiscardValues();
    auto& as = context.GetMasm();
    Label requested, restore, resume;
    context.Load(t0, state, state_offset_halt_reason, 4);
    as.BNE(t0, zero, &restore);
    context.Load(t0, state, state_offset_exit_request); as.dbar(0);
    as.BEQ(t0, zero, &resume);
    as.srli_d(t0, t0, 63);
    as.LI(a0, static_cast<u32>(HaltReason::CodeMiss));
    as.BEQ(t0, zero, &requested);
    as.LI(a0, static_cast<u32>(HaltReason::Signal));
    as.Bind(&requested);
    context.Store(a0, state, state_offset_halt_reason, 4);
    as.Bind(&restore);
    const auto& params = inst->GetArg<ir::Params>(0);
    std::optional<u32> offset;
    for (const auto& param : params) {
        if (!offset) {
            if (!param.data.IsImm()) throw std::runtime_error("invalid checkpoint offset");
            offset = param.data.imm.Get();
        } else {
            if (!param.data.IsValue()) throw std::runtime_error("invalid checkpoint value");
            const auto value = param.data.value;
            const u32 size = ir::GetValueSizeByte(value.Type());
            if (!size || size > 16) throw std::runtime_error("invalid checkpoint width");
            vectors.ReadCheckpointPart(t0, value, 0);
            context.Store(t0, state, state_offset_uniform_buffer + *offset, std::min(size, 8u));
            ++stats.cold_uniform_stores;
            if (size > 8) {
                vectors.ReadCheckpointPart(t0, value, 1);
                context.Store(t0, state, state_offset_uniform_buffer + *offset + 8, 8);
                ++stats.cold_uniform_stores;
            }
            offset.reset();
        }
    }
    if (offset) throw std::runtime_error("incomplete checkpoint snapshot");
    PublishPC(false);
    context.Load(a0, state, state_offset_halt_reason, 4); as.J(&epilogue);
    as.Bind(&resume);
    context.DiscardValues();
}
void JitTranslator::EmitSetLocation(ir::Inst* inst) {
    PublishPC();
    if (execution_mode == ExecutionMode::Ordinary) {
        context.Load(t0, state, state_offset_current_loc); context.Store(t0, state, state_offset_prev_loc);
    }
    context.Lambda(t0, inst->GetArg<ir::Lambda>(0)); context.Store(t0, state, state_offset_current_loc);
}
void JitTranslator::EmitGetLocation(ir::Inst* inst) {
    PublishPC();
    context.Load(t0, state, state_offset_current_loc); context.Write(inst, t0);
}
void JitTranslator::EmitNop(ir::Inst*) {}
void JitTranslator::EmitUniformBarrier(ir::Inst*) { context.FlushValues(); context.DiscardValues(); }
void JitTranslator::EmitXchgBarrier(ir::Inst*) {}
void JitTranslator::EmitPushRSB(ir::Inst*) {}
void JitTranslator::EmitPopRSB(ir::Inst*) {}
void JitTranslator::EmitCallReturn(ir::Inst*) {}

void JitTranslator::EmitTerminal(const ir::Terminal& terminal) {
    PublishPC();
    vectors.Barrier();
    auto& as = context.GetMasm();
    if (execution_mode == ExecutionMode::SingleInstruction) CheckHalt();
    VisitVariant<void>(terminal, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::ReturnToHost>) Return(HaltReason::CallHost);
        else if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                           std::is_same_v<T, ir::terminal::LinkBlockFast> ||
                           std::is_same_v<T, ir::terminal::ExternalLinkBlock>) {
            if (execution_mode == ExecutionMode::Ordinary) {
                context.Load(t0, state, state_offset_current_loc); context.Store(t0, state, state_offset_prev_loc);
            }
            as.LI(t0, term.next.Value()); context.Store(t0, state, state_offset_current_loc);
            Return(HaltReason::None);
        } else if constexpr (std::is_same_v<T, ir::terminal::If> || std::is_same_v<T, ir::terminal::Condition>) {
            if constexpr (std::is_same_v<T, ir::terminal::If>) context.Read(t0, term.cond);
            else EmitCondition(term.cond, t0);
            Label other; as.BEQ(t0, zero, &other);
            EmitTerminal(term.then_); as.Bind(&other); EmitTerminal(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            for (const auto& item : term.cases) {
                context.Read(t0, term.value); as.LI(t1, item.case_value.Get());
                Label next; as.BNE(t0, t1, &next); EmitTerminal(item.then); as.Bind(&next);
            }
            Return(HaltReason::None);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            CheckHalt(); EmitTerminal(term.else_);
        } else Return(HaltReason::None);
    });
}

void JitTranslator::EmitSingleRetirement() {
    CheckHalt();
    auto& as = context.GetMasm();
    Label retired;
    context.Load(t0, state, state_offset_exit_request); as.dbar(0);
    as.srli_d(t0, t0, 63); as.BEQ(t0, zero, &retired);
    Return(HaltReason::Signal);
    as.Bind(&retired); Return(HaltReason::Step);
}
}  // namespace swift::runtime::backend::loongarch64
