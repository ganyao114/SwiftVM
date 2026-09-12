#include "translator.h"
#include "runtime/backend/arm64/continuation_contract.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"
#include "runtime/common/svm_config.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <type_traits>

namespace swift::runtime::backend::arm64 {

#define __ masm.

bool JitTranslator::IsCanonicalTerminalEntry() const {
    return cur_block && canonical_terminal_entries.contains(
            cur_block->GetStartLocation().Value());
}

bool JitTranslator::EmitCanonicalTerminalEdge(ir::Location target) {
    if (!IsCanonicalTerminalEntry() || !IsRegionInternalEdge(target)) {
        return false;
    }
    context.RecordExecCounter(exec_offset_exit_direct);
    ++statistics.region_block_edges;
    statistics.region_block_local_branch_bytes += sizeof(u32);
    const u32 link_before = context.CurrentBufferSize();
    __ B(context.GetLabel(target.Value()));
    RecordBoundaryRange(BoundarySubsequence::LinkTail,
                        link_before,
                        context.CurrentBufferSize());
    return true;
}

void JitTranslator::EmitDispatcherTerminal(
        LinkSiteKind direct_link_kind,
        DirectLinkFlagsBypass flags_bypass,
        bool external_edge,
        bool call_exit) {
    constexpr auto merge_cause = FlagsRegsAuditMergeCause::TerminalDispatcher;
    const bool pending_call_flags = CanUseIndirectCallContinuation() &&
                                    CanDeferFullNZCVMerge(merge_cause);
    const bool call_continuation = CanUseCallContinuation();
    const bool local_published_entry = external_edge && static_next_loc &&
            IsRegionInternalEdge(ir::Location{*static_next_loc});
    const auto local_flags_bypass = pending_call_flags
            ? DirectLinkFlagsBypass{}
            : MergeNZCV(merge_cause,
                        FlagsRegsAuditEdgeKind::Dispatcher,
                        !local_published_entry && context.ContinuationActive() &&
                                static_next_loc &&
                                context.CanEmitDirectLink(
                                        ir::Location{*static_next_loc}));
    const auto terminal_flags_bypass = local_flags_bypass.Valid()
            ? local_flags_bypass
            : flags_bypass;
    context.RecordExecCounter(
            call_exit && cur_block_is_call
                    ? exec_offset_exit_call
                    : (static_next_loc ? exec_offset_exit_direct
                                       : exec_offset_exit_indirect));
    if (!EmitStaticForward(call_continuation ? LinkSiteKind::Call
                                             : direct_link_kind,
                           terminal_flags_bypass,
                           external_edge) &&
        !(CanUseIndirectCallContinuation()
                  ? EmitIndirectCallForward(pending_call_flags)
                  : EmitIndirectForward())) {
        if (!TryEmitReturnFlagsBypass(terminal_flags_bypass)) {
            context.ReturnHost();
        }
    }
}

void JitTranslator::EmitTerminal(const ir::Terminal& terminal,
                                 LinkSiteKind direct_link_kind,
                                 DirectLinkFlagsBypass flags_bypass) {
    VisitVariant<void>(terminal, [this, direct_link_kind, flags_bypass](auto term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (!std::is_same_v<T, ir::terminal::Invalid> &&
                      !std::is_same_v<T, ir::terminal::ReturnToDispatch> &&
                      !std::is_same_v<T, ir::terminal::ExternalLinkBlock>) {
            PublishPendingStaticLocation();
        }
        if constexpr (std::is_same_v<T, ir::terminal::Invalid>) {
            EmitDispatcherTerminal(direct_link_kind, flags_bypass, false, false);
        } else if constexpr (std::is_same_v<T, ir::terminal::ReturnToDispatch>) {
            EmitDispatcherTerminal(direct_link_kind, flags_bypass, false, true);
        } else if constexpr (std::is_same_v<T, ir::terminal::ExternalLinkBlock>) {
            static_next_loc = term.next.Value();
            dynamic_next_loc.reset();
            dynamic_location_miss = nullptr;
            EmitDispatcherTerminal(direct_link_kind, flags_bypass, true, false);
        } else if constexpr (std::is_same_v<T, ir::terminal::ReturnToHost>) {
            MergeNZCV(FlagsRegsAuditMergeCause::HostExit,
                      FlagsRegsAuditEdgeKind::Host);
            context.RecordExecCounter(exec_offset_exit_syscall);
            __ Mov(ipw, static_cast<u32>(HaltReason::CallHost));
            __ Str(ipw, MemOperand(state, state_offset_halt_reason));
            context.ReturnHost();
        } else if constexpr (std::is_same_v<T, ir::terminal::LinkBlock>) {
            if (EmitCanonicalTerminalEdge(term.next)) {
                return;
            }
            if (IsRegionInternalEdge(term.next)) {
                EmitRegionEdge(term.next);
                return;
            }
            const bool flags_bypassable = !flag_state.flags_token_valid &&
                                          context.CanEmitDirectLink(term.next);
            const auto local_flags_bypass = MergeNZCV(
                    flags_audit_block_edge ==
                                    FlagsRegsAuditEdgeKind::Dispatcher
                            ? FlagsRegsAuditMergeCause::TerminalDispatcher
                            : FlagsRegsAuditMergeCause::TerminalInternal,
                    flags_audit_block_edge,
                    flags_bypassable);
            context.RecordExecCounter(exec_offset_exit_direct);
            auto* exit = IsSelfEdge(term.next) && backedge_exit_label
                    ? backedge_exit_label.get()
                    : GetDirectCycleExit(term.next);
            backedge_exit_referenced |=
                    exit && exit == backedge_exit_label.get();
            auto* self_target = IsSelfEdge(term.next) &&
                                        (backedge_flags_recipe || loop_hoist_body_entry)
                    ? LocalBranchTarget(term.next)
                    : nullptr;
            const u32 link_before = context.CurrentBufferSize();
            RecordExitPollFault(
                    context.Forward(term.next,
                                    exit,
                                    self_target,
                                    direct_link_kind,
                                    local_flags_bypass.Valid() ? local_flags_bypass
                                                              : flags_bypass),
                    exit);
            RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                                context.CurrentBufferSize());
        } else if constexpr (std::is_same_v<T, ir::terminal::LinkBlockFast>) {
            if (EmitCanonicalTerminalEdge(term.next)) {
                return;
            }
            if (IsRegionInternalEdge(term.next)) {
                EmitRegionEdge(term.next);
                return;
            }
            const bool flags_bypassable = !flag_state.flags_token_valid &&
                                          context.CanEmitDirectLink(term.next);
            const auto local_flags_bypass = MergeNZCV(
                    flags_audit_block_edge ==
                                    FlagsRegsAuditEdgeKind::Dispatcher
                            ? FlagsRegsAuditMergeCause::TerminalDispatcher
                            : FlagsRegsAuditMergeCause::TerminalInternal,
                    flags_audit_block_edge,
                    flags_bypassable);
            context.RecordExecCounter(exec_offset_exit_direct);
            auto* exit = IsSelfEdge(term.next) && backedge_exit_label
                    ? backedge_exit_label.get()
                    : GetDirectCycleExit(term.next);
            backedge_exit_referenced |=
                    exit && exit == backedge_exit_label.get();
            auto* self_target = IsSelfEdge(term.next) &&
                                        (backedge_flags_recipe || loop_hoist_body_entry)
                    ? LocalBranchTarget(term.next)
                    : nullptr;
            const u32 link_before = context.CurrentBufferSize();
            RecordExitPollFault(
                    context.Forward(term.next,
                                    exit,
                                    self_target,
                                    direct_link_kind,
                                    local_flags_bypass.Valid() ? local_flags_bypass
                                                              : flags_bypass),
                    exit);
            RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                                context.CurrentBufferSize());
        } else if constexpr (std::is_same_v<T, ir::terminal::PopRSBHint>) {
            // A retained return target uses the inline L1 path. Without that
            // target an L1 module returns to the dispatcher; only modules that
            // disable L1 consume RSB frames.
            const bool l1_enabled = context.GetFeatures().indirect_l1;
            const bool inline_l1_return = l1_enabled && dynamic_next_loc.has_value();
            MergeNZCV(l1_enabled && !inline_l1_return
                              ? FlagsRegsAuditMergeCause::TerminalDispatcher
                              : FlagsRegsAuditMergeCause::TerminalInternal,
                      l1_enabled ? FlagsRegsAuditEdgeKind::Dispatcher
                                 : FlagsRegsAuditEdgeKind::RSBMiss);
            context.RecordExecCounter(exec_offset_exit_ret);
            if (l1_enabled) {
                if (inline_l1_return) {
                    const bool emitted = context.ContinuationActive()
                            ? EmitContinuationForward()
                            : EmitIndirectForward();
                    ASSERT(emitted);
                } else {
                    context.ReturnHost();
                }
                return;
            }
            if (True(context.GetConfig().global_opts & Optimizations::ReturnStackBuffer)) {
                const u32 link_before = context.CurrentBufferSize();
                const auto actual_target = dynamic_next_loc
                        ? std::optional{context.X(*dynamic_next_loc)}
                        : std::nullopt;
                dynamic_next_loc.reset();
                dynamic_location_miss = nullptr;
                context.EmitRSBPop(actual_target);
                RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                                    context.CurrentBufferSize());
            } else {
                context.ReturnHost();
            }
        } else if constexpr (std::is_same_v<T, ir::terminal::If>) {
            if (EmitRegionIf(term,
                             direct_link_kind == LinkSiteKind::Unconditional)) {
                return;
            }
            // One commit for both arms. MergeNZCV does not clobber host NZCV,
            // so a local b.cond can still read the cmp. Publishing per arm
            // doubled the AdvancePC merge we just removed.
            const bool flags_bypassable =
                    CanBypassTerminalFlagsMerge(term.then_) &&
                    CanBypassTerminalFlagsMerge(term.else_);
            const auto local_flags_bypass = MergeNZCV(
                    FlagsRegsAuditMergeCause::TerminalDispatcher,
                    flags_audit_block_edge,
                    flags_bypassable && !flag_state.flags_token_valid);
            const auto branch_flags_bypass =
                    flags_bypassable
                            ? (local_flags_bypass.Valid() ? local_flags_bypass
                                                         : flags_bypass)
                            : DirectLinkFlagsBypass{};
            flag_state.nzcv_dirty = false;
            flag_state.nzcv_requested = {};
            InvalidateFlagsToken();
            Label else_label;
            if (!EmitDeadEdgeZeroBranch(term.cond, &else_label, false)) {
                if (auto local = LocalConditionFor(term.cond)) {
                    __ B(&else_label,
                         static_cast<Condition>(static_cast<u8>(*local) ^ 1));
                } else {
                    __ Cbz(context.W(term.cond), &else_label);
                }
            }
            EmitTerminal(term.then_,
                         LinkSiteKind::ConditionalThen,
                         branch_flags_bypass);
            __ Bind(&else_label);
            EmitTerminal(term.else_,
                         LinkSiteKind::ConditionalElse,
                         branch_flags_bypass);
        } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
            if (EmitRegionCondition(
                        term,
                        direct_link_kind == LinkSiteKind::Unconditional)) {
                return;
            }
            DirectLinkFlagsBypass branch_flags_bypass{};
            if (flag_state.save_in_nzcv && flag_state.nzcv_dirty) {
                const bool flags_bypassable =
                        CanBypassTerminalFlagsMerge(term.then_) &&
                        CanBypassTerminalFlagsMerge(term.else_);
                const auto local_flags_bypass = MergeNZCV(
                        FlagsRegsAuditMergeCause::TerminalDispatcher,
                        flags_audit_block_edge,
                        flags_bypassable && !flag_state.flags_token_valid);
                if (flags_bypassable) {
                    branch_flags_bypass = local_flags_bypass;
                }
            } else {
                LoadNZCVFromFlags();
            }
            flag_state.nzcv_dirty = false;
            flag_state.nzcv_requested = {};
            InvalidateFlagsToken();
            Label else_label;
            auto host_cond = MapCond(term.cond);
            __ B(&else_label, static_cast<Condition>(static_cast<u8>(host_cond) ^ 1));
            EmitTerminal(term.then_,
                         LinkSiteKind::ConditionalThen,
                         branch_flags_bypass);
            __ Bind(&else_label);
            EmitTerminal(term.else_,
                         LinkSiteKind::ConditionalElse,
                         branch_flags_bypass);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            // Linear compare chain; each arm ends with its own terminal.
            MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
                      flags_audit_block_edge);
            // Cmp below clobbers host NZCV. Commit is done; do not let
            // terminal keep re-merge the switch key into x26.
            flag_state.nzcv_dirty = false;
            flag_state.nzcv_requested = {};
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
            context.ReturnHost();
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            Label no_halt;
            __ Ldr(ipw, MemOperand(state, state_offset_halt_reason));
            __ Cbz(ipw, &no_halt);
            MergeNZCV(FlagsRegsAuditMergeCause::HostExit,
                      FlagsRegsAuditEdgeKind::Host);
            context.ReturnHost();
            __ Bind(&no_halt);
            EmitTerminal(term.else_, LinkSiteKind::CheckHalt);
        } else {
            PANIC("Unknown terminal!");
        }
    });
}

