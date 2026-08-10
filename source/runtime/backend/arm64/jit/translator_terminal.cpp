#include "translator.h"
#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"

#include <functional>
#include <iterator>
#include <type_traits>

namespace swift::runtime::backend::arm64 {

#define __ masm.

void JitTranslator::EmitTerminal(const ir::Terminal& terminal,
                                 LinkSiteKind direct_link_kind) {
    VisitVariant<void>(terminal, [this, direct_link_kind](auto term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::Invalid>) {
            // Flat decoded blocks have no explicit terminal: the next location was
            // already written to state->current_loc by a SetLocation instruction.
            MergeNZCV();
            context.RecordExecCounter(static_next_loc ? exec_offset_exit_direct
                                                      : exec_offset_exit_indirect);
            if (!EmitStaticForward() && !EmitIndirectForward()) {
                __ Ret();
            }
        } else if constexpr (std::is_same_v<T, ir::terminal::ReturnToDispatch>) {
            MergeNZCV();
            context.RecordExecCounter(
                    cur_block_is_call ? exec_offset_exit_call
                                      : (static_next_loc ? exec_offset_exit_direct
                                                         : exec_offset_exit_indirect));
            if (!EmitStaticForward() && !EmitIndirectForward()) {
                __ Ret();
            }
        } else if constexpr (std::is_same_v<T, ir::terminal::ReturnToHost>) {
            MergeNZCV();
            context.RecordExecCounter(exec_offset_exit_syscall);
            __ Mov(ipw, static_cast<u32>(HaltReason::CallHost));
            __ Str(ipw, MemOperand(state, state_offset_halt_reason));
            __ Ret();
        } else if constexpr (std::is_same_v<T, ir::terminal::LinkBlock>) {
            if (IsRegionInternalEdge(term.next)) {
                EmitRegionEdge(term.next);
                return;
            }
            MergeNZCV();
            context.RecordExecCounter(exec_offset_exit_direct);
            auto* exit = IsSelfEdge(term.next) && backedge_exit_label
                    ? backedge_exit_label.get()
                    : GetDirectCycleExit(term.next);
            backedge_exit_referenced |=
                    exit && exit == backedge_exit_label.get();
            auto* self_target = IsSelfEdge(term.next) &&
                                        (backedge_flags_plan || loop_hoist_body_entry)
                    ? LocalBranchTarget(term.next)
                    : nullptr;
            const u32 link_before = context.CurrentBufferSize();
            context.Forward(term.next, exit, self_target, direct_link_kind);
            RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                                context.CurrentBufferSize());
        } else if constexpr (std::is_same_v<T, ir::terminal::LinkBlockFast>) {
            if (IsRegionInternalEdge(term.next)) {
                EmitRegionEdge(term.next);
                return;
            }
            MergeNZCV();
            context.RecordExecCounter(exec_offset_exit_direct);
            auto* exit = IsSelfEdge(term.next) && backedge_exit_label
                    ? backedge_exit_label.get()
                    : GetDirectCycleExit(term.next);
            backedge_exit_referenced |=
                    exit && exit == backedge_exit_label.get();
            auto* self_target = IsSelfEdge(term.next) &&
                                        (backedge_flags_plan || loop_hoist_body_entry)
                    ? LocalBranchTarget(term.next)
                    : nullptr;
            const u32 link_before = context.CurrentBufferSize();
            context.Forward(term.next, exit, self_target, direct_link_kind);
            RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                                context.CurrentBufferSize());
        } else if constexpr (std::is_same_v<T, ir::terminal::PopRSBHint>) {
            // Return Stack Buffer: this is the real pop+predict site. It must
            // run here (not at the PopRSB instruction) because guest flags have
            // just been committed by FlushFlags/MergeNZCV — the hit path
            // branches directly to the return target, which expects the flags
            // register to be current. EmitRSBPop ends in Br (hit) or Ret (miss/
            // underflow), so it fully terminates the block.
            MergeNZCV();
            context.RecordExecCounter(exec_offset_exit_ret);
            if (True(context.GetConfig().global_opts & Optimizations::ReturnStackBuffer)) {
                const u32 link_before = context.CurrentBufferSize();
                context.EmitRSBPop();
                RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                                    context.CurrentBufferSize());
            } else {
                __ Ret();
            }
        } else if constexpr (std::is_same_v<T, ir::terminal::If>) {
            if (EmitRegionIf(term,
                             direct_link_kind == LinkSiteKind::Unconditional)) {
                return;
            }
            Label else_label;
            if (auto local = LocalConditionFor(term.cond)) {
                __ B(&else_label,
                     static_cast<Condition>(static_cast<u8>(*local) ^ 1));
            } else {
                __ Cbz(context.W(term.cond), &else_label);
            }
            EmitTerminal(term.then_, LinkSiteKind::ConditionalThen);
            __ Bind(&else_label);
            EmitTerminal(term.else_, LinkSiteKind::ConditionalElse);
        } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
            if (EmitRegionCondition(
                        term,
                        direct_link_kind == LinkSiteKind::Unconditional)) {
                return;
            }
            Label else_label;
            auto host_cond = MapCond(term.cond);
            if (!(save_in_nzcv && nzcv_dirty)) {
                LoadNZCVFromFlags();
            }
            __ B(&else_label, static_cast<Condition>(static_cast<u8>(host_cond) ^ 1));
            EmitTerminal(term.then_, LinkSiteKind::ConditionalThen);
            __ Bind(&else_label);
            EmitTerminal(term.else_, LinkSiteKind::ConditionalElse);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            // Linear compare chain; each arm ends with its own terminal.
            MergeNZCV();
            auto value = context.R(term.value);
            for (auto& case_ : term.cases) {
                Label next_case;
                __ Mov(ip, case_.case_value.Get());
                __ Cmp(value, ip);
                __ B(&next_case, ne);
                EmitTerminal(case_.then, LinkSiteKind::SwitchArm);
                __ Bind(&next_case);
            }
            // No case matched: bail out to the dispatcher.
            context.RecordExecCounter(exec_offset_exit_indirect);
            __ Ret();
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            Label no_halt;
            __ Ldr(ipw, MemOperand(state, state_offset_halt_reason));
            __ Cbz(ipw, &no_halt);
            MergeNZCV();
            __ Ret();
            __ Bind(&no_halt);
            EmitTerminal(term.else_, LinkSiteKind::CheckHalt);
        } else {
            PANIC("Unknown terminal!");
        }
    });
}

