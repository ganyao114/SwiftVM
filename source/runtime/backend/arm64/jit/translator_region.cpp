#include "base/logging.h"
#include "translator.h"

#include "runtime/backend/arm64/helper_call_contract.h"

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

void JitTranslator::CollectRegionTargets(const ir::Terminal& terminal,
                                         std::vector<u64>& targets) const {
    VisitVariant<void>(terminal, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                      std::is_same_v<T, ir::terminal::LinkBlockFast>) {
            targets.push_back(term.next.Value());
        } else if constexpr (std::is_same_v<T, ir::terminal::If> ||
                             std::is_same_v<T, ir::terminal::Condition>) {
            CollectRegionTargets(term.then_, targets);
            CollectRegionTargets(term.else_, targets);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            for (const auto& item : term.cases) {
                CollectRegionTargets(item.then, targets);
            }
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            CollectRegionTargets(term.else_, targets);
        }
    });
}

void JitTranslator::PrepareRegionEdges(ir::HIRFunction* function) {
    region_edges_active = context.GetConfig().region_edges;
    region_blocks.clear();
    region_block_map.clear();
    region_cycle_edges.clear();
    region_flags_canonical_stubs.clear();
    if (!region_edges_active) {
        return;
    }

    std::vector<ir::Block*> blocks;
    for (auto& hir_block : function->GetHIRBlocksRPO()) {
        auto* block = hir_block.GetBlock();
        if (block->GetInstList().empty() && !block->HasTerminal()) {
            continue;
        }
        blocks.push_back(block);
        region_blocks.insert(block->GetStartLocation().Value());
        region_block_map.emplace(block->GetStartLocation().Value(), block);
    }
    if (blocks.size() < 2) {
        region_edges_active = false;
        region_blocks.clear();
        region_block_map.clear();
        return;
    }

    if (context.DensityProfileEnabled() && GetSvmConfig().ra_hot_coalesce_all) {
        const u64 unit = function->GetFunction()->GetStartLocation().Value();
        for (auto* block : blocks) {
            std::vector<u64> targets;
            CollectRegionTargets(block->GetTerminal(), targets);
            bool external = targets.empty();
            for (const u64 target : targets) {
                const bool internal = region_blocks.contains(target);
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

    std::map<u64, std::vector<u64>> graph;
    for (auto* block : blocks) {
        const u64 source = block->GetStartLocation().Value();
        auto& successors = graph[source];
        std::vector<u64> targets;
        CollectRegionTargets(block->GetTerminal(), targets);
        for (const u64 target : targets) {
            if (region_blocks.contains(target) &&
                std::find(successors.begin(), successors.end(), target) ==
                        successors.end()) {
                successors.push_back(target);
            }
        }
    }

    enum class Color : u8 { White, Gray, Black };
    std::map<u64, Color> colors;
    std::function<void(u64)> visit = [&](u64 source) {
        colors[source] = Color::Gray;
        for (const u64 target : graph[source]) {
            const auto color = colors.contains(target) ? colors[target] : Color::White;
            if (color == Color::Gray) {
                // DFS 回边覆盖每个有向环；非成环边不承担 safepoint 税。
                region_cycle_edges.emplace(source, target);
            } else if (color == Color::White) {
                visit(target);
            }
        }
        colors[source] = Color::Black;
    };
    for (auto* block : blocks) {
        const u64 location = block->GetStartLocation().Value();
        if (!colors.contains(location)) {
            visit(location);
        }
    }
}

std::optional<ir::Location>
JitTranslator::RegionLeafTarget(const ir::Terminal& terminal) const {
    std::optional<ir::Location> result;
    VisitVariant<void>(terminal, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                      std::is_same_v<T, ir::terminal::LinkBlockFast>) {
            result = term.next;
        }
    });
    return result;
}

bool JitTranslator::IsRegionInternalEdge(ir::Location target) const {
    return region_edges_active && region_blocks.contains(target.Value());
}

bool JitTranslator::IsRegionCycleEdge(ir::Location target) const {
    return region_edges_active && cur_block &&
           region_cycle_edges.contains(
                   {cur_block->GetStartLocation().Value(), target.Value()});
}

bool JitTranslator::HasRegionCycleEdgeFromCurrent() const {
    if (!region_edges_active || !cur_block) {
        return false;
    }
    const u64 source = cur_block->GetStartLocation().Value();
    return std::any_of(region_cycle_edges.begin(), region_cycle_edges.end(),
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

u32 JitTranslator::CountCycleExitCandidates(
        std::span<ir::Block* const> blocks) const {
    u32 count{};
    for (auto* block : blocks) {
        std::vector<u64> targets;
        CollectRegionTargets(block->GetTerminal(), targets);
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (u64 target : targets) {
            count += IsDirectCycleCutEdge(block->GetStartLocation(),
                                          ir::Location{target});
        }
    }
    std::vector<u64> local_cycle_sources;
    local_cycle_sources.reserve(region_cycle_edges.size());
    for (const auto& edge : region_cycle_edges) {
        local_cycle_sources.push_back(edge.first);
    }
    std::sort(local_cycle_sources.begin(), local_cycle_sources.end());
    local_cycle_sources.erase(
            std::unique(local_cycle_sources.begin(), local_cycle_sources.end()),
            local_cycle_sources.end());
    count += static_cast<u32>(local_cycle_sources.size());
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
        target.Value() >= cur_block->GetStartLocation().Value()) {
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
                                   bool commit_flags) {
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
            backedge_flags_recipe->dead_successor && IsDirectCycleCutEdge(target);
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
    RecordExitPollFault(
            context.ForwardLocal(target,
                                 cycle_exit,
                                 fallthrough,
                                 fallthrough ? nullptr : LocalBranchTarget(target)),
            cycle_exit);
    RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                        context.CurrentBufferSize());
}

bool JitTranslator::BlockIsFlagsTransparent(ir::Block* block) const {
    if (!block) {
        return false;
    }
    for (auto& inst : block->GetInstList()) {
        switch (inst.GetOp()) {
            case ir::OpCode::GetFlags:
            case ir::OpCode::CallLambda:
            case ir::OpCode::CallLocation:
            case ir::OpCode::CallDynamic:
            case ir::OpCode::X87Op:
            case ir::OpCode::TestFlags:
            case ir::OpCode::TestNotFlags:
            case ir::OpCode::SaveFlags:
            case ir::OpCode::BranchOnlyFlags:
            case ir::OpCode::SetCarry:
            case ir::OpCode::SetOverflow:
            case ir::OpCode::ClearFlags:
            case ir::OpCode::InvertCarry:
            case ir::OpCode::Adc:
            case ir::OpCode::Sbb:
            case ir::OpCode::CondSelect:
            case ir::OpCode::CondSet:
                return false;
            default:
                break;
        }
    }
    return true;
}

bool JitTranslator::EmitRegionIf(const ir::terminal::If& terminal,
                                 bool allow_fallthrough) {
    const auto then_target = RegionLeafTarget(terminal.then_);
    const auto else_target = RegionLeafTarget(terminal.else_);
    if (!then_target || !else_target ||
        !IsRegionInternalEdge(*then_target) ||
        !IsRegionInternalEdge(*else_target)) {
        return false;
    }

    const auto local = LocalConditionFor(terminal.cond);
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
    if (FlagsRegsEnabled() && True(flag_state.nzcv_requested)) {
        if (EmitRegionFlagsJoin(
                    RecipeRegionFlagsJoin(*then_target,
                                        *else_target,
                                        allow_fallthrough),
                    branch)) {
            return true;
        }
    } else {
        MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
                  FlagsRegsAuditEdgeKind::RegionInternal);
    }
    auto needs_stub = [&](ir::Location target) {
        return IsRegionCycleEdge(target) || IsDirectCycleCutEdge(target);
    };

    context.RecordExecCounter(exec_offset_exit_direct);
    context.RecordExecCounter(exec_offset_region_edges);

    const bool then_layout = allow_fallthrough &&
                             CanUseRegionSuccessorLayout(*then_target) &&
                             !needs_stub(*else_target);
    const bool else_layout = allow_fallthrough &&
                             CanUseRegionSuccessorLayout(*else_target) &&
                             !needs_stub(*then_target);
    if (then_layout || else_layout) {
        const auto fall = then_layout ? *then_target : *else_target;
        const auto taken = then_layout ? *else_target : *then_target;
        ASSERT(!needs_stub(taken));
        branch(LocalBranchTarget(taken), !then_layout);
        ++statistics.region_block_edges;
        EmitRegionEdge(fall, !needs_stub(fall), false);
        return true;
    }

    Label then_stub;
    const bool stub = needs_stub(*then_target);
    branch(stub ? &then_stub : LocalBranchTarget(*then_target),
           true);
    if (!stub) {
        ++statistics.region_block_edges;
    }
    EmitRegionEdge(*else_target, false, false);
    if (stub) {
        __ Bind(&then_stub);
        EmitRegionEdge(*then_target, false, false);
    }
    return true;
}

bool JitTranslator::EmitRegionCondition(
        const ir::terminal::Condition& terminal,
        bool allow_fallthrough) {
    const auto then_target = RegionLeafTarget(terminal.then_);
    const auto else_target = RegionLeafTarget(terminal.else_);
    if (!then_target || !else_target ||
        !IsRegionInternalEdge(*then_target) ||
        !IsRegionInternalEdge(*else_target)) {
        return false;
    }

    const auto host_cond = MapCond(terminal.cond);
    auto branch = [&](Label* label, bool on_true) {
        const auto cond = on_true
                ? host_cond
                : static_cast<Condition>(static_cast<u8>(host_cond) ^ 1);
        __ B(label, cond);
    };
    if (FlagsRegsEnabled() && flag_state.save_in_nzcv && flag_state.nzcv_dirty &&
        True(flag_state.nzcv_requested)) {
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
    } else {
        LoadNZCVFromFlags();
    }
    auto needs_stub = [&](ir::Location target) {
        return IsRegionCycleEdge(target) || IsDirectCycleCutEdge(target);
    };

    context.RecordExecCounter(exec_offset_exit_direct);
    context.RecordExecCounter(exec_offset_region_edges);

    const bool then_layout = allow_fallthrough &&
                             CanUseRegionSuccessorLayout(*then_target) &&
                             !needs_stub(*else_target);
    const bool else_layout = allow_fallthrough &&
                             CanUseRegionSuccessorLayout(*else_target) &&
                             !needs_stub(*then_target);
    if (then_layout || else_layout) {
        const auto fall = then_layout ? *then_target : *else_target;
        const auto taken = then_layout ? *else_target : *then_target;
        ASSERT(!needs_stub(taken));
        branch(LocalBranchTarget(taken), !then_layout);
        ++statistics.region_block_edges;
        EmitRegionEdge(fall, !needs_stub(fall), false);
        return true;
    }

    Label then_stub;
    const bool stub = needs_stub(*then_target);
    branch(stub ? &then_stub : LocalBranchTarget(*then_target),
           true);
    if (!stub) {
        ++statistics.region_block_edges;
    }
    EmitRegionEdge(*else_target, false, false);
    if (stub) {
        __ Bind(&then_stub);
        EmitRegionEdge(*then_target, false, false);
    }
    return true;
}
bool JitTranslator::CanonicalCarryEnabled() const {
    return GetSvmConfig().flags_cfinv &&
            True(context.GetConfig().arm64_features & Arm64Features::FlagM);
}

std::optional<JitTranslator::BackedgeCarryRecipe>
JitTranslator::RecipeBackedgeCarry(ir::Block* block, ir::Inst* final_save,
                                 ir::Inst* condition, bool dead_successor) {
    BackedgeCarryRecipe recipe{.canonical = CanonicalCarryEnabled(),
                           .marker = final_save};
    if (recipe.canonical) {
        for (auto& inst : block->GetInstList()) {
            if (inst.Id() > final_save->Id() &&
                inst.Id() < condition->Id() &&
                inst.GetOp() == ir::OpCode::InvertCarry) {
                recipe.marker = &inst;
            }
        }
        return recipe;
    }

    constexpr u32 kCarryOffset = offsetof(swift::x86::ThreadContext64,
                                          carry_inverted);
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() <= final_save->Id() ||
            inst.GetOp() != ir::OpCode::StoreUniform) {
            continue;
        }
        const auto uniform = inst.GetArg<ir::Uniform>(0);
        if (uniform.GetOffset() != kCarryOffset ||
            uniform.GetType() != ir::ValueType::U8) {
            continue;
        }
        auto value = inst.GetArg<ir::Value>(1);
        auto* def = value.Def();
        if (!def || def->GetOp() != ir::OpCode::LoadImm ||
            value.Type() != ir::ValueType::U8 ||
            (!dead_successor && def->GetUses() != 1)) {
            if (GetSvmConfig().dump_ir) {
                SVM_DIAG_FORMAT(Codegen,
                           "[backedge-proof] {:#x} reject polarity value def={} op={} type={} uses={}\n",
                           block->GetStartLocation().Value(), def != nullptr,
                           def ? static_cast<u32>(def->GetOp()) : UINT32_MAX,
                           static_cast<u32>(value.Type()),
                           def ? def->GetUses() : UINT32_MAX);
            }
            return std::nullopt;
        }
        const u64 immediate = def->GetArg<ir::Imm>(0).Get();
        if (immediate > 1) {
            return std::nullopt;
        }
        recipe.store = &inst;
        recipe.load = def;
        recipe.marker = &inst;
        recipe.inverted = static_cast<u8>(immediate);
    }
    if (!recipe.store || !recipe.load || recipe.store->Id() >= condition->Id()) {
        if (GetSvmConfig().dump_ir) {
            SVM_DIAG_FORMAT(Codegen,
                       "[backedge-proof] {:#x} reject polarity store={} load={} cond={}\n",
                       block->GetStartLocation().Value(),
                       recipe.store ? recipe.store->Id() : UINT32_MAX,
                       recipe.load ? recipe.load->Id() : UINT32_MAX,
                       condition->Id());
        }
        return std::nullopt;
    }
    return recipe;
}