bool JitTranslator::CanBypassTerminalFlagsMerge(
        const ir::Terminal& terminal) const {
    return VisitVariant<bool>(terminal, [this](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                      std::is_same_v<T, ir::terminal::LinkBlockFast>) {
            return !IsRegionInternalEdge(term.next) &&
                   context.CanEmitDirectLink(term.next);
        } else if constexpr (std::is_same_v<T, ir::terminal::If>) {
            return CanBypassTerminalFlagsMerge(term.then_) &&
                   CanBypassTerminalFlagsMerge(term.else_);
        } else {
            return false;
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

bool JitTranslator::LaterNeedsHostPstate(ir::Inst* from) const {
    if (!local_conditions.empty()) {
        return true;
    }
    bool seen = false;
    for (auto& inst : cur_block->GetInstList()) {
        if (!seen) {
            seen = &inst == from;
            continue;
        }
        switch (inst.GetOp()) {
            case ir::OpCode::CondSet:
            case ir::OpCode::CondSelect:
            case ir::OpCode::LocalCondSet:
            case ir::OpCode::FCmpCondSet:
            case ir::OpCode::InvertCarry:
            case ir::OpCode::Adc:
            case ir::OpCode::Sbb:
            case ir::OpCode::TestFlags:
            case ir::OpCode::TestNotFlags:
            case ir::OpCode::VecFCmp:
            case ir::OpCode::PublishFCmpFlags:
            case ir::OpCode::PublishSse42StrFlags:
                return true;
            default:
                break;
        }
    }
    bool needs = false;
    std::function<void(const ir::Terminal&)> visit = [&](const ir::Terminal& terminal) {
        VisitVariant<void>(terminal, [&](auto term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
                needs = true;
            } else if constexpr (std::is_same_v<T, ir::terminal::If>) {
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
    return needs;
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
bool JitTranslator::EmitStaticForward(LinkSiteKind direct_link_kind,
                                      DirectLinkFlagsBypass flags_bypass,
                                      bool external_edge) {
    if (!static_next_loc) {
        return false;
    }
    const u64 target = *static_next_loc;
    const u32 link_before = context.CurrentBufferSize();
    const auto location = ir::Location{target};
    auto* cycle_exit = external_edge ? GetExternalCycleExit(location)
                                     : GetDirectCycleExit(location);
    const auto forwarded = external_edge && IsRegionInternalEdge(location)
            ? JitContext::StaticForwardResult{
                      true, context.ForwardPublishedEntry(location, cycle_exit)}
            : context.ForwardStatic(
                      location, cycle_exit, direct_link_kind, flags_bypass);
    RecordExitPollFault(forwarded.poll_fault, cycle_exit);
    if (forwarded.emitted) {
        static_next_loc.reset();
    } else {
        PublishPendingStaticLocation();
    }
    RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                        context.CurrentBufferSize());
    return forwarded.emitted;
}

bool JitTranslator::EmitIndirectForward() {
    if (!context.GetFeatures().indirect_l1 || !dynamic_next_loc) {
        return false;
    }
    const auto location = context.X(*dynamic_next_loc);
    dynamic_next_loc.reset();
    auto* miss = dynamic_location_miss;
    dynamic_location_miss = nullptr;
    const u32 link_before = context.CurrentBufferSize();
    const auto fault = context.ForwardIndirectL1(location, miss);
    memory_state.fault_metadata.push_back({
            .guest_start = cur_block->GetStartLocation().Value(),
            .host_begin = fault.begin,
            .host_end = fault.end,
            .recovery_reg = miss ? location.GetCode() : UINT32_MAX,
    });
    RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                        context.CurrentBufferSize());
    return true;
}

bool JitTranslator::EmitContinuationForward() {
    if (!context.ContinuationActive() || !dynamic_next_loc) {
        return false;
    }
    const auto location = context.X(*dynamic_next_loc);
    dynamic_next_loc.reset();
    auto& mismatch_site = continuation_mismatch_sites[location.GetCode()];
    if (!mismatch_site.label) {
        mismatch_site.label = std::make_unique<Label>();
        mismatch_site.guest_start = cur_block->GetStartLocation().Value();
    }
    auto& dispatch_site = indirect_dispatch_sites[location.GetCode()];
    if (!dispatch_site.label) {
        dispatch_site.label = std::make_unique<Label>();
        dispatch_site.guest_start = cur_block->GetStartLocation().Value();
    }
    dynamic_location_miss = nullptr;
    const u32 link_before = context.CurrentBufferSize();
    const auto fault = context.ForwardContinuation(location,
                                                   mismatch_site.label.get(),
                                                   dispatch_site.label.get());
    RecordDeferredFault(fault, dispatch_site.label.get(), FaultRecoveryKind::ExternalContinuation);
    RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                        context.CurrentBufferSize());
    return true;
}

void JitTranslator::RecordDeferredFault(
        JitContext::FaultRange fault,
        Label* recovery,
        FaultRecoveryKind recovery_kind) {
    ASSERT(recovery);
    const size_t index = memory_state.fault_metadata.size();
    memory_state.fault_metadata.push_back({
            .guest_start = cur_block->GetStartLocation().Value(),
            .host_begin = fault.begin,
            .host_end = fault.end,
            .recovery_kind = recovery_kind,
    });
    memory_state.pending_deferred_faults.push_back({index, recovery});
}

void JitTranslator::ResolveDeferredFaults(Label* recovery) {
    ASSERT(recovery && recovery->IsBound());
    const u32 offset = static_cast<u32>(recovery->GetLocation());
    for (auto it = memory_state.pending_deferred_faults.begin();
         it != memory_state.pending_deferred_faults.end();) {
        if (it->recovery != recovery) {
            ++it;
            continue;
        }
        ASSERT(it->metadata_index < memory_state.fault_metadata.size());
        memory_state.fault_metadata[it->metadata_index].recovery_offset = offset;
        it = memory_state.pending_deferred_faults.erase(it);
    }
}

bool JitTranslator::CanUseCallContinuation() const {
    return context.ContinuationActive() && call_return_value && call_return_pc &&
           static_next_loc && next_region_block == call_return_pc &&
           context.IsGPRMappedTo(*call_return_value, 14) &&
           !backedge_exit_label && direct_cycle_exits.empty() &&
           !backedge_flags_recipe && vec_nan_cold_sites.empty();
}

bool JitTranslator::CanUseIndirectCallContinuation() const {
    return context.ContinuationActive() && call_return_value && call_return_pc &&
           dynamic_next_loc && next_region_block == call_return_pc &&
           context.IsGPRMappedTo(*call_return_value, 14) &&
           !backedge_exit_label && direct_cycle_exits.empty() &&
           !backedge_flags_recipe && vec_nan_cold_sites.empty();
}

bool JitTranslator::EmitIndirectCallForward(bool pending_flags) {
    if (!CanUseIndirectCallContinuation()) {
        return false;
    }
    const auto location = context.X(*dynamic_next_loc);
    dynamic_next_loc.reset();
    auto& miss_site = (pending_flags ? pending_call_miss_sites
                                     : indirect_call_miss_sites)[location.GetCode()];
    if (!miss_site.label) {
        miss_site.label = std::make_unique<Label>();
        miss_site.guest_start = cur_block->GetStartLocation().Value();
    }
    if (!pending_flags) {
        auto& dispatch_site = indirect_dispatch_sites[location.GetCode()];
        if (!dispatch_site.label) {
            dispatch_site.label = std::make_unique<Label>();
            dispatch_site.guest_start = cur_block->GetStartLocation().Value();
        }
    }
    const u32 link_before = context.CurrentBufferSize();
    const auto faults = context.ForwardIndirectCall(location, miss_site.label.get(), pending_flags);
    memory_state.fault_metadata.push_back({
            .guest_start = cur_block->GetStartLocation().Value(),
            .host_begin = faults.lookup_fault.begin,
            .host_end = faults.lookup_fault.end,
            .recovery_reg = dynamic_location_miss ? location.GetCode()
                                                  : UINT32_MAX,
    });
    RecordDeferredFault(
            faults.target_fault, miss_site.label.get(), FaultRecoveryKind::IndirectCallMiss);
    if (pending_flags && !flag_state.flags_token_keep) {
        flag_state.nzcv_dirty = false;
        flag_state.nzcv_requested = {};
    }
    dynamic_location_miss = nullptr;
    RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                        context.CurrentBufferSize());
    return true;
}

void JitTranslator::EmitIndirectExitColdPaths() {
    for (u32 reg = 0; reg < pending_call_miss_sites.size(); ++reg) {
        auto& site = pending_call_miss_sites[reg];
        if (!site.label) {
            continue;
        }
        __ Bind(site.label.get());
        ResolveDeferredFaults(site.label.get());
        ContinuationContract::PublishExternalFrame(masm);
        const XRegister location{reg};
        const XRegister scratch = location == ip0 ? ip1 : ip0;
        EmitNZCVMerge(static_cast<u64>(HostFlags::NZCV),
                      scratch);
        auto* recovery = terminal_location_publication.MissLabel(location);
        ASSERT(recovery);
        __ B(recovery);
        site = {};
    }
    for (u32 reg = 0; reg < indirect_dispatch_sites.size(); ++reg) {
        auto& call_site = indirect_call_miss_sites[reg];
        auto& mismatch_site = continuation_mismatch_sites[reg];
        auto& dispatch_site = indirect_dispatch_sites[reg];
        if (!dispatch_site.label) {
            ASSERT(!call_site.label && !mismatch_site.label);
            continue;
        }
        if (call_site.label) {
            __ Bind(call_site.label.get());
            ResolveDeferredFaults(call_site.label.get());
            ContinuationContract::PublishExternalFrame(masm);
            if (mismatch_site.label) {
                __ B(dispatch_site.label.get());
            }
        }
        if (mismatch_site.label) {
            __ Bind(mismatch_site.label.get());
            ResolveDeferredFaults(mismatch_site.label.get());
            __ Ldr(rsb_ptr, MemOperand(state, state_offset_rsb_empty));
        }
        __ Bind(dispatch_site.label.get());
        ResolveDeferredFaults(dispatch_site.label.get());
        const XRegister location{reg};
        auto* recovery = terminal_location_publication.MissLabel(location);
        const auto fault = context.ForwardIndirectL1(location, recovery);
        memory_state.fault_metadata.push_back({
                .guest_start = dispatch_site.guest_start,
                .host_begin = fault.begin,
                .host_end = fault.end,
                .recovery_reg = recovery ? reg : UINT32_MAX,
        });
        call_site = {};
        mismatch_site = {};
        dispatch_site = {};
    }
}

Condition JitTranslator::MapCond(ir::Cond cond) {
    // ir::Cond values match the ARM condition encoding.
    return static_cast<Condition>(static_cast<u8>(cond) & 0xF);
}


#undef __

}  // namespace swift::runtime::backend::arm64
