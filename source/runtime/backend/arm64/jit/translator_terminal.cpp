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
    return cur_block && IsCanonicalTerminalEntry(cur_block->GetStartLocation().Value());
}

bool JitTranslator::EmitCanonicalTerminalEdge(ir::Location target) {
    if (!IsCanonicalTerminalEntry() || !IsRegionInternalEdge(target)) {
        return false;
    }
    context.RecordExecCounter(exec_offset_exit_direct);
    ++statistics.region_block_edges;
    statistics.region_block_local_branch_bytes += sizeof(u32);
    const u32 link_before = context.CurrentBufferSize();
    auto* cycle_exit = GetExternalCycleExit(target);
    RecordExitPollFault(
            context.ForwardPublishedEntry(
                    target, cycle_exit, cycle_exit ? nullptr : GetTerminalRequestExit()),
            cycle_exit ? cycle_exit : GetTerminalRequestExit(),
            target);
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
    const bool canonical_return = context.ContinuationActive() && call_return_value &&
                                  call_return_pc &&
                                  context.RequiresUnitState(ir::Location{*call_return_pc});
    if (canonical_return) {
        // A guest callee may overwrite ordinary SSA registers and spill
        // slots. Return through canonical dispatch instead of this unit's
        // captured continuation; its dependent entry is never exported.
        ContinuationContract::PublishExternalFrame(masm, context.X(*call_return_value));
        direct_link_kind = LinkSiteKind::Unconditional;
    }
    context.RecordExecCounter(
            call_exit && cur_block_is_call
                    ? exec_offset_exit_call
                    : (static_next_loc ? exec_offset_exit_direct
                                       : exec_offset_exit_indirect));
    if (!EmitStaticForward(call_continuation ? LinkSiteKind::Call : direct_link_kind,
                           terminal_flags_bypass,
                           external_edge,
                           !call_return_value) &&
        !(CanUseIndirectCallContinuation() ? EmitIndirectCallForward(pending_call_flags)
                                           : EmitIndirectForward())) {
        if (!TryEmitReturnFlagsBypass(terminal_flags_bypass)) {
            context.ReturnHost();
        }
    }
}