std::unique_ptr<JitTranslator::BackedgeFlagsRecipe>
JitTranslator::RecipeBackedgeFlags(ir::Block* block) {
    if ((!backedge_flags && !region_branch_flags) || !block) {
        return nullptr;
    }

    std::optional<ir::Location> then_target;
    std::optional<ir::Location> else_target;
    ir::Value condition{};
    VisitVariant<void>(block->GetTerminal(), [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::If>) {
            condition = term.cond;
            auto direct = [](const ir::Terminal& edge) -> std::optional<ir::Location> {
                return VisitVariant<std::optional<ir::Location>>(
                        edge, [](const auto& target) -> std::optional<ir::Location> {
                            using E = std::decay_t<decltype(target)>;
                            if constexpr (std::is_same_v<E, ir::terminal::LinkBlock> ||
                                          std::is_same_v<E, ir::terminal::LinkBlockFast>) {
                                return target.next;
                            }
                            return std::nullopt;
                        });
            };
            then_target = direct(term.then_);
            else_target = direct(term.else_);
        }
    });
    if (!then_target || !else_target || !condition.Def() ||
        condition.Def()->GetOp() != ir::OpCode::LocalCondSet) {
        return nullptr;
    }
    const auto self = block->GetStartLocation();
    const bool then_self = *then_target == self;
    const bool else_self = *else_target == self;
    const bool then_dead = region_branch_flags &&
                           IsRegionInternalEdge(*then_target) &&
                           TargetKillsIncomingFlags(*then_target);
    const bool else_dead = region_branch_flags &&
                           IsRegionInternalEdge(*else_target) &&
                           TargetKillsIncomingFlags(*else_target);
    const bool dead_successor = then_dead != else_dead;
    if (!dead_successor && (!backedge_flags || then_self == else_self)) {
        return nullptr;
    }

    ir::Inst* first_producer = nullptr;
    ir::Inst* final_save = nullptr;
    ir::Flags final_requested{};
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() != ir::OpCode::SaveFlags) {
            continue;
        }
        auto* producer = inst.GetArg<ir::Value>(0).Def();
        if (!producer) {
            return nullptr;
        }
        if (!first_producer || producer->Id() < first_producer->Id()) {
            first_producer = producer;
        }
        const auto requested = inst.GetArg<ir::Flags>(1);
        if (True(requested & ir::Flags::Parity) &&
            True(requested & ir::Flags::AuxiliaryCarry) &&
            True(requested & ir::Flags::NZCV)) {
            final_save = &inst;
            final_requested = requested;
        }
    }
    if (!first_producer || !final_save) {
        if (GetSvmConfig().dump_ir) {
            SVM_DIAG_FORMAT(Codegen, "[backedge-proof] {:#x} reject flags producer\n",
                       block->GetStartLocation().Value());
        }
        return nullptr;
    }

    auto carry_recipe = RecipeBackedgeCarry(
            block, final_save, condition.Def(), dead_successor);
    if (!carry_recipe) {
        return nullptr;
    }

    if (!dead_successor) {
        // The old block-entry flags stay live until the first producer. A
        // fault is allowed in that prefix; the self-recipe recovery veneer
        // reconstructs them.
        for (auto& inst : block->GetInstList()) {
            if (&inst == first_producer) {
                break;
            }
            if (!RetainsPendingHostNZCV(inst)) {
                if (GetSvmConfig().dump_ir) {
                    SVM_DIAG_FORMAT(Codegen,
                               "[backedge-proof] {:#x} reject pre-producer op={} id={}\n",
                               block->GetStartLocation().Value(),
                               static_cast<u32>(inst.GetOp()), inst.Id());
                }
                return nullptr;
            }
        }
    }
    // Once the next producer overwrites host NZCV there may be no synchronous
    // fault or architectural observer before the terminal safepoint.
    auto* lazy_producer = dead_successor
            ? final_save->GetArg<ir::Value>(0).Def()
            : first_producer;
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() <= lazy_producer->Id()) {
            continue;
        }
        const auto helper = HelperCallContract::Resolve(inst, context.GetFeatures());
        if (MayFaultOrObserve(inst) || (helper && !helper->RetainsPendingNZCV())) {
            if (GetSvmConfig().dump_ir) {
                SVM_DIAG_FORMAT(Codegen,
                           "[backedge-proof] {:#x} reject post-producer fault op={} id={} producer={}\n",
                           block->GetStartLocation().Value(),
                           static_cast<u32>(inst.GetOp()), inst.Id(), lazy_producer->Id());
            }
            return nullptr;
        }
    }
    // The tail after the omitted store is intentionally tiny: advancing the
    // guest PC and consuming the already-live local condition only.
    ir::Inst* final_advance = nullptr;
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() <= carry_recipe->marker->Id()) {
            continue;
        }
        if (inst.GetOp() != ir::OpCode::AdvancePC &&
            inst.GetOp() != ir::OpCode::LocalCondSet &&
            inst.GetOp() != ir::OpCode::ZeroExtend32 &&
            inst.GetOp() != ir::OpCode::ZeroExtend32To64 &&
            inst.GetOp() != ir::OpCode::SetHostGPR) {
            if (GetSvmConfig().dump_ir) {
                SVM_DIAG_FORMAT(Codegen,
                           "[backedge-proof] {:#x} reject tail op={} id={}\n",
                           block->GetStartLocation().Value(),
                           static_cast<u32>(inst.GetOp()), inst.Id());
            }
            return nullptr;
        }
        if (inst.GetOp() == ir::OpCode::AdvancePC) {
            if (final_advance) {
                return nullptr;
            }
            final_advance = &inst;
        }
    }
    if (!final_advance || final_advance->Id() >= condition.Def()->Id()) {
        return nullptr;
    }

    auto recipe = std::make_unique<BackedgeFlagsRecipe>();
    recipe->dead_successor = dead_successor;
    recipe->canonical_carry = carry_recipe->canonical;
    recipe->self_is_then = dead_successor ? then_dead : then_self;
    recipe->self_target = dead_successor
            ? (then_dead ? *then_target : *else_target)
            : self;
    recipe->cold_target = recipe->self_is_then ? *else_target : *then_target;
    recipe->carry_inverted = carry_recipe->inverted;
    recipe->requested = GuestNZCVToHost(final_requested & ir::Flags::NZCV);
    recipe->polarity_load = carry_recipe->load && carry_recipe->load->GetUses() == 1
            ? carry_recipe->load
            : nullptr;
    recipe->polarity_store = carry_recipe->store;
    recipe->final_save = final_save;
    recipe->final_advance = final_advance;
    (void)RecipeRegionBranchPFAF(*recipe, lazy_producer);
    if (GetSvmConfig().dump_ir) {
        SVM_DIAG_FORMAT(Codegen,
                   "[backedge-proof] {:#x} eligible mode={} hot={:#x} cold={:#x} pfaf={}\n",
                   block->GetStartLocation().Value(),
                   dead_successor ? "region-dead" : "self-lazy",
                   recipe->self_target.Value(), recipe->cold_target.Value(),
                   recipe->defer_pfaf);
    }
    return recipe;
}

