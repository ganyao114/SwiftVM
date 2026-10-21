#include "runtime/backend/arm64/alloc/target_analysis_traits.h"
#include "base/logging.h"
#include "translator.h"
#include "runtime/backend/arm64/alloc/edge_flags_analysis.h"

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
#include "runtime/backend/arm64/defines.h"
#include "runtime/common/backedge_control.h"
#include "runtime/common/svm_config.h"
#include "translator/x86/cpu.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

bool JitTranslator::ImportRegionPreparation(ir::Block* block, ir::HIRFunction* function) {
    const ir::Block* first = block;
    if (function) {
        if (function->GetHIRBlocksRPO().empty()) return false;
        first = function->GetHIRBlocksRPO().front().GetBlock();
    }
    if (!context.RequireGPRPlan(first)) return false;
    const auto actions = context.GetAllocation().GPRPlanFor(first);
    if (!actions || !actions->region ||
        !actions->region->MatchesUnit(block, function, context.GetAllocation(),
                                      context.GetConfig(), context.GetFeatures())) return false;
    region_plan_owner = actions->region;
    region_function_mode = function != nullptr;
    canonical_entries_active = region_function_mode;
    return true;
}

void JitTranslator::ReportRegionEdges() {
    if (!RegionEdgesActive()) return;
    if (context.DensityProfileEnabled() && GetSvmConfig().ra_hot_coalesce_all) {
        const u64 unit = RegionPlan().root_location;
        for (const auto& [location, facts] : RegionBlockMap()) {
            auto* block = facts.block;
            std::vector<u64> targets;
            CollectRegionTargets(block->GetTerminal(), targets);
            bool external = targets.empty();
            for (const u64 target : targets) {
                const bool internal = RegionPlan().ContainsInternalBlock(target);
                external |= !internal;
                SVM_DIAG_PRINT(Codegen,
                             "[svm-gap-cfg-edge] unit=0x%llx block=0x%llx "
                             "target=0x%llx internal=%u\n",
                             static_cast<unsigned long long>(unit),
                             static_cast<unsigned long long>(
                                     block->GetStartLocation().Value()),
                             static_cast<unsigned long long>(target),
                             internal ? 1u : 0u);
            }
            // CheckHalt also has an implicit interrupt exit not represented by
            // its `else_` target. Treat it as an observing edge in the audit.
            VisitVariant<void>(block->GetTerminal(), [&](const auto& term) {
                using T = std::decay_t<decltype(term)>;
                if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
                    external = true;
                }
            });
            SVM_DIAG_PRINT(Codegen,
                         "[svm-gap-cfg] unit=0x%llx block=0x%llx external=%u\n",
                         static_cast<unsigned long long>(unit),
                         static_cast<unsigned long long>(
                                 block->GetStartLocation().Value()),
                         external ? 1u : 0u);
        }
    }

}

bool JitTranslator::IsRegionInternalEdge(ir::Location target) const {
    return RegionEdgesActive() && RegionPlan().ContainsInternalBlock(target.Value());
}

bool JitTranslator::IsRegionCycleEdge(ir::Location target) const {
    return RegionEdgesActive() && cur_block &&
           RegionCycleEdges().contains(
                   {cur_block->GetStartLocation().Value(), target.Value()});
}

bool JitTranslator::HasRegionCycleEdgeFromCurrent() const {
    if (!RegionEdgesActive() || !cur_block) {
        return false;
    }
    const u64 source = cur_block->GetStartLocation().Value();
    return std::any_of(RegionCycleEdges().begin(), RegionCycleEdges().end(),
                       [&](const auto& edge) { return edge.first == source; });
}

bool JitTranslator::IsDirectCycleCutEdge(ir::Location source,
                                         ir::Location target) const {
    if (!direct_cycle_latch || target.Value() >= source.Value()) {
        return false;
    }
    // PrepareRegionEdges covers internal cycles with exact DFS backedges.
    // The total-order cut is only needed for linkable external edges.
    if (IsRegionInternalEdge(target)) {
        return false;
    }
    return context.CanBypassDispatcher(target);
}

bool JitTranslator::IsDirectCycleCutEdge(ir::Location target) const {
    return cur_block &&
           IsDirectCycleCutEdge(cur_block->GetStartLocation(), target);
}

u32 JitTranslator::CountCycleExitCandidates() const {
    const auto& plan = RegionPlan();
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
           !backedge_exit_label && !backedge_flags_recipe &&
           vec_nan_cold_sites.empty();
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
        if (FlagsRegsEnabled()) {
            MaterializeFlagsTokenResult();
        } else {
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
    RecordExitPollFault(
            context.ForwardLocal(target,
                                 cycle_exit,
                                 fallthrough,
                                 fallthrough ? nullptr : LocalBranchTarget(target),
                                 request_exit),
            cycle_exit ? cycle_exit : request_exit,
            cycle_exit ? std::nullopt : std::optional<ir::Location>{target});
    RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                        context.CurrentBufferSize());
}

bool JitTranslator::BlockIsFlagsTransparent(ir::Block* block) const {
    if (!block) return false;
    const auto found = RegionBlockMap().find(block->GetStartLocation().Value());
    return found != RegionBlockMap().end() ? found->second.transparent : block == cur_block && current_flags_transparent;
}

