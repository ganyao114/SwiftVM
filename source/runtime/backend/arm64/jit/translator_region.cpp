#include "base/logging.h"
#include "runtime/backend/arm64/compile/edge_flags.h"
#include "runtime/backend/arm64/compile/effects.h"
#include "runtime/ir/hir_builder.h"
#include "translator.h"

#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/arm64/continuation_contract.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <numeric>
#include <string_view>
#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/context.h"
#include "runtime/backend/jit_cache.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/common/backedge_control.h"
#include "runtime/common/svm_config.h"
#include "translator/x86/cpu.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

bool JitTranslator::ImportUnitEdges(const ir::Block* block, const ir::HIRFunction* function) {
    const ir::Block* first = block;
    if (function) {
        if (function->GetHIRBlocksRPO().empty()) return false;
        first = function->GetHIRBlocksRPO().front().GetBlock();
    }
    if (!context.RequireBlockForEmission(first)) return false;
    const auto& actions = context.GetActions();
    const auto* certificate = actions.Certificate();
    if (!certificate || !certificate->MatchesUnit(block,
                                                  function,
                                                  context.GetAllocation(),
                                                  actions,
                                                  context.GetConfig(),
                                                  context.GetFeatures()))
        return false;
    edge_actions = &actions.Edges();
    region_function_mode = function != nullptr;
    canonical_entries_active = region_function_mode;
    return true;
}

void JitTranslator::ReportRegionEdges() {
    if (!RegionEdgesActive()) return;
    if (context.DensityProfileEnabled() && GetSvmConfig().ra_hot_coalesce_all) {
        const auto* certificate = context.GetActions().Certificate();
        ASSERT(certificate);
        const u64 unit = certificate->root_location;
        const auto& edges = Edges();
        for (const auto& row : edges.report) {
            for (const auto target :
                 std::span<const u64>{edges.report_targets}.subspan(row.begin, row.count)) {
                SVM_DIAG_PRINT(Codegen,
                               "[svm-gap-cfg-edge] unit=0x%llx block=0x%llx "
                               "target=0x%llx internal=%u\n",
                               static_cast<unsigned long long>(unit),
                               static_cast<unsigned long long>(row.location),
                               static_cast<unsigned long long>(target),
                               edges.ContainsInternalBlock(target) ? 1u : 0u);
            }
            SVM_DIAG_PRINT(Codegen,
                           "[svm-gap-cfg] unit=0x%llx block=0x%llx external=%u\n",
                           static_cast<unsigned long long>(unit),
                           static_cast<unsigned long long>(row.location),
                           row.external ? 1u : 0u);
        }
    }
}

bool JitTranslator::IsRegionInternalEdge(ir::Location target) const {
    return RegionEdgesActive() && Edges().ContainsInternalBlock(target.Value()) &&
           (!context.RequiresUnitState(target) || context.CanCarryUnitState(target));
}

bool JitTranslator::IsRegionCycleEdge(ir::Location target) const {
    return RegionEdgesActive() && cur_block &&
           Edges().IsCycleEdge(cur_block->GetStartLocation().Value(), target.Value());
}

bool JitTranslator::HasRegionCycleEdgeFromCurrent() const {
    return RegionEdgesActive() && cur_block &&
           Edges().HasCycleSource(cur_block->GetStartLocation().Value());
}

bool JitTranslator::IsDirectCycleCutEdge(ir::Location source,
                                         ir::Location target) const {
    if (!direct_cycle_latch || target.Value() >= source.Value()) {
        return false;
    }
    // PrepareRegionEdges covers internal cycles with exact DFS backedges.
    // The total-order cut also covers required value transfers when optional
    // region-edge optimization is disabled.
    const bool carries = Edges().CarriesUnitState(source.Value(), target.Value());
    if (RegionEdgesActive() && Edges().ContainsInternalBlock(target.Value()) &&
        (!context.RequiresUnitState(target) || carries)) {
        return false;
    }
    return carries || context.CanBypassDispatcher(target);
}

bool JitTranslator::IsDirectCycleCutEdge(ir::Location target) const {
    return cur_block &&
           IsDirectCycleCutEdge(cur_block->GetStartLocation(), target);
}

u32 JitTranslator::CountCycleExitCandidates() const {
    const auto& plan = Edges();
    u32 count = plan.local_cycle_source_count;
    // IR discovery is frozen; module membership can still change between
    // preparation and emission and must retain its live admission check.
    for (const auto& [source, target] : plan.backward_terminal_candidates)
        count += IsDirectCycleCutEdge(ir::Location{source}, ir::Location{target});
    return count;
}

Label* JitTranslator::GetDirectCycleExit(ir::Location target) {
    if (!IsDirectCycleCutEdge(target)) {
        return nullptr;
    }
    auto& label = direct_cycle_exits[target.Value()];
    if (!label) {
        label = std::make_unique<Label>();
        ++direct_cycle_cut_edges;
    }
    return label.get();
}

