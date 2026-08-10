#include "translator.h"

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
                std::fprintf(stderr,
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
            std::fprintf(stderr,
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

bool JitTranslator::IsDirectCycleCutEdge(ir::Location target) const {
    if (!direct_cycle_latch || !cur_block ||
        target.Value() >= cur_block->GetStartLocation().Value()) {
        return false;
    }
    // Guest block starts form a total order, so every non-self directed cycle
    // has at least one descending edge. This deterministic cut is independent
    // of translation/cache order; it may conservatively cover acyclic backward
    // jumps. A cross-region cycle's only descending edge may itself be local to
    // one region without forming a region-local cycle, so region mode must cover
    // both local and external descending edges. Exact DFS cycle edges do not pay
    // twice: EmitRegionEdge selects the DFS exit before requesting this cut.
    // External direct edges can stay in JIT only when BlockLink owns both ends.
    return IsRegionInternalEdge(target) || context.CanBypassDispatcher(target);
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

bool JitTranslator::CanRegionFallThrough(ir::Location target) const {
    return next_region_block && *next_region_block == target.Value() &&
           !backedge_exit_label && !backedge_flags_plan &&
           !IsDirectCycleCutEdge(target) &&
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
        MergeNZCV(FlagsRegsAuditMergeCause::TerminalInternal,
                  FlagsRegsAuditEdgeKind::RegionInternal);
    }
    if (record_edge_counters) {
        context.RecordExecCounter(exec_offset_exit_direct);
        context.RecordExecCounter(exec_offset_region_edges);
    }
    ++region_block_edges;
    const bool region_cycle = IsRegionCycleEdge(target);
    const bool pending_flags_cycle = !commit_flags && backedge_flags_plan &&
            backedge_flags_plan->dead_successor && IsDirectCycleCutEdge(target);
    auto* ordered_cycle_exit = region_cycle || pending_flags_cycle
            ? nullptr
            : GetDirectCycleExit(target);
    const bool cycle = region_cycle || pending_flags_cycle || ordered_cycle_exit;
    if (cycle) {
        ASSERT(region_cycle || pending_flags_cycle
                       ? backedge_exit_label != nullptr
                       : ordered_cycle_exit != nullptr);
        context.RecordExecCounter(exec_offset_region_cycle_polls);
        if (region_cycle) {
            backedge_exit_referenced = true;
        }
        ++region_block_cycles;
    }
    if (fallthrough) {
        context.RecordExecCounter(exec_offset_region_fallthroughs);
        ++region_block_fallthroughs;
    } else {
        region_block_local_branch_bytes += sizeof(u32);
    }
    const u32 link_before = context.CurrentBufferSize();
    context.ForwardLocal(target,
                         region_cycle || pending_flags_cycle
                                 ? backedge_exit_label.get()
                                 : ordered_cycle_exit,
                         fallthrough,
                         fallthrough ? nullptr : LocalBranchTarget(target));
    RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                        context.CurrentBufferSize());
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
    // MergeNZCV 只使用 MRS/AND/ORR，不改 host NZCV；因此可在条件判定前提交一次。
    MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
              FlagsRegsAuditEdgeKind::RegionInternal);
    auto branch = [&](Label* label, bool on_true) {
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
    };
    auto needs_stub = [&](ir::Location target) {
        return IsRegionCycleEdge(target) || IsDirectCycleCutEdge(target);
    };

    context.RecordExecCounter(exec_offset_exit_direct);
    context.RecordExecCounter(exec_offset_region_edges);

    const bool then_fallthrough = allow_fallthrough &&
                                  CanRegionFallThrough(*then_target) &&
                                  !needs_stub(*else_target);
    const bool else_fallthrough = allow_fallthrough &&
                                  CanRegionFallThrough(*else_target) &&
                                  !needs_stub(*then_target);
    if (then_fallthrough || else_fallthrough) {
        const auto fall = then_fallthrough ? *then_target : *else_target;
        const auto taken = then_fallthrough ? *else_target : *then_target;
        ASSERT(!needs_stub(taken));
        branch(LocalBranchTarget(taken), !then_fallthrough);
        ++region_block_edges;
        EmitRegionEdge(fall, true, false);
        return true;
    }

    Label then_stub;
    const bool stub = needs_stub(*then_target);
    branch(stub ? &then_stub : LocalBranchTarget(*then_target),
           true);
    if (!stub) {
        ++region_block_edges;
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
    // 与 EmitRegionIf 相同，提交 flags 的指令保持当前 NZCV，条件可直接复用。
    if (save_in_nzcv && nzcv_dirty) {
        MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
                  FlagsRegsAuditEdgeKind::RegionInternal);
    } else {
        LoadNZCVFromFlags();
    }
    auto branch = [&](Label* label, bool on_true) {
        const auto cond = on_true
                ? host_cond
                : static_cast<Condition>(static_cast<u8>(host_cond) ^ 1);
        __ B(label, cond);
    };
    auto needs_stub = [&](ir::Location target) {
        return IsRegionCycleEdge(target) || IsDirectCycleCutEdge(target);
    };

    context.RecordExecCounter(exec_offset_exit_direct);
    context.RecordExecCounter(exec_offset_region_edges);

    const bool then_fallthrough = allow_fallthrough &&
                                  CanRegionFallThrough(*then_target) &&
                                  !needs_stub(*else_target);
    const bool else_fallthrough = allow_fallthrough &&
                                  CanRegionFallThrough(*else_target) &&
                                  !needs_stub(*then_target);
    if (then_fallthrough || else_fallthrough) {
        const auto fall = then_fallthrough ? *then_target : *else_target;
        const auto taken = then_fallthrough ? *else_target : *then_target;
        ASSERT(!needs_stub(taken));
        branch(LocalBranchTarget(taken), !then_fallthrough);
        ++region_block_edges;
        EmitRegionEdge(fall, true, false);
        return true;
    }

    Label then_stub;
    const bool stub = needs_stub(*then_target);
    branch(stub ? &then_stub : LocalBranchTarget(*then_target),
           true);
    if (!stub) {
        ++region_block_edges;
    }
    EmitRegionEdge(*else_target, false, false);
    if (stub) {
        __ Bind(&then_stub);
        EmitRegionEdge(*then_target, false, false);
    }
    return true;
}
std::unique_ptr<JitTranslator::BackedgeFlagsPlan>
JitTranslator::PlanBackedgeFlags(ir::Block* block) {
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
            fmt::print(stderr, "[backedge-proof] {:#x} reject flags producer\n",
                       block->GetStartLocation().Value());
        }
        return nullptr;
    }

    ir::Inst* polarity_store = nullptr;
    ir::Inst* polarity_load = nullptr;
    u8 polarity = 0;
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
                fmt::print(stderr,
                           "[backedge-proof] {:#x} reject polarity value def={} op={} type={} uses={}\n",
                           block->GetStartLocation().Value(),
                           def != nullptr,
                           def ? static_cast<u32>(def->GetOp()) : UINT32_MAX,
                           static_cast<u32>(value.Type()),
                           def ? def->GetUses() : UINT32_MAX);
            }
            return nullptr;
        }
        const u64 immediate = def->GetArg<ir::Imm>(0).Get();
        if (immediate > 1) {
            return nullptr;
        }
        polarity_store = &inst;
        polarity_load = def;
        polarity = static_cast<u8>(immediate);
    }
    if (!polarity_store || !polarity_load ||
        polarity_store->Id() >= condition.Def()->Id()) {
        if (GetSvmConfig().dump_ir) {
            fmt::print(stderr,
                       "[backedge-proof] {:#x} reject polarity store={} load={} cond={}\n",
                       block->GetStartLocation().Value(),
                       polarity_store ? polarity_store->Id() : UINT32_MAX,
                       polarity_load ? polarity_load->Id() : UINT32_MAX,
                       condition.Def()->Id());
        }
        return nullptr;
    }

    if (!dead_successor) {
        // The old block-entry flags stay live until the first producer. A
        // fault is allowed in that prefix; the self-plan recovery veneer
        // reconstructs them.
        for (auto& inst : block->GetInstList()) {
            if (&inst == first_producer) {
                break;
            }
            if (!PreservesHostNZCV(inst.GetOp())) {
                if (GetSvmConfig().dump_ir) {
                    fmt::print(stderr,
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
        if (inst.Id() > lazy_producer->Id() && MayFaultOrObserve(inst.GetOp())) {
            if (GetSvmConfig().dump_ir) {
                fmt::print(stderr,
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
        if (inst.Id() <= polarity_store->Id()) {
            continue;
        }
        if (inst.GetOp() != ir::OpCode::AdvancePC &&
            inst.GetOp() != ir::OpCode::LocalCondSet &&
            inst.GetOp() != ir::OpCode::ZeroExtend32 &&
            inst.GetOp() != ir::OpCode::ZeroExtend32To64 &&
            inst.GetOp() != ir::OpCode::SetHostGPR) {
            if (GetSvmConfig().dump_ir) {
                fmt::print(stderr,
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

    auto plan = std::make_unique<BackedgeFlagsPlan>();
    plan->dead_successor = dead_successor;
    plan->self_is_then = dead_successor ? then_dead : then_self;
    plan->self_target = dead_successor
            ? (then_dead ? *then_target : *else_target)
            : self;
    plan->cold_target = plan->self_is_then ? *else_target : *then_target;
    plan->carry_inverted = polarity;
    plan->requested = GuestNZCVToHost(final_requested & ir::Flags::NZCV);
    plan->polarity_load = polarity_load->GetUses() == 1 ? polarity_load : nullptr;
    plan->polarity_store = polarity_store;
    plan->final_save = final_save;
    plan->final_advance = final_advance;
    (void)PlanRegionBranchPFAF(*plan, lazy_producer);
    if (GetSvmConfig().dump_ir) {
        fmt::print(stderr,
                   "[backedge-proof] {:#x} eligible mode={} hot={:#x} cold={:#x} pfaf={}\n",
                   block->GetStartLocation().Value(),
                   dead_successor ? "region-dead" : "self-lazy",
                   plan->self_target.Value(), plan->cold_target.Value(),
                   plan->defer_pfaf);
    }
    return plan;
}

void JitTranslator::EmitBackedgeMaterialize(const BackedgeFlagsPlan& plan) {
    __ Mov(ipw1, plan.carry_inverted);
    __ Strb(ipw1,
            MemOperand(state,
                       state_offset_uniform_buffer +
                               offsetof(swift::x86::ThreadContext64,
                                        carry_inverted)));
    const u64 requested = static_cast<u64>(plan.requested);
    u64 keep = ~requested;
    __ Mrs(ip0, NZCV);
    __ And(flags, flags, ForceCast<s64>(keep));
    __ And(ip0, ip0, static_cast<u32>(requested));
    __ Orr(flags, flags, ip0);
}

void JitTranslator::EmitRegionBranchPFAF(const BackedgeFlagsPlan& plan) {
    if (!plan.defer_pfaf) {
        return;
    }
    ASSERT_MSG(ReproveRegionBranchPFAF(),
               "region branch PF/AF operand proof drifted before cold emission");
    using Deferred = BackedgeFlagsPlan::DeferredOperand;
    auto load = [&](const WRegister& dst, const Deferred& operand) {
        const u32 bits = plan.pfaf_width * 8;
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
                if (plan.pfaf_width == sizeof(u8)) {
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
    load(ipw, plan.pfaf_left);
    load(ipw1, plan.pfaf_right);
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
    if (!backedge_flags_plan) {
        return false;
    }
    auto& plan = *backedge_flags_plan;
    if (!save_in_nzcv || !nzcv_dirty || nzcv_requested != plan.requested) {
        if (GetSvmConfig().dump_ir) {
            fmt::print(stderr,
                       "[backedge-proof] {:#x} emitter fallback save={} dirty={} actual={:#x} expected={:#x}\n",
                       cur_block->GetStartLocation().Value(),
                       save_in_nzcv,
                       nzcv_dirty,
                       static_cast<u64>(nzcv_requested),
                       static_cast<u64>(plan.requested));
        }
        // Static proof and emitter state disagreed. Recreate the omitted
        // polarity write, commit through the ordinary path, and let the
        // generic terminal keep this block correct (but unoptimized).
        __ Mov(ipw1, plan.carry_inverted);
        __ Strb(ipw1,
                MemOperand(state,
                           state_offset_uniform_buffer +
                                   offsetof(swift::x86::ThreadContext64,
                                            carry_inverted)));
        MergeNZCV(FlagsRegsAuditMergeCause::TerminalInternal,
                  FlagsRegsAuditEdgeKind::RegionInternal);
        EmitRegionBranchPFAF(plan);
        plan.optimized = false;
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
        const auto branch_to_cold = plan.self_is_then
                ? static_cast<Condition>(static_cast<u8>(*local) ^ 1)
                : *local;
        __ B(backedge_flags_plan->cold_exit.get(), branch_to_cold);
    } else if (plan.self_is_then) {
        __ Cbz(context.W(condition), backedge_flags_plan->cold_exit.get());
    } else {
        __ Cbnz(context.W(condition), backedge_flags_plan->cold_exit.get());
    }
    plan.cold_referenced = true;
    if (plan.dead_successor) {
        // The target's prefix proves the incoming six arithmetic flags dead
        // before every observer. Keep the pending host NZCV only through the
        // terminal branch; a cycle poll still routes its cold arm through the
        // ordinary backedge stub, which materializes this same plan.
        EmitRegionEdge(plan.self_target, false, true, false);
        return true;
    }
    context.RecordExecCounter(exec_offset_exit_direct);
    if (IsRegionInternalEdge(plan.self_target)) {
        context.RecordExecCounter(exec_offset_region_edges);
        context.RecordExecCounter(exec_offset_region_cycle_polls);
        ++region_block_edges;
        ++region_block_cycles;
        region_block_local_branch_bytes += sizeof(u32);
    }
    backedge_exit_referenced = true;
    const u32 link_before = context.CurrentBufferSize();
    context.Forward(plan.self_target,
                    backedge_exit_label.get(),
                    LocalBranchTarget(plan.self_target));
    RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                        context.CurrentBufferSize());
    return true;
}

void JitTranslator::EmitBackedgeColdPaths() {
    if (!backedge_flags_plan) {
        return;
    }
    auto& plan = *backedge_flags_plan;

    if (plan.dead_successor) {
        if (plan.optimized && plan.cold_referenced) {
            __ Bind(plan.cold_exit.get());
            EmitBackedgeMaterialize(plan);
            EmitRegionBranchPFAF(plan);
            if (IsRegionInternalEdge(plan.cold_target)) {
                // The veneer above has already committed both the carry
                // polarity byte and the requested NZCV bits.  Do not charge
                // the ordinary region-edge merge a second time.
                EmitRegionEdge(plan.cold_target, false, true, false);
            } else {
                context.RecordExecCounter(exec_offset_exit_direct);
                context.Forward(plan.cold_target,
                                nullptr,
                                nullptr,
                                LinkSiteKind::BackedgeCold);
            }
        }
        nzcv_dirty = false;
        nzcv_requested = {};
        backedge_flags_plan.reset();
        return;
    }

    __ Bind(plan.external_entry.get());
    // All non-self entries begin with committed x26/State. Dispatcher lookup
    // clobbers NZCV. Normalize the committed carry representation to this
    // block's compile-time polarity before reconstructing host NZCV: a fault
    // before the first producer must see the same local ABI on an external
    // first iteration as it does after a self edge.
    Label polarity_ready;
    __ Ldrb(ipw0,
            MemOperand(state,
                       state_offset_uniform_buffer +
                               offsetof(swift::x86::ThreadContext64,
                                        carry_inverted)));
    __ Cmp(ipw0, plan.carry_inverted);
    __ B(&polarity_ready, eq);
    __ Eor(flags, flags, static_cast<u64>(HostFlags::C));
    __ Bind(&polarity_ready);
    __ Mov(ipw0, plan.carry_inverted);
    __ Strb(ipw0,
            MemOperand(state,
                       state_offset_uniform_buffer +
                               offsetof(swift::x86::ThreadContext64,
                                        carry_inverted)));
    __ And(ip0, flags, static_cast<u64>(HostFlags::NZCV));
    __ Msr(NZCV, ip0);
    __ B(plan.local_entry.get());

    if (plan.optimized && plan.cold_referenced) {
        __ Bind(plan.cold_exit.get());
        EmitBackedgeMaterialize(plan);
        if (IsRegionInternalEdge(plan.cold_target)) {
            EmitRegionEdge(plan.cold_target);
        } else {
            context.RecordExecCounter(exec_offset_exit_direct);
            context.Forward(plan.cold_target,
                            nullptr,
                            nullptr,
                            LinkSiteKind::BackedgeCold);
        }
    }

    u32 recovery_offset = 0;
    if (plan.optimized) {
        recovery_offset = context.CurrentBufferSize();
        __ Bind(plan.fault_recovery.get());
        EmitBackedgeMaterialize(plan);
        __ Ret();
    }
    backedge_block_metadata.push_back({cur_block->GetStartLocation().Value(),
                                       backedge_host_begin,
                                       backedge_host_end,
                                       recovery_offset});
    // The next emitted block always starts from the committed ABI. The local
    // state represented by this object has been materialized on every edge
    // that can reach it.
    nzcv_dirty = false;
    nzcv_requested = {};
    backedge_flags_plan.reset();
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

bool JitTranslator::MayFaultOrObserve(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::LoadMemory:
        case ir::OpCode::StoreMemory:
        case ir::OpCode::LoadMemoryTSO:
        case ir::OpCode::StoreMemoryTSO:
        case ir::OpCode::MemoryCopy:
        case ir::OpCode::MemoryCopyTSO:
        case ir::OpCode::CompareAndSwap:
        case ir::OpCode::CompareAndSwap128:
        case ir::OpCode::CheckMemoryAlignment:
        case ir::OpCode::AtomicExchange:
        case ir::OpCode::AtomicFetchAdd:
        case ir::OpCode::AtomicRMW:
        case ir::OpCode::CallLambda:
        case ir::OpCode::CallLocation:
        case ir::OpCode::CallDynamic:
        case ir::OpCode::X87Op:
        case ir::OpCode::Sse42Str:
            return true;
        default:
            return false;
    }
}

bool JitTranslator::TargetKillsIncomingFlags(ir::Location target) const {
    const auto found = region_block_map.find(target.Value());
    if (found == region_block_map.end() || !found->second) {
        return false;
    }

    ir::Flags incoming = ir::Flags::All;
    bool killed = false;
    for (const auto& inst : found->second->GetInstList()) {
        // A synchronous observer before the complete overwrite would expose
        // the source block's still-uncommitted NZCV/polarity recipe.
        // After the overwrite, a fault is still unsafe until AdvancePC has
        // committed the target instruction's replacement flags.
        if (MayFaultOrObserve(inst.GetOp())) {
            return false;
        }
        if (killed) {
            if (inst.GetOp() == ir::OpCode::AdvancePC) {
                return true;
            }
            continue;
        }
        switch (inst.GetOp()) {
            case ir::OpCode::TestFlags:
            case ir::OpCode::TestNotFlags:
                if (True(incoming & inst.GetArg<ir::Flags>(0))) return false;
                break;
            case ir::OpCode::GetFlags:
                return false;
            case ir::OpCode::Adc:
            case ir::OpCode::Sbb:
            case ir::OpCode::InvertCarry:
                if (True(incoming & ir::Flags::Carry)) return false;
                break;
            case ir::OpCode::CondSelect:
            case ir::OpCode::CondSet:
            case ir::OpCode::LocalCondSet:
                if (True(incoming & ir::Flags::NZCV)) return false;
                break;
            case ir::OpCode::Goto:
            case ir::OpCode::NotGoto:
            case ir::OpCode::BindLabel:
                // Do not prove path-sensitive kills inside a block.
                return false;
            case ir::OpCode::SaveFlags:
                incoming &= ~inst.GetArg<ir::Flags>(1);
                break;
            case ir::OpCode::ClearFlags:
                incoming &= ~inst.GetArg<ir::Flags>(0);
                break;
            case ir::OpCode::SetCarry:
                incoming &= ~ir::Flags::Carry;
                break;
            case ir::OpCode::SetOverflow:
                incoming &= ~ir::Flags::Overflow;
                break;
            case ir::OpCode::PublishFCmpFlags:
            case ir::OpCode::BranchOnlyFlags:
                incoming = ir::Flags::None;
                break;
            default:
                break;
        }
        killed = incoming == ir::Flags::None;
    }
    return false;
}



bool JitTranslator::IsSelfEdge(ir::Location target) const {
    return cur_block && target == cur_block->GetStartLocation();
}

Label* JitTranslator::LocalBranchTarget(ir::Location target) const {
    if (IsSelfEdge(target)) {
        if (loop_hoist_body_entry) {
            return loop_hoist_body_entry.get();
        }
        if (backedge_flags_plan && !backedge_flags_plan->dead_successor) {
            return backedge_flags_plan->local_entry.get();
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
    if (backedge_flags_plan && backedge_flags_plan->optimized) {
        EmitBackedgeMaterialize(*backedge_flags_plan);
    }
    __ Mov(ip1, cur_block->GetStartLocation().Value());
    __ Str(ip1, MemOperand(state, state_offset_current_loc));
    __ Tbnz(ip0, 63, &signal);
    __ Mov(ipw1, static_cast<u32>(HaltReason::CodeMiss));
    __ B(&publish);
    __ Bind(&signal);
    __ Mov(ipw1, static_cast<u32>(HaltReason::Signal));
    __ Bind(&publish);
    __ Str(ipw1, MemOperand(state, state_offset_halt_reason));
    __ Ret();
    backedge_exit_label.reset();
    backedge_exit_referenced = false;
}

void JitTranslator::EmitDirectCycleExitStubs() {
    for (auto& [target, label] : direct_cycle_exits) {
        ASSERT(label);
        Label signal;
        Label publish;
        __ Bind(label.get());
        // The poll runs after MergeNZCV and FlushSpillWrites. Publish the edge
        // target so resuming after the guest signal continues at the committed
        // terminal boundary rather than repeating the source block.
        __ Mov(ip1, target);
        __ Str(ip1, MemOperand(state, state_offset_current_loc));
        __ Tbnz(ip0, 63, &signal);
        __ Mov(ipw1, static_cast<u32>(HaltReason::CodeMiss));
        __ B(&publish);
        __ Bind(&signal);
        __ Mov(ipw1, static_cast<u32>(HaltReason::Signal));
        __ Bind(&publish);
        __ Str(ipw1, MemOperand(state, state_offset_halt_reason));
        __ Ret();
    }
    direct_cycle_exits.clear();
}

}  // namespace swift::runtime::backend::arm64