void JitTranslator::EmitTerminal(const ir::Terminal& terminal,
                                 LinkSiteKind direct_link_kind,
                                 DirectLinkFlagsBypass flags_bypass,
                                 bool canonical_transfer) {
    VisitVariant<void>(terminal, [this, direct_link_kind, flags_bypass, canonical_transfer](auto term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (!std::is_same_v<T, ir::terminal::If> &&
                      !std::is_same_v<T, ir::terminal::Condition> &&
                      !std::is_same_v<T, ir::terminal::Switch> &&
                      !std::is_same_v<T, ir::terminal::CheckHalt>) {
            if (canonical_transfer && FlagsRegsEnabled()) LoadNZCVFromFlags();
        }
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
            dynamic_location_deferred = false;
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
                                    local_flags_bypass.Valid() ? local_flags_bypass : flags_bypass,
                                    exit ? nullptr : GetTerminalRequestExit()),
                    exit ? exit : GetTerminalRequestExit(),
                    term.next);
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
                                    local_flags_bypass.Valid() ? local_flags_bypass : flags_bypass,
                                    exit ? nullptr : GetTerminalRequestExit()),
                    exit ? exit : GetTerminalRequestExit(),
                    term.next);
            RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                                context.CurrentBufferSize());
        } else if constexpr (std::is_same_v<T, ir::terminal::PopRSBHint>) {
            // A retained return target uses the inline L1 path. Without that
            // target an L1 module returns to the dispatcher; only modules that
            // disable L1 consume RSB frames.
            const bool l1_enabled = context.GetFeatures().indirect_l1;
            const bool inline_l1_return = l1_enabled && dynamic_next_loc.has_value();
            // The return key comparison clobbers PSTATE. A transparent
            // callee tail can inherit pending NZCV from another region block,
            // so commit it before either continuation or dispatcher traversal.
            MergeNZCV(FlagsRegsEnabled() || (l1_enabled && !inline_l1_return)
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
                dynamic_location_deferred = false;
                context.EmitRSBPop(actual_target, GetTerminalRequestExit());
                RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                                    context.CurrentBufferSize());
            } else {
                context.ReturnHost();
            }
        } else if constexpr (std::is_same_v<T, ir::terminal::If>) {
            if (EmitRegionIf(term,
                             direct_link_kind == LinkSiteKind::Unconditional,
                             canonical_transfer)) {
                return;
            }
            // One commit for both arms. MergeNZCV does not clobber host NZCV,
            // so a local b.cond can still read the cmp. Publishing per arm
            // doubled the AdvancePC merge we just removed.
            const auto pending_mask = PendingNZCVMergeMask(
                    FlagsRegsAuditMergeCause::TerminalDispatcher);
            const bool flags_bypassable =
                    CanBypassTerminalFlagsMerge(term.then_) &&
                    CanBypassTerminalFlagsMerge(term.else_) &&
                    (!pending_mask || *pending_mask == static_cast<u64>(HostFlags::NZCV));
            const auto local_flags_bypass = canonical_transfer
                    ? DirectLinkFlagsBypass{}
                    : MergeNZCV(FlagsRegsAuditMergeCause::TerminalDispatcher,
                                flags_audit_block_edge,
                                flags_bypassable && !flag_state.flags_token_valid);
            const auto available_bypass = local_flags_bypass.Valid()
                    ? local_flags_bypass : flags_bypass;
            const auto branch_flags_bypass = flags_bypassable &&
                    available_bypass.edge_flags.valid_nzcv_mask == kEdgeNZCVMask
                    ? available_bypass : DirectLinkFlagsBypass{};
            const bool canonical_edges = canonical_transfer ||
                    (FlagsRegsEnabled() && !branch_flags_bypass.Valid());
            if (branch_flags_bypass.Valid()) {
                terminal_bypass_nzcv |= static_cast<HostFlags>(
                        branch_flags_bypass.edge_flags.valid_nzcv_mask);
            }
            flag_state.nzcv_dirty = false;
            flag_state.nzcv_requested = {};
            InvalidateFlagsToken();
            Label else_label;
            if (!EmitDeadEdgeZeroBranch(term.cond, &else_label, false)) {
                if (auto local = LocalConditionFor(term.cond)) {
                    __ B(&else_label,
                         static_cast<Condition>(static_cast<u8>(*local) ^ 1));
                } else {
                    __ Cbz(ConditionRegister(term.cond), &else_label);
                }
            }
            EmitTerminal(term.then_,
                         LinkSiteKind::ConditionalThen,
                         branch_flags_bypass, canonical_edges);
            __ Bind(&else_label);
            EmitTerminal(term.else_,
                         LinkSiteKind::ConditionalElse,
                         branch_flags_bypass, canonical_edges);
        } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
            if (ir::IsAlwaysCondition(term.cond)) {
                // AL and NV both mean true, with no conditional inverse.
                // Publish pending producers before using the canonical word:
                // an independent ClearFlags can update x26 without PSTATE.
                // A transparent successor must not merge those old bits back.
                if (FlagsRegsEnabled()) {
                    if (!canonical_transfer)
                        MergeNZCV(FlagsRegsAuditMergeCause::TerminalDispatcher,
                                  flags_audit_block_edge);
                    flag_state.nzcv_dirty = false;
                    flag_state.nzcv_requested = {};
                    InvalidateFlagsToken();
                }
                EmitTerminal(term.then_, direct_link_kind,
                             FlagsRegsEnabled() ? DirectLinkFlagsBypass{} : flags_bypass,
                             canonical_transfer || FlagsRegsEnabled());
                return;
            }
            if (EmitRegionCondition(
                        term,
                        direct_link_kind == LinkSiteKind::Unconditional,
                        canonical_transfer)) {
                return;
            }
            DirectLinkFlagsBypass branch_flags_bypass{};
            if (flag_state.save_in_nzcv && flag_state.nzcv_dirty) {
                const bool complete_pstate = flag_state.nzcv_requested == HostFlags::NZCV;
                const bool flags_bypassable =
                        CanBypassTerminalFlagsMerge(term.then_) &&
                        CanBypassTerminalFlagsMerge(term.else_) && complete_pstate;
                const auto local_flags_bypass = MergeNZCV(
                        FlagsRegsAuditMergeCause::TerminalDispatcher,
                        flags_audit_block_edge,
                        flags_bypassable && !flag_state.flags_token_valid);
                if (flags_bypassable) {
                    branch_flags_bypass = local_flags_bypass;
                }
                if (!complete_pstate) LoadNZCVFromFlags();
            } else {
                // A PF token can still be pending after NZCV was committed
                // or cleared. Publish the complete guest flags before the
                // split invalidates that token. PendingNZCVMergeMask decides
                // whether PSTATE is authoritative; a nontransparent block
                // must not import its unrelated host NZCV here.
                if (!canonical_transfer && FlagsRegsEnabled()) {
                    MergeNZCV(FlagsRegsAuditMergeCause::TerminalDispatcher,
                              flags_audit_block_edge);
                }
                LoadNZCVFromFlags();
            }
            const bool canonical_edges = canonical_transfer ||
                    (FlagsRegsEnabled() && !branch_flags_bypass.Valid());
            if (branch_flags_bypass.Valid()) {
                terminal_bypass_nzcv |= static_cast<HostFlags>(
                        branch_flags_bypass.edge_flags.valid_nzcv_mask);
            }
            flag_state.nzcv_dirty = false;
            flag_state.nzcv_requested = {};
            InvalidateFlagsToken();
            Label else_label;
            auto host_cond = MapCond(term.cond);
            __ B(&else_label, static_cast<Condition>(static_cast<u8>(host_cond) ^ 1));
            EmitTerminal(term.then_,
                         LinkSiteKind::ConditionalThen,
                         branch_flags_bypass, canonical_edges);
            __ Bind(&else_label);
            EmitTerminal(term.else_,
                         LinkSiteKind::ConditionalElse,
                         branch_flags_bypass, canonical_edges);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            // Linear compare chain; each arm ends with its own terminal.
            if (!canonical_transfer) {
                MergeNZCV(BlockIsFlagsTransparent(cur_block)
                                  ? FlagsRegsAuditMergeCause::TerminalDispatcher
                                  : FlagsRegsAuditMergeCause::PStateClobber,
                          flags_audit_block_edge);
            }
            // Cmp below clobbers host NZCV. Commit is done; do not let
            // terminal keep re-merge the switch key into x26.
            flag_state.nzcv_dirty = false;
            flag_state.nzcv_requested = {};
            auto value = context.R(term.value);
            // Nested terminals can consume the original raw condition, which
            // need not equal canonical x26 after a partial guest-flags save.
            __ Mrs(ip, NZCV);
            __ Str(ip, MemOperand(sp, -16, PreIndex));
            for (auto& case_ : term.cases) {
                Label next_case;
                __ Mov(ip, case_.case_value.Get());
                __ Cmp(value, ip);
                __ B(&next_case, ne);
                __ Ldr(ip, MemOperand(sp, 16, PostIndex));
                __ Msr(NZCV, ip);
                EmitTerminal(case_.then, LinkSiteKind::SwitchArm, {}, true);
                __ Bind(&next_case);
            }
            // No case matched: bail out to the dispatcher.
            __ Ldr(ip, MemOperand(sp, 16, PostIndex));
            if (FlagsRegsEnabled()) LoadNZCVFromFlags();
            else __ Msr(NZCV, ip);
            context.RecordExecCounter(exec_offset_exit_indirect);
            context.ReturnHost();
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            Label no_halt;
            __ Ldr(ipw, MemOperand(state, state_offset_halt_reason));
            __ Cbz(ipw, &no_halt);
            if (canonical_transfer && FlagsRegsEnabled()) LoadNZCVFromFlags();
            MergeNZCV(FlagsRegsAuditMergeCause::HostExit,
                      FlagsRegsAuditEdgeKind::Host);
            context.ReturnHost();
            __ Bind(&no_halt);
            EmitTerminal(term.else_, LinkSiteKind::CheckHalt, {}, canonical_transfer);
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
    if (auto it = local_conditions.find(value.Id()); it != local_conditions.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool JitTranslator::LaterNeedsHostPstate(const ir::Inst* from) const {
    return !local_conditions.empty() ||
           (from && context.GetActions().Consumer(from->Id()) &&
            context.GetActions().Consumer(from->Id())->Has(ConsumerFlag::LaterPstate));
}

bool JitTranslator::IsCompactFCmp(ir::Value value) const {
    if (!value.Def() || value.Def()->GetOp() != ir::OpCode::VecFCmp ||
        value.Def()->GetArg<ir::Imm>(3).Get() == 0)
        return false;
    const auto* use = cur_instr ? context.GetActions().Consumer(cur_instr->Id()) : nullptr;
    if (use && use->Has(ConsumerFlag::IncomingPacked)) return false;
    const auto* row = context.GetActions().Consumer(value.Id());
    return !row || !row->Has(ConsumerFlag::PackedRelation);
}

bool JitTranslator::RecordLocalCondition(const ir::Inst* inst, ir::Cond cond) {
    const auto* row = inst ? context.GetActions().Consumer(inst->Id()) : nullptr;
    if (!row || !row->Has(ConsumerFlag::LocalCondition)) return false;
    local_conditions.emplace(inst->Id(), MapCond(cond));
    return true;
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
                                      bool external_edge,
                                      bool preserve_unit_state) {
    if (!static_next_loc) {
        return false;
    }
    const u64 target = *static_next_loc;
    const u32 link_before = context.CurrentBufferSize();
    const auto location = ir::Location{target};
    // Published entries and static dispatcher transfers can reach an internal
    // block without traversing its ordinary IR edge. Keep their ordered cut
    // even when the target belongs to this Function's region.
    auto* cycle_exit = external_edge || IsRegionInternalEdge(location)
                               ? GetExternalCycleExit(location)
                               : GetDirectCycleExit(location);
    // Forward links emit no exit poll: any cycle through linked units
    // contains a backward edge, and backward external edges poll through
    // their direct cycle cut (cycle_exit) or the dispatcher. This edge
    // therefore only delays exit observation by a bounded chain length.
    Label* request_exit = nullptr;
    const bool publish_frame = context.ContinuationActive() && direct_link_kind == LinkSiteKind::Call;
    const auto forwarded =
            external_edge && direct_link_kind != LinkSiteKind::Call &&
                            (preserve_unit_state || !context.RequiresUnitState(location)) &&
                            IsRegionInternalEdge(location)
                    ? JitContext::StaticForwardResult{true,
                                                      context.ForwardPublishedEntry(location,
                                                                                    cycle_exit,
                                                                                    request_exit,
                                                                                    publish_frame)}
                    : context.ForwardStatic(location,
                                            cycle_exit,
                                            direct_link_kind,
                                            flags_bypass,
                                            request_exit,
                                            preserve_unit_state);
    RecordExitPollFault(forwarded.poll_fault, cycle_exit, location);
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
    auto* miss = dynamic_location_deferred
                         ? terminal_location_publication.GetOrCreateMissLabel(location)
                         : nullptr;
    dynamic_location_deferred = false;
    const u32 link_before = context.CurrentBufferSize();
    const auto fault = context.ForwardIndirectL1(location, miss, GetTerminalRequestExit());
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
    if (dynamic_location_deferred)
        (void)terminal_location_publication.GetOrCreateMissLabel(location);
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
    dynamic_location_deferred = false;
    const u32 link_before = context.CurrentBufferSize();
    const auto fault = context.ForwardContinuation(location,
                                                   mismatch_site.label.get(),
                                                   dispatch_site.label.get(),
                                                   GetTerminalRequestExit());
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
           !context.RequiresUnitState(ir::Location{*call_return_pc}) && static_next_loc &&
           next_region_block == call_return_pc && context.IsGPRMappedTo(*call_return_value, 14) &&
           !backedge_exit_label && direct_cycle_exits.empty() && !backedge_flags_recipe;
}

bool JitTranslator::CanUseIndirectCallContinuation() const {
    return context.ContinuationActive() && call_return_value && call_return_pc &&
           !context.RequiresUnitState(ir::Location{*call_return_pc}) && dynamic_next_loc &&
           next_region_block == call_return_pc && context.IsGPRMappedTo(*call_return_value, 14) &&
           !backedge_exit_label && direct_cycle_exits.empty() && !backedge_flags_recipe;
}

bool JitTranslator::EmitIndirectCallForward(bool pending_flags) {
    if (!CanUseIndirectCallContinuation()) {
        return false;
    }
    const auto location = context.X(*dynamic_next_loc);
    dynamic_next_loc.reset();
    if (dynamic_location_deferred)
        (void)terminal_location_publication.GetOrCreateMissLabel(location);
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
    const auto faults = context.ForwardIndirectCall(location, miss_site.label.get(), pending_flags,
                                                    GetTerminalRequestExit());
    memory_state.fault_metadata.push_back({
            .guest_start = cur_block->GetStartLocation().Value(),
            .host_begin = faults.lookup_fault.begin,
            .host_end = faults.lookup_fault.end,
            .recovery_reg = dynamic_location_deferred ? location.GetCode() : UINT32_MAX,
    });
    RecordDeferredFault(
            faults.target_fault, miss_site.label.get(), FaultRecoveryKind::IndirectCallMiss);
    if (pending_flags && !flag_state.flags_token_keep) {
        flag_state.nzcv_dirty = false;
        flag_state.nzcv_requested = {};
    }
    dynamic_location_deferred = false;
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