Label* JitTranslator::GetExternalCycleExit(ir::Location target) {
    if (!direct_cycle_latch || !cur_block ||
        target.Value() > cur_block->GetStartLocation().Value()) {
        return nullptr;
    }
    auto& label = direct_cycle_exits[target.Value()];
    if (!label) {
        label = std::make_unique<Label>();
        ++direct_cycle_cut_edges;
    }
    return label.get();
}

bool JitTranslator::CanUseRegionSuccessorLayout(ir::Location target) const {
    return next_region_block && *next_region_block == target.Value() &&
           !backedge_exit_label && !backedge_flags_recipe;
}

void JitTranslator::EmitRegionEdge(ir::Location target,
                                   bool fallthrough,
                                   bool record_edge_counters,
                                   bool commit_flags,
                                   bool poll_request) {
    ASSERT(IsRegionInternalEdge(target));
    if (fallthrough && IsSelfEdge(target) && loop_hoist_body_entry) {
        fallthrough = false;
    }
    if (commit_flags) {
        const auto requested = PendingNZCVMergeMask(
                FlagsRegsAuditMergeCause::TerminalInternal);
        const bool accepts_pending = FlagsRegsEnabled() && requested &&
                RegionSuccessorAcceptsEdgeFlags(
                        target,
                        PendingEdgeFlagsState(static_cast<HostFlags>(*requested),
                                              EdgeFlagsProducer::Arithmetic));
        if (accepts_pending) {
            // NZCV acceptance does not imply that the successor overwrites
            // PF. Every block resets its compile-time token, so publish it
            // while the source value is still live, even on a deferred edge.
            PublishFlagsToken();
        } else {
            // A normal internal edge must not bypass canonical publication
            // just because no optional self-loop recipe was selected.
            MergeNZCV(FlagsRegsAuditMergeCause::TerminalInternal,
                      FlagsRegsAuditEdgeKind::RegionInternal);
        }
    }
    if (record_edge_counters) {
        context.RecordExecCounter(exec_offset_exit_direct);
        context.RecordExecCounter(exec_offset_region_edges);
    }
    ++statistics.region_block_edges;
    const bool region_cycle = IsRegionCycleEdge(target);
    const bool pending_flags_cycle = !commit_flags && backedge_flags_recipe &&
            backedge_flags_recipe->plan.dead_successor && IsDirectCycleCutEdge(target);
    auto* ordered_cycle_exit = region_cycle || pending_flags_cycle
            ? nullptr
            : GetDirectCycleExit(target);
    const bool cycle = region_cycle || pending_flags_cycle || ordered_cycle_exit;
    if (cycle) {
        ASSERT(region_cycle || pending_flags_cycle
                       ? backedge_exit_label != nullptr
                       : ordered_cycle_exit != nullptr);
        context.RecordExecCounter(exec_offset_region_cycle_polls);
        if (region_cycle || pending_flags_cycle) {
            backedge_exit_referenced = true;
        }
        ++statistics.region_block_cycles;
    }
    if (fallthrough) {
        context.RecordExecCounter(exec_offset_region_fallthroughs);
        ++statistics.region_block_fallthroughs;
    } else {
        statistics.region_block_local_branch_bytes += sizeof(u32);
    }
    const u32 link_before = context.CurrentBufferSize();
    auto* cycle_exit = region_cycle || pending_flags_cycle
            ? backedge_exit_label.get()
            : ordered_cycle_exit;
    Label* request_exit{};
    if (!cycle_exit && poll_request &&
        !commit_flags && backedge_flags_recipe && backedge_flags_recipe->optimized) {
        if (!backedge_exit_label) backedge_exit_label = std::make_unique<Label>();
        request_exit = backedge_exit_label.get();
        backedge_exit_referenced = true;
    }
    // Non-cycle region edges emit no exit poll. PrepareRegionEdges covers
    // every directed cycle in the region with exact DFS backedges, and
    // those poll through cycle_exit above; cross-region cycles pass a
    // backward external edge whose direct cycle cut polls. A forward edge
    // therefore only delays exit observation by a bounded block count.
    RecordExitPollFault(context.ForwardLocal(target,
                                             cycle_exit,
                                             fallthrough,
                                             fallthrough ? nullptr : LocalBranchTarget(target),
                                             request_exit),
                        cycle_exit ? cycle_exit : request_exit,
                        target);
    RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                        context.CurrentBufferSize());
}

bool JitTranslator::BlockIsFlagsTransparent(const ir::Block* block) const {
    if (!block) return false;
    const auto* found = Edges().InternalBlock(block->GetStartLocation().Value());
    return found ? found->transparent : block == cur_block && current_flags_transparent;
}