std::optional<Condition> JitTranslator::LocalConditionFor(ir::Value value) const {
    if (!value.Def()) {
        return std::nullopt;
    }
    if (auto it = local_conditions.find(value.Def()); it != local_conditions.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool JitTranslator::IsCompactFCmp(ir::Value value) {
    return value.Def() && value.Def()->GetOp() == ir::OpCode::VecFCmp &&
           value.Def()->GetArg<ir::Imm>(3).Get() != 0;
}

bool JitTranslator::RecordLocalCondition(ir::Inst* inst, ir::Cond cond) {
    if (inst->GetUses() != 1) {
        return false;
    }
    auto& list = cur_block->GetInstList();
    for (auto it = std::next(list.iterator_to(*inst)); it != list.end(); ++it) {
        bool names = false;
        for (auto value : it->GetValues()) {
            names = names || value.Def() == inst;
        }
        if (!names) {
            continue;
        }
        const bool supported =
                (it->GetOp() == ir::OpCode::Goto ||
                 it->GetOp() == ir::OpCode::NotGoto) &&
                        it->GetArg<ir::Value>(0).Def() == inst ||
                it->GetOp() == ir::OpCode::Select &&
                        it->GetArg<ir::Value>(0).Def() == inst;
        if (!supported) {
            return false;
        }
        local_conditions.emplace(inst, MapCond(cond));
        return true;
    }

    bool terminal_use = false;
    std::function<void(const ir::Terminal&)> visit = [&](const ir::Terminal& terminal) {
        VisitVariant<void>(terminal, [&](auto term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::If>) {
                if (term.cond.Def() == inst) {
                    terminal_use = true;
                }
                visit(term.then_);
                visit(term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
                visit(term.then_);
                visit(term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
                visit(term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                for (const auto& arm : term.cases) {
                    visit(arm.then);
                }
            }
        });
    };
    visit(cur_block->GetTerminal());
    if (terminal_use) {
        local_conditions.emplace(inst, MapCond(cond));
    }
    return terminal_use;
}

// A direct jmp/call decodes to SetLocation(imm) + ReturnToDispatcher, and the
// trampoline then re-reads state->current_loc and walks the L1 hash chain for
// a target that was already known when the code was emitted. The dispatch
// table indexed here is the same one the RSB pop and JitContext::Forward's
// BlockLink path already branch through, with the same safety property: SMC
// invalidation (SmcTracker::ClearDispatchSlots) zeroes the slot, so a stale
// translation degrades to the Cbz fallback rather than to a wild branch.
bool JitTranslator::EmitStaticForward() {
    if (!static_next_loc) {
        return false;
    }
    const u64 target = *static_next_loc;
    static_next_loc.reset();
    const u32 link_before = context.CurrentBufferSize();
    const auto location = ir::Location{target};
    const bool emitted = context.ForwardStatic(
            location, GetDirectCycleExit(location));
    RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                        context.CurrentBufferSize());
    return emitted;
}

bool JitTranslator::EmitIndirectForward() {
    if (!context.GetFeatures().indirect_l1 || !dynamic_next_loc) {
        return false;
    }
    const auto location = context.X(*dynamic_next_loc);
    dynamic_next_loc.reset();
    const u32 link_before = context.CurrentBufferSize();
    context.ForwardIndirectL1(location);
    RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                        context.CurrentBufferSize());
    return true;
}

Condition JitTranslator::MapCond(ir::Cond cond) {
    // ir::Cond values match the ARM condition encoding.
    return static_cast<Condition>(static_cast<u8>(cond) & 0xF);
}


#undef __

}  // namespace swift::runtime::backend::arm64