bool JitTranslator::EmitRegionIf(const ir::terminal::If& terminal,
                                 bool allow_fallthrough,
                                 bool canonical_transfer) {
    const auto then_target = RegionLeafTarget(terminal.then_);
    const auto else_target = RegionLeafTarget(terminal.else_);
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
                __ Cbnz(context.W(terminal.cond), label);
            } else {
                __ Cbz(context.W(terminal.cond), label);
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
    const auto then_target = RegionLeafTarget(terminal.then_);
    const auto else_target = RegionLeafTarget(terminal.else_);
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
        if (!canonical_transfer && FlagsRegsEnabled() && BlockIsFlagsTransparent(cur_block)) {
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

std::unique_ptr<JitTranslator::BackedgeFlagsRecipe>
JitTranslator::ImportBackedgeFlagsRecipe() {
    const auto& plan = gpr_plan->backedge_flags;
    if (!plan) return nullptr;
    return std::make_unique<BackedgeFlagsRecipe>(plan, GuestNZCVToHost(plan->requested));
}

void JitTranslator::EmitBackedgeMaterialize(const BackedgeFlagsRecipe& recipe) {
    if (!recipe.plan.canonical_carry) {
        __ Mov(ipw1, recipe.plan.carry_inverted);
        __ Strb(ipw1,
                MemOperand(state,
                           state_offset_uniform_buffer +
                                   offsetof(swift::x86::ThreadContext64,
                                            carry_inverted)));
    }
    const u64 requested = static_cast<u64>(recipe.requested);
    if (requested == static_cast<u64>(HostFlags::NZCV) &&
        context.CanUseRegionTrampoline()) {
        EmitOutlinedNZCVMergeResume(false);
    } else {
        EmitNZCVMerge(requested, ip0);
    }
}

void JitTranslator::EmitRegionBranchPFAF(const BackedgeFlagsRecipe& recipe) {
    if (!recipe.plan.pfaf) {
        return;
    }
    const auto& plan = *recipe.plan.pfaf;
    ASSERT(plan.width == 1 || plan.width == 2);
    using Deferred = RegionPFAFPlan::Operand;
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

    // x11 is terminal-owned and is already excluded from the live value set;
    // x16/x17 are the backend's fixed scratch pair. The cold recomputation is
    // therefore invisible to RA and cannot extend a hot SSA interval.
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
            __ Mov(ipw1, recipe.plan.carry_inverted);
            __ Strb(ipw1,
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
    if (recipe.plan.dead_successor && !recipe.plan.pfaf &&
        flag_state.flags_token_valid) {
        __ Bfi(flags, FlagsTokenResult(), HostFlagsBit::ParityByte, 8);
    }
    Label self_arm;
    if (auto local = LocalConditionFor(condition)) {
        const auto branch_to_self = recipe.plan.self_is_then
                ? *local : static_cast<Condition>(static_cast<u8>(*local) ^ 1);
        __ B(&self_arm, branch_to_self);
    } else if (recipe.plan.self_is_then) {
        __ Cbnz(context.W(condition), &self_arm);
    } else {
        __ Cbz(context.W(condition), &self_arm);
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
    MaterializeFlagsTokenResult();
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
    RecordExitPollFault(
            context.Forward(recipe.plan.self_target,
                            backedge_exit_label.get(),
                            LocalBranchTarget(recipe.plan.self_target)),
            backedge_exit_label.get());
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
        __ Ldrb(ipw0,
                MemOperand(state,
                           state_offset_uniform_buffer +
                                   offsetof(swift::x86::ThreadContext64,
                                            carry_inverted)));
        __ Cmp(ipw0, recipe.plan.carry_inverted);
        __ B(&polarity_ready, eq);
        __ Eor(flags, flags, static_cast<u64>(HostFlags::C));
        __ Bind(&polarity_ready);
        __ Mov(ipw0, recipe.plan.carry_inverted);
        __ Strb(ipw0,
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
            EmitRegionEdge(recipe.plan.cold_target, false, true, true, false);
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
bool JitTranslator::MayFaultOrObserve(const ir::Inst& inst) const {
    return TargetMayFaultOrObserve(inst, context.GetFeatures());
}

bool JitTranslator::MayFaultOrObserve(ir::OpCode op) {
    return TargetMayFaultOrObserve(op);
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
    ResolveExitPollFaults(terminal_request_exit.get(),
                          cur_block->GetStartLocation(), frame_recovery);
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
    ResolveExitPollFaults(backedge_exit_label.get(),
                          cur_block->GetStartLocation());
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
        ResolveExitPollFaults(label.get(), ir::Location{target}, frame_recovery);
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

void JitTranslator::RecordExitPollFault(
        std::optional<JitContext::FaultRange> fault,
        Label* recovery,
        std::optional<ir::Location> guest_start) {
    if (!fault) {
        return;
    }
    ASSERT(recovery);
    const size_t index = memory_state.fault_metadata.size();
    memory_state.fault_metadata.push_back({
            guest_start ? guest_start->Value() : cur_block->GetStartLocation().Value(),
            fault->begin,
            fault->end,
            0,
            UINT32_MAX});
    memory_state.pending_exit_poll_faults.push_back(
            {index, recovery, fault->publish_frame, guest_start.has_value()});
}

void JitTranslator::ResolveExitPollFaults(Label* recovery,
                                          ir::Location resume_location,
                                          std::optional<u32> frame_recovery) {
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
        if (!it->guest_final) {
            metadata.guest_start = resume_location.Value();
        }
        metadata.recovery_offset = it->publish_frame ? *frame_recovery : offset;
        it = memory_state.pending_exit_poll_faults.erase(it);
    }
}

}  // namespace swift::runtime::backend::arm64