bool JitTranslator::EmitRegionIf(const ir::terminal::If& terminal,
                                 bool allow_fallthrough,
                                 bool canonical_transfer) {
    const auto then_target = EdgeLeafTarget(terminal.then_);
    const auto else_target = EdgeLeafTarget(terminal.else_);
    if (!then_target || !else_target ||
        !IsRegionInternalEdge(*then_target) ||
        !IsRegionInternalEdge(*else_target)) {
        return false;
    }

    const auto local = LocalConditionFor(terminal.cond);
    bool canonical_edges = canonical_transfer;
    auto edge = [&](ir::Location target, bool fallthrough) {
        if (canonical_edges && FlagsRegsEnabled()) LoadNZCVFromFlags();
        EmitRegionEdge(target, fallthrough, false);
    };
    auto branch = [&](Label* label, bool on_true) {
        if (!EmitDeadEdgeZeroBranch(terminal.cond, label, on_true)) {
            if (local) {
                const auto cond = on_true
                        ? *local
                        : static_cast<Condition>(static_cast<u8>(*local) ^ 1);
                __ B(label, cond);
            } else if (on_true) {
                __ Cbnz(ConditionRegister(terminal.cond), label);
            } else {
                __ Cbz(ConditionRegister(terminal.cond), label);
            }
        }
    };
    if (FlagsRegsEnabled() && flag_state.nzcv_requested == HostFlags::NZCV) {
        if (EmitRegionFlagsJoin(
                    RecipeRegionFlagsJoin(*then_target,
                                        *else_target,
                                        allow_fallthrough),
                    branch)) {
            return true;
        }
    } else {
        if (!canonical_transfer) {
            MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
                      FlagsRegsAuditEdgeKind::RegionInternal);
        }
        // Partial/raw PSTATE must not become a transparent successor's guest flags.
        canonical_edges |= FlagsRegsEnabled() && !BlockIsFlagsTransparent(cur_block);
    }
    context.RecordExecCounter(exec_offset_exit_direct);
    context.RecordExecCounter(exec_offset_region_edges);

    const bool then_layout = allow_fallthrough &&
                             CanUseRegionSuccessorLayout(*then_target);
    const bool else_layout = allow_fallthrough &&
                             CanUseRegionSuccessorLayout(*else_target);
    if (then_layout || else_layout) {
        const auto fall = then_layout ? *then_target : *else_target;
        const auto taken = then_layout ? *else_target : *then_target;
        Label fall_stub;
        branch(&fall_stub, then_layout);
        edge(taken, false);
        __ Bind(&fall_stub);
        edge(fall, true);
        return true;
    }

    Label then_stub;
    branch(&then_stub, true);
    edge(*else_target, false);
    __ Bind(&then_stub);
    edge(*then_target, false);
    return true;
}

bool JitTranslator::EmitRegionCondition(
        const ir::terminal::Condition& terminal,
        bool allow_fallthrough,
        bool canonical_transfer) {
    // EmitTerminal handles constant conditions through the selected ordinary
    // edge, including its commit/poll semantics. Never invert AL or NV here.
    if (ir::IsAlwaysCondition(terminal.cond)) return false;
    const auto then_target = EdgeLeafTarget(terminal.then_);
    const auto else_target = EdgeLeafTarget(terminal.else_);
    if (!then_target || !else_target ||
        !IsRegionInternalEdge(*then_target) ||
        !IsRegionInternalEdge(*else_target)) {
        return false;
    }

    const auto host_cond = MapCond(terminal.cond);
    bool canonical_edges = canonical_transfer;
    auto edge = [&](ir::Location target, bool fallthrough) {
        if (canonical_edges && FlagsRegsEnabled()) LoadNZCVFromFlags();
        EmitRegionEdge(target, fallthrough, false);
    };
    auto branch = [&](Label* label, bool on_true) {
        const auto cond = on_true
                ? host_cond
                : static_cast<Condition>(static_cast<u8>(host_cond) ^ 1);
        __ B(label, cond);
    };
    if (FlagsRegsEnabled() && flag_state.save_in_nzcv && flag_state.nzcv_dirty &&
        flag_state.nzcv_requested == HostFlags::NZCV) {
        if (EmitRegionFlagsJoin(
                    RecipeRegionFlagsJoin(*then_target,
                                        *else_target,
                                        allow_fallthrough),
                    branch)) {
            return true;
        }
    } else if (flag_state.save_in_nzcv && flag_state.nzcv_dirty) {
        MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
                  FlagsRegsAuditEdgeKind::RegionInternal);
        LoadNZCVFromFlags();
        canonical_edges |= FlagsRegsEnabled();
    } else {
        // PF/AF publication is independent of a pending NZCV merge. The
        // canonical handoff must retain a live PF token even when an earlier
        // partial write made this block nontransparent to PSTATE.
        if (!canonical_transfer && FlagsRegsEnabled()) {
            MergeNZCV(FlagsRegsAuditMergeCause::TerminalDispatcher,
                      FlagsRegsAuditEdgeKind::RegionInternal);
        }
        LoadNZCVFromFlags();
    }
    context.RecordExecCounter(exec_offset_exit_direct);
    context.RecordExecCounter(exec_offset_region_edges);

    const bool then_layout = allow_fallthrough &&
                             CanUseRegionSuccessorLayout(*then_target);
    const bool else_layout = allow_fallthrough &&
                             CanUseRegionSuccessorLayout(*else_target);
    if (then_layout || else_layout) {
        const auto fall = then_layout ? *then_target : *else_target;
        const auto taken = then_layout ? *else_target : *then_target;
        Label fall_stub;
        branch(&fall_stub, then_layout);
        edge(taken, false);
        __ Bind(&fall_stub);
        edge(fall, true);
        return true;
    }

    Label then_stub;
    branch(&then_stub, true);
    edge(*else_target, false);
    __ Bind(&then_stub);
    edge(*then_target, false);
    return true;
}
bool JitTranslator::CanonicalCarryEnabled() const {
    return GetSvmConfig().flags_cfinv &&
            True(context.GetConfig().arm64_features & Arm64Features::FlagM);
}