void JitTranslator::EmitBackedgeMaterialize(const BackedgeFlagsRecipe& recipe) {
    if (!recipe.canonical_carry) {
        __ Mov(ipw1, recipe.carry_inverted);
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
    if (!recipe.defer_pfaf) {
        return;
    }
    ASSERT_MSG(ReproveRegionBranchPFAF(),
               "region branch PF/AF operand proof drifted before cold emission");
    using Deferred = BackedgeFlagsRecipe::DeferredOperand;
    auto load = [&](const WRegister& dst, const Deferred& operand) {
        const u32 bits = recipe.pfaf_width * 8;
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
                if (recipe.pfaf_width == sizeof(u8)) {
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
    load(ipw, recipe.pfaf_left);
    load(ipw1, recipe.pfaf_right);
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
        if (!recipe.canonical_carry) {
            __ Mov(ipw1, recipe.carry_inverted);
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
    if (auto local = LocalConditionFor(condition)) {
        const auto branch_to_cold = recipe.self_is_then
                ? static_cast<Condition>(static_cast<u8>(*local) ^ 1)
                : *local;
        __ B(backedge_flags_recipe->cold_exit.get(), branch_to_cold);
    } else if (recipe.self_is_then) {
        __ Cbz(context.W(condition), backedge_flags_recipe->cold_exit.get());
    } else {
        __ Cbnz(context.W(condition), backedge_flags_recipe->cold_exit.get());
    }
    recipe.cold_referenced = true;
    if (recipe.dead_successor) {
        backedge_exit_referenced |=
                IsRegionCycleEdge(recipe.cold_target) ||
                IsDirectCycleCutEdge(recipe.cold_target);
        // The target's prefix proves the incoming six arithmetic flags dead
        // before every observer. Keep the pending host NZCV only through the
        // terminal branch; a cycle poll still routes its cold arm through the
        // ordinary backedge stub, which materializes this same recipe.
        EmitRegionEdge(recipe.self_target, false, true, false);
        return true;
    }
    MaterializeFlagsTokenResult();
    context.RecordExecCounter(exec_offset_exit_direct);
    if (IsRegionInternalEdge(recipe.self_target)) {
        context.RecordExecCounter(exec_offset_region_edges);
        context.RecordExecCounter(exec_offset_region_cycle_polls);
        ++statistics.region_block_edges;
        ++statistics.region_block_cycles;
        statistics.region_block_local_branch_bytes += sizeof(u32);
    }
    backedge_exit_referenced = true;
    const u32 link_before = context.CurrentBufferSize();
    RecordExitPollFault(
            context.Forward(recipe.self_target,
                            backedge_exit_label.get(),
                            LocalBranchTarget(recipe.self_target)),
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

    if (recipe.dead_successor) {
        if (recipe.optimized && recipe.cold_referenced) {
            __ Bind(recipe.cold_exit.get());
            EmitBackedgeMaterialize(recipe);
            EmitRegionBranchPFAF(recipe);
            if (IsRegionInternalEdge(recipe.cold_target)) {
                // The veneer above has already committed both the carry
                // polarity byte and the requested NZCV bits.  Do not charge
                // the ordinary region-edge merge a second time.
                EmitRegionEdge(recipe.cold_target, false, true, false);
            } else {
                context.RecordExecCounter(exec_offset_exit_direct);
                (void) context.Forward(recipe.cold_target,
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
    if (!recipe.canonical_carry) {
        Label polarity_ready;
        __ Ldrb(ipw0,
                MemOperand(state,
                           state_offset_uniform_buffer +
                                   offsetof(swift::x86::ThreadContext64,
                                            carry_inverted)));
        __ Cmp(ipw0, recipe.carry_inverted);
        __ B(&polarity_ready, eq);
        __ Eor(flags, flags, static_cast<u64>(HostFlags::C));
        __ Bind(&polarity_ready);
        __ Mov(ipw0, recipe.carry_inverted);
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
        if (IsRegionInternalEdge(recipe.cold_target)) {
            EmitRegionEdge(recipe.cold_target);
        } else {
            context.RecordExecCounter(exec_offset_exit_direct);
            (void) context.Forward(recipe.cold_target,
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
    // Deliberately narrow emitter audit. These are the only pre-producer
    // operations admitted by the first spike; every listed ARM64 lowering is
    // flag-neutral (including its address arithmetic and CBNZ/TBZ guards).
    switch (op) {
        case ir::OpCode::LoadUniform:
        case ir::OpCode::StoreUniform:
        case ir::OpCode::LoadMemory:
        case ir::OpCode::StoreMemory:
        case ir::OpCode::GetHostGPR:
        case ir::OpCode::GetHostFPR:
        case ir::OpCode::SetHostGPR:
        case ir::OpCode::SetHostFPR:
        case ir::OpCode::LoadImm:
        case ir::OpCode::AdvancePC:
        case ir::OpCode::BitCast:
        case ir::OpCode::GetOperand:
        case ir::OpCode::Zero:
        case ir::OpCode::ZeroExtend32:
        case ir::OpCode::ZeroExtend32To64:
        case ir::OpCode::VecFAdd:
        case ir::OpCode::VecFSub:
        case ir::OpCode::VecFMul:
        // These lower to their non-S forms when no surviving SaveFlags pseudo
        // names them. The proof stops at the first producer that does have
        // such a pseudo, so earlier dead-flags pointer arithmetic is neutral.
        case ir::OpCode::Add:
        case ir::OpCode::Sub:
            return true;
        default:
            return false;
    }
}

bool JitTranslator::RetainsPendingHostNZCV(const ir::Inst& inst) const {
    const auto helper = HelperCallContract::Resolve(inst, context.GetFeatures());
    return helper ? helper->RetainsPendingNZCV()
                  : PreservesHostNZCV(inst.GetOp());
}

bool JitTranslator::MayFaultOrObserve(const ir::Inst& inst) const {
    return guest_state_map.MayFaultOrObserve(inst);
}

bool JitTranslator::MayFaultOrObserve(ir::OpCode op) {
    return GuestStateMap::MayFaultOrObserve(op);
}

bool JitTranslator::IsSelfEdge(ir::Location target) const {
    return cur_block && target == cur_block->GetStartLocation();
}

Label* JitTranslator::LocalBranchTarget(ir::Location target) const {
    if (IsSelfEdge(target)) {
        if (loop_hoist_body_entry) {
            return loop_hoist_body_entry.get();
        }
        if (backedge_flags_recipe && !backedge_flags_recipe->dead_successor) {
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

void JitTranslator::EmitBackedgeExitStub() {
    if (!backedge_exit_label || !backedge_exit_referenced) {
        backedge_exit_label.reset();
        return;
    }
    Label signal;
    Label publish;
    __ Bind(backedge_exit_label.get());
    ResolveExitPollFaults(backedge_exit_label.get(),
                          cur_block->GetStartLocation());
    const bool shared_reason = translating_function && share_cycle_exit_reason;
    const bool region_reason = context.CanUseRegionTrampoline();
    if (!shared_reason && !region_reason) {
        __ Ldar(ip0, MemOperand(state, state_offset_exit_request));
    }
    if (backedge_flags_recipe && backedge_flags_recipe->optimized) {
        EmitBackedgeMaterialize(*backedge_flags_recipe);
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
    __ Tbnz(ip0, 63, &signal);
    __ Mov(ipw1, static_cast<u32>(HaltReason::CodeMiss));
    __ B(&publish);
    __ Bind(&signal);
    __ Mov(ipw1, static_cast<u32>(HaltReason::Signal));
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
        __ Bind(label.get());
        ResolveExitPollFaults(label.get(), ir::Location{target});
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

void JitTranslator::EmitCycleExitReasonTail(Label* reason) {
    ASSERT(reason);
    __ Bind(reason);
    if (context.CanUseRegionTrampoline()) {
        context.EmitCycleReasonBranch();
        return;
    }
    Label signal;
    Label publish;
    __ Ldar(ip0, MemOperand(state, state_offset_exit_request));
    __ Tbnz(ip0, 63, &signal);
    __ Mov(ipw1, static_cast<u32>(HaltReason::CodeMiss));
    __ B(&publish);
    __ Bind(&signal);
    __ Mov(ipw1, static_cast<u32>(HaltReason::Signal));
    __ Bind(&publish);
    __ Str(ipw1, MemOperand(state, state_offset_halt_reason));
    context.ReturnHost();
}

void JitTranslator::RecordExitPollFault(
        std::optional<JitContext::FaultRange> fault,
        Label* recovery) {
    if (!fault) {
        return;
    }
    ASSERT(recovery);
    const size_t index = memory_state.fault_metadata.size();
    memory_state.fault_metadata.push_back({cur_block->GetStartLocation().Value(),
                              fault->begin,
                              fault->end,
                              0,
                              UINT32_MAX});
    memory_state.pending_exit_poll_faults.push_back({index, recovery});
}

void JitTranslator::ResolveExitPollFaults(Label* recovery,
                                          ir::Location resume_location) {
    ASSERT(recovery && recovery->IsBound());
    const u32 offset = static_cast<u32>(recovery->GetLocation());
    for (auto it = memory_state.pending_exit_poll_faults.begin();
         it != memory_state.pending_exit_poll_faults.end();) {
        if (it->recovery != recovery) {
            ++it;
            continue;
        }
        ASSERT(it->metadata_index < memory_state.fault_metadata.size());
        auto& metadata = memory_state.fault_metadata[it->metadata_index];
        metadata.guest_start = resume_location.Value();
        metadata.recovery_offset = offset;
        it = memory_state.pending_exit_poll_faults.erase(it);
    }
}

}  // namespace swift::runtime::backend::arm64