bool JitTranslator::RequiresCanonicalBackedgeEntry(ir::Location target) const {
    if (!CanImportBackedgeFlagsRecipe()) return false;
    // Acceptance can be inherited through a transparent single-successor
    // chain. No such chain may carry pending flags into a split initializer,
    // which reconstructs PSTATE from canonical x26 rather than incoming NZCV.
    // The existing roster bounds cycles without another graph or cache.
    for (size_t remaining = Edges().blocks.size(); remaining != 0; --remaining) {
        const auto* row = Edges().InternalBlock(target.Value());
        if (!row) return false;
        const auto* plan = context.GetActions().BackedgeFor(row->block);
        if (plan && !plan->dead_successor) return true;
        if (!row->transparent) return false;
        const auto next = EdgeLeafTarget(row->block->GetTerminal());
        if (!next) return false;
        target = *next;
    }
    return false;
}

bool JitTranslator::CanImportBackedgeFlagsRecipe() const {
    const auto& space = context.GetModule()->GetAddressSpace();
    const auto* cache = space.GetJitDiskCache();
    // Both disk-cache producers and AOT captures retain the standard ABI.
    // Test the actual storage mode, so module feature overrides cannot enable
    // a private self-entry ABI after the cache was constructed.
    if (space.SerializableCodeEmission() || (cache && cache->Enabled())) return false;
    // FLAGS_REGS reserves the second masked-merge register x12; XPOOL OFF
    // reserves the historical scratch pair. Otherwise retain normal emission.
    return !ScratchXPoolEnabled(context.GetFeatures()) || FlagsRegsEnabled();
}

std::unique_ptr<JitTranslator::BackedgeFlagsRecipe>
JitTranslator::ImportBackedgeFlagsRecipe() {
    if (!CanImportBackedgeFlagsRecipe()) return nullptr;
    const auto& features = context.GetFeatures();
    const auto& actions = context.GetActions();
    const auto* plan = actions.BackedgeFor(cur_block);
    // Deferred PF/AF cold recomputation names x16/x17. They are reserved only
    // with XPOOL OFF; otherwise leave the ordinary SaveFlags work enabled.
    const auto* pfaf = ScratchXPoolEnabled(features) ? nullptr : actions.PFAFFor(cur_block);
    return plan ? std::make_unique<BackedgeFlagsRecipe>(
                          *plan, pfaf, GuestNZCVToHost(plan->Required()))
                : nullptr;
}

void JitTranslator::EmitBackedgeMaterialize(const BackedgeFlagsRecipe& recipe) {
    if (!recipe.plan.canonical_carry) {
        __ Mov(ipw, recipe.plan.carry_inverted);
        __ Strb(ipw,
                MemOperand(state,
                           state_offset_uniform_buffer +
                                   offsetof(swift::x86::ThreadContext64,
                                            carry_inverted)));
    }
    const u64 requested = static_cast<u64>(recipe.requested);
    // x11 is terminal-owned. x16/x17 can hold cross-block SSA under XPOOL,
    // so neither the fixed scratch pair nor the outlined ip1 resume ABI is
    // available here. Full NZCV is three inline instructions; NZV is five.
    // Other noncontiguous masks use the already-reserved x12 (FLAGS_REGS),
    // or the historical reserved pair when XPOOL is disabled.
    const Register* mask_scratch = FlagsRegsEnabled() ? &atomic_scratch : &ip0;
    EmitNZCVMerge(requested, ip,
                  requested == static_cast<u64>(HostFlags::NZCV) ? nullptr : mask_scratch);
}

void JitTranslator::EmitRegionBranchPFAF(const BackedgeFlagsRecipe& recipe) {
    if (!recipe.pfaf) {
        return;
    }
    const auto& plan = *recipe.pfaf;
    ASSERT(plan.width == 1 || plan.width == 2);
    using Deferred = PFAFOperand;
    auto load = [&](const WRegister& dst, const Deferred& operand) {
        const u32 bits = plan.width * 8;
        switch (operand.kind) {
            case Deferred::Kind::Imm: {
                const u32 mask = bits == 32 ? UINT32_MAX : (1u << bits) - 1;
                __ Mov(dst, static_cast<u32>(operand.value) & mask);
                return;
            }
            case Deferred::Kind::HostGPR:
                __ Ubfx(dst, XRegister{static_cast<u32>(operand.value)},
                        operand.offset * 8, bits);
                return;
            case Deferred::Kind::Uniform: {
                const s32 offset = state_offset_uniform_buffer +
                                   static_cast<s32>(operand.value);
                if (plan.width == sizeof(u8)) {
                    __ Ldrb(dst, MemOperand(state, offset));
                } else {
                    __ Ldrh(dst, MemOperand(state, offset));
                }
                return;
            }
            case Deferred::Kind::None:
                PANIC("invalid deferred PF/AF operand");
        }
    };

    // Import enables this deferred recomputation only with XPOOL OFF, where
    // x11/x16/x17 are all reserved from the unit's live value set.
    load(ipw, plan.left);
    load(ipw1, plan.right);
    __ Sub(ipw0, ipw, ipw1);
    u32 begin = context.CurrentBufferSize();
    __ Bfi(flags, ip0, HostFlagsBit::ParityByte, 8);
    RecordPFAFDensity(PFAFDensityKind::PFWrite, begin);

    begin = context.CurrentBufferSize();
    __ Eor(ipw, ipw, ipw0);
    __ Eor(ipw, ipw, ipw1);
    __ Ubfx(ipw, ipw, 4, 1);
    __ Bfi(flags, ip, HostFlagsBit::AuxiliaryCarry, 1);
    RecordPFAFDensity(PFAFDensityKind::AFWrite, begin);
}

bool JitTranslator::EmitBackedgeFlagsTerminal(const ir::Terminal& terminal) {
    if (!backedge_flags_recipe) {
        return false;
    }
    auto& recipe = *backedge_flags_recipe;
    if (!flag_state.save_in_nzcv || !flag_state.nzcv_dirty || flag_state.nzcv_requested != recipe.requested) {
        if (GetSvmConfig().dump_ir) {
            SVM_DIAG_FORMAT(Codegen,
                       "[backedge-proof] {:#x} emitter fallback save={} dirty={} actual={:#x} expected={:#x}\n",
                       cur_block->GetStartLocation().Value(),
                       flag_state.save_in_nzcv,
                       flag_state.nzcv_dirty,
                       static_cast<u64>(flag_state.nzcv_requested),
                       static_cast<u64>(recipe.requested));
        }
        // Static proof and emitter state disagreed. Recreate the omitted
        // polarity write, commit through the ordinary path, and let the
        // generic terminal keep this block correct (but unoptimized).
        if (!recipe.plan.canonical_carry) {
            __ Mov(ipw, recipe.plan.carry_inverted);
            __ Strb(ipw,
                    MemOperand(state,
                               state_offset_uniform_buffer +
                                       offsetof(swift::x86::ThreadContext64,
                                                carry_inverted)));
        }
        MergeNZCV(FlagsRegsAuditMergeCause::TerminalInternal,
                  FlagsRegsAuditEdgeKind::RegionInternal);
        EmitRegionBranchPFAF(recipe);
        recipe.optimized = false;
        return false;
    }

    ir::Value condition{};
    VisitVariant<void>(terminal, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::If>) {
            condition = term.cond;
        }
    });
    if (!condition.Def()) {
        return false;
    }
    if (!backedge_exit_label) backedge_exit_label = std::make_unique<Label>();
    // A cold exit without operand rereads must retain the producer's PF.
    // Commit while its token source is still live, before either branch arm.
    // The hot successor proves incoming flags dead; BFI preserves host NZCV.
    if (!recipe.plan.dead_successor) {
        // A prefix fault on the next iteration must see this producer's PF.
        // Publish before either branch while the SSA source is live. The
        // backedge keeps NZCV pending, but must not retain an expired token.
        PublishFlagsToken();
        InvalidateFlagsToken();
    } else if (!recipe.pfaf && flag_state.flags_token_valid) {
        __ Bfi(flags, FlagsTokenResult(), HostFlagsBit::ParityByte, 8);
    }
    Label self_arm;
    if (auto local = LocalConditionFor(condition)) {
        const auto branch_to_self = recipe.plan.self_is_then
                ? *local : static_cast<Condition>(static_cast<u8>(*local) ^ 1);
        __ B(&self_arm, branch_to_self);
    } else if (recipe.plan.self_is_then) {
        __ Cbnz(ConditionRegister(condition), &self_arm);
    } else {
        __ Cbz(ConditionRegister(condition), &self_arm);
    }
    ASSERT(backedge_exit_label);
    RecordExitPollFault(
            context.PollTerminalExit(recipe.plan.cold_target, backedge_exit_label.get()),
            backedge_exit_label.get(), recipe.plan.cold_target);
    backedge_exit_referenced = true;
    __ B(recipe.cold_exit.get());
    __ Bind(&self_arm);
    recipe.cold_referenced = true;
    if (recipe.plan.dead_successor) {
        backedge_exit_referenced |=
                IsRegionCycleEdge(recipe.plan.cold_target) ||
                IsDirectCycleCutEdge(recipe.plan.cold_target);
        // The target's prefix proves the incoming six arithmetic flags dead
        // before every observer. Keep the pending host NZCV only through the
        // terminal branch; a cycle poll still routes its cold arm through the
        // ordinary backedge stub, which materializes this same recipe.
        EmitRegionEdge(recipe.plan.self_target, false, true, false);
        return true;
    }
    context.RecordExecCounter(exec_offset_exit_direct);
    if (IsRegionInternalEdge(recipe.plan.self_target)) {
        context.RecordExecCounter(exec_offset_region_edges);
        context.RecordExecCounter(exec_offset_region_cycle_polls);
        ++statistics.region_block_edges;
        ++statistics.region_block_cycles;
        statistics.region_block_local_branch_bytes += sizeof(u32);
    }
    backedge_exit_referenced = true;
    const u32 link_before = context.CurrentBufferSize();
    RecordExitPollFault(context.Forward(recipe.plan.self_target,
                                        backedge_exit_label.get(),
                                        LocalBranchTarget(recipe.plan.self_target)),
                        backedge_exit_label.get(),
                        recipe.plan.self_target);
    RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                        context.CurrentBufferSize());
    return true;
}

void JitTranslator::EmitBackedgeColdPaths() {
    if (!backedge_flags_recipe) {
        return;
    }
    auto& recipe = *backedge_flags_recipe;

    if (recipe.plan.dead_successor) {
        if (recipe.optimized && recipe.cold_referenced) {
            __ Bind(recipe.cold_exit.get());
            EmitBackedgeMaterialize(recipe);
            EmitRegionBranchPFAF(recipe);
            if (IsRegionInternalEdge(recipe.plan.cold_target)) {
                // The veneer above has already committed both the carry
                // polarity byte and the requested NZCV bits.  Do not charge
                // the ordinary region-edge merge a second time.
                EmitRegionEdge(recipe.plan.cold_target, false, true, false, false);
            } else {
                context.RecordExecCounter(exec_offset_exit_direct);
                (void) context.Forward(recipe.plan.cold_target,
                                       nullptr,
                                       nullptr,
                                       LinkSiteKind::BackedgeCold);
            }
        }
        flag_state.nzcv_dirty = false;
        flag_state.nzcv_requested = {};
        backedge_flags_recipe.reset();
        return;
    }

    __ Bind(recipe.external_entry.get());
    // All non-self entries begin with committed x26/State. Dispatcher lookup
    // clobbers NZCV. Adjust the committed carry representation to this
    // block's compile-time polarity before reconstructing host NZCV: a fault
    // before the first producer must see the same local ABI on an external
    // first iteration as it does after a self edge.
    if (!recipe.plan.canonical_carry) {
        Label polarity_ready;
        __ Ldrb(ipw,
                MemOperand(state,
                           state_offset_uniform_buffer +
                                   offsetof(swift::x86::ThreadContext64,
                                            carry_inverted)));
        __ Cmp(ipw, recipe.plan.carry_inverted);
        __ B(&polarity_ready, eq);
        __ Eor(flags, flags, static_cast<u64>(HostFlags::C));
        __ Bind(&polarity_ready);
        __ Mov(ipw, recipe.plan.carry_inverted);
        __ Strb(ipw,
                MemOperand(state,
                           state_offset_uniform_buffer +
                                   offsetof(swift::x86::ThreadContext64,
                                            carry_inverted)));
    }
    __ Msr(NZCV, flags);
    __ B(recipe.local_entry.get());

    if (recipe.optimized && recipe.cold_referenced) {
        __ Bind(recipe.cold_exit.get());
        EmitBackedgeMaterialize(recipe);
        if (IsRegionInternalEdge(recipe.plan.cold_target)) {
            // NZCV, polarity and PF are already canonical; do not lease
            // another merge scratch register inside this cold veneer.
            EmitRegionEdge(recipe.plan.cold_target, false, true, false, false);
        } else {
            context.RecordExecCounter(exec_offset_exit_direct);
            (void) context.Forward(recipe.plan.cold_target,
                                   nullptr,
                                   nullptr,
                                   LinkSiteKind::BackedgeCold);
        }
    }

    u32 recovery_offset = 0;
    if (recipe.optimized) {
        recovery_offset = context.CurrentBufferSize();
        __ Bind(recipe.fault_recovery.get());
        EmitBackedgeMaterialize(recipe);
        context.ReturnHost();
    }
    backedge_block_metadata.push_back({cur_block->GetStartLocation().Value(),
                                       backedge_host_begin,
                                       backedge_host_end,
                                       recovery_offset});
    // The next emitted block always starts from the committed ABI. The local
    // state represented by this object has been computed on every edge
    // that can reach it.
    flag_state.nzcv_dirty = false;
    flag_state.nzcv_requested = {};
    backedge_flags_recipe.reset();
}

bool JitTranslator::PreservesHostNZCV(ir::OpCode op) {
    return TargetPreservesHostNZCV(op);
}
bool JitTranslator::RetainsPendingHostNZCV(const ir::Inst& inst) const {
    return TargetRetainsPendingHostNZCV(inst, context.GetFeatures());
}
bool JitTranslator::IsSelfEdge(ir::Location target) const {
    return cur_block && target == cur_block->GetStartLocation();
}

Label* JitTranslator::LocalBranchTarget(ir::Location target) const {
    if (IsSelfEdge(target)) {
        if (loop_hoist_body_entry) {
            return loop_hoist_body_entry.get();
        }
        if (backedge_flags_recipe && !backedge_flags_recipe->plan.dead_successor) {
            return backedge_flags_recipe->local_entry.get();
        }
    }
    return context.GetInternalLabel(target.Value());
}

bool JitTranslator::HasSelfEdge(const ir::Terminal& terminal) const {
    return VisitVariant<bool>(terminal, [this](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                      std::is_same_v<T, ir::terminal::LinkBlockFast>) {
            return IsSelfEdge(term.next);
        } else if constexpr (std::is_same_v<T, ir::terminal::If> ||
                             std::is_same_v<T, ir::terminal::Condition>) {
            return HasSelfEdge(term.then_) || HasSelfEdge(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            return HasSelfEdge(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            return std::any_of(term.cases.begin(), term.cases.end(),
                               [this](const auto& item) {
                                   return HasSelfEdge(item.then);
                               });
        } else {
            return false;
        }
    });
}

Label* JitTranslator::GetTerminalRequestExit() {
    if (!terminal_request_exit) terminal_request_exit = std::make_unique<Label>();
    return terminal_request_exit.get();
}

void JitTranslator::EmitTerminalRequestExitStub() {
    if (!terminal_request_exit) return;
    std::optional<u32> frame_recovery;
    if (std::any_of(memory_state.pending_exit_poll_faults.begin(),
                    memory_state.pending_exit_poll_faults.end(),
                    [&](const auto& fault) {
                        return fault.recovery == terminal_request_exit.get() &&
                               fault.publish_frame;
                    })) {
        frame_recovery = context.CurrentBufferSize();
        ContinuationContract::PublishExternalFrame(masm);
    }
    __ Bind(terminal_request_exit.get());
    ResolveExitPollFaults(terminal_request_exit.get(), frame_recovery);
    if (FlagsRegsEnabled()) EmitSplitFlagsPublish();
    Label reason;
    // FLAGS_REGS=0 disk units have no region-reason relocation. Keep their
    // request-only exit within the movable unit instead of an external B.
    EmitCycleExitReasonTail(&reason, FlagsRegsEnabled());
    terminal_request_exit.reset();
}

void JitTranslator::EmitBackedgeExitStub() {
    if (!backedge_exit_label || !backedge_exit_referenced) {
        backedge_exit_label.reset();
        return;
    }
    Label retirement;
    Label signal;
    Label publish;
    __ Bind(backedge_exit_label.get());
    ResolveExitPollFaults(backedge_exit_label.get());
    const bool shared_reason = translating_function && share_cycle_exit_reason;
    const bool region_reason = context.CanUseRegionTrampoline();
    if (backedge_flags_recipe && backedge_flags_recipe->optimized) {
        EmitBackedgeMaterialize(*backedge_flags_recipe);
        EmitRegionBranchPFAF(*backedge_flags_recipe);
    } else if (FlagsRegsEnabled()) {
        if (TryEmitCycleExitFlags()) {
            return;
        }
        EmitSplitFlagsPublish();
    }
    if (shared_reason) {
        if (!cycle_exit_reason) {
            cycle_exit_reason = std::make_unique<Label>();
        }
        __ B(cycle_exit_reason.get());
        return;
    }
    if (region_reason) {
        context.EmitCycleReasonBranch();
        return;
    }
    __ Ldar(ip0, MemOperand(state, state_offset_exit_request));
    __ Tbnz(ip0, 62, &retirement);
    __ Tbnz(ip0, 63, &signal);
    __ Mov(ipw1, static_cast<u32>(HaltReason::CodeMiss));
    __ B(&publish);
    __ Bind(&signal);
    __ Mov(ipw1, static_cast<u32>(HaltReason::Signal));
    __ B(&publish);
    __ Bind(&retirement);
    __ Mov(ipw1, static_cast<u32>(HaltReason::Retirement));
    __ Bind(&publish);
    __ Str(ipw1, MemOperand(state, state_offset_halt_reason));
    context.ReturnHost();
}

void JitTranslator::EmitDirectCycleExitStubs() {
    if (direct_cycle_exits.empty()) {
        return;
    }
    Label local_reason;
    Label* reason = translating_function && share_cycle_exit_reason
            ? nullptr
            : &local_reason;
    bool reason_referenced{};
    for (auto it = direct_cycle_exits.begin(); it != direct_cycle_exits.end(); ++it) {
        auto& [target, label] = *it;
        ASSERT(label);
        std::optional<u32> frame_recovery;
        if (std::any_of(memory_state.pending_exit_poll_faults.begin(),
                        memory_state.pending_exit_poll_faults.end(),
                        [&](const auto& fault) {
                            return fault.recovery == label.get() && fault.publish_frame;
                        })) {
            frame_recovery = context.CurrentBufferSize();
            // Request branches already pushed their frame and target the common label.
            ContinuationContract::PublishExternalFrame(masm);
        }
        __ Bind(label.get());
        ResolveExitPollFaults(label.get(), frame_recovery);
        if (FlagsRegsEnabled()) {
            if (TryEmitCycleExitFlags()) {
                continue;
            }
            EmitSplitFlagsPublish();
        }
        reason_referenced = true;
        if (!reason) {
            if (!cycle_exit_reason) {
                cycle_exit_reason = std::make_unique<Label>();
            }
            reason = cycle_exit_reason.get();
        }
        if ((translating_function && share_cycle_exit_reason) ||
            std::next(it) != direct_cycle_exits.end()) {
            __ B(reason);
        }
    }
    if ((!translating_function || !share_cycle_exit_reason) &&
        reason_referenced) {
        EmitCycleExitReasonTail(reason);
    }
    direct_cycle_exits.clear();
}

void JitTranslator::EmitCycleExitReasonTail(Label* reason, bool allow_outline) {
    ASSERT(reason);
    __ Bind(reason);
    if (allow_outline && context.CanUseRegionTrampoline()) {
        context.EmitCycleReasonBranch();
        return;
    }
    Label retirement;
    Label signal;
    Label publish;
    __ Ldar(ip0, MemOperand(state, state_offset_exit_request));
    __ Tbnz(ip0, 62, &retirement);
    __ Tbnz(ip0, 63, &signal);
    __ Mov(ipw1, static_cast<u32>(HaltReason::CodeMiss));
    __ B(&publish);
    __ Bind(&signal);
    __ Mov(ipw1, static_cast<u32>(HaltReason::Signal));
    __ B(&publish);
    __ Bind(&retirement);
    __ Mov(ipw1, static_cast<u32>(HaltReason::Retirement));
    __ Bind(&publish);
    __ Str(ipw1, MemOperand(state, state_offset_halt_reason));
    context.ReturnHost();
}

void JitTranslator::RecordExitPollFault(std::optional<JitContext::FaultRange> fault,
                                        Label* recovery,
                                        ir::Location resume_location) {
    if (!fault) {
        return;
    }
    ASSERT(recovery);
    const size_t index = memory_state.fault_metadata.size();
    memory_state.fault_metadata.push_back(
            {resume_location.Value(), fault->begin, fault->end, 0, UINT32_MAX});
    memory_state.pending_exit_poll_faults.push_back({index, recovery, fault->publish_frame});
}

void JitTranslator::ResolveExitPollFaults(Label* recovery, std::optional<u32> frame_recovery) {
    ASSERT(recovery && recovery->IsBound());
    const u32 offset = static_cast<u32>(recovery->GetLocation());
    for (auto it = memory_state.pending_exit_poll_faults.begin();
         it != memory_state.pending_exit_poll_faults.end();) {
        if (it->recovery != recovery) {
            ++it;
            continue;
        }
        ASSERT(it->metadata_index < memory_state.fault_metadata.size());
        ASSERT(!it->publish_frame || frame_recovery.has_value());
        auto& metadata = memory_state.fault_metadata[it->metadata_index];
        metadata.recovery_offset = it->publish_frame ? *frame_recovery : offset;
        it = memory_state.pending_exit_poll_faults.erase(it);
    }
}

}  // namespace swift::runtime::backend::arm64
