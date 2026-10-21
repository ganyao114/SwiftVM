#include "target_allocation_planner.h"
#include "runtime/common/perf_stats.h"
#include "target_analysis_traits.h"
#include "gpr_plan_verifier.h"
#include "zero_store_verifier.h"
#include "base/scope_exit.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/gpr_coalescing_contract.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/common/svm_config.h"
#include "runtime/common/variant_util.h"
#include "aarch64/assembler-aarch64.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <type_traits>
#include <unordered_map>

namespace swift::runtime::backend::arm64 {
using vixl::aarch64::XRegister;
namespace {


// Env-gated Bind census (SVM_BINDCENSUS): counts Bind candidates by class
// and how many changed, to attribute residual target replays. Zero output
// and only a few increments when unset.
struct BindCensus {
    unsigned long long calls{};
    unsigned long long changed_calls{};
    unsigned long long transfer_candidates{};
    unsigned long long transfer_changed{};
    unsigned long long narrow_candidates{};
    unsigned long long narrow_changed{};
    unsigned long long writer_candidates{};
    unsigned long long writer_changed{};
    unsigned long long operand_candidates{};
    unsigned long long operand_changed{};
    unsigned long long narrow_no_old{};
    unsigned long long narrow_old_preserved{};
    unsigned long long narrow_old_home{};
    unsigned long long narrow_old_end{};
    unsigned long long narrow_old_elided{};
    unsigned long long narrow_old_ok{};
    unsigned long long replay_skipped{};
    unsigned long long fail_calls{};
    unsigned long long fail_prepare{};
    unsigned long long fail_transfer{};
    unsigned long long fail_narrow{};
    unsigned long long fail_writer{};
    unsigned long long fail_operand{};
    unsigned long long fail_none_found{};
};
BindCensus g_bind_census;
bool g_bind_census_registered{};
bool BindCensusEnabled() {
    static const bool enabled = std::getenv("SVM_BINDCENSUS") != nullptr;
    if (enabled && !g_bind_census_registered) {
        g_bind_census_registered = true;
        std::atexit([] {
            const auto& c = g_bind_census;
            std::fprintf(stderr,
                "[svm-bindcensus] calls=%llu changed_calls=%llu transfer=%llu/%llu "
                "narrow=%llu/%llu writer=%llu/%llu operand=%llu/%llu "
                "narrow_reasons no_old=%llu preserved=%llu home=%llu end=%llu elided=%llu ok=%llu replay_skipped=%llu "
                "fail_calls=%llu fail_prepare=%llu fail_transfer=%llu fail_narrow=%llu fail_writer=%llu fail_operand=%llu fail_none=%llu\n",
                c.calls, c.changed_calls, c.transfer_changed, c.transfer_candidates,
                c.narrow_changed, c.narrow_candidates, c.writer_changed,
                c.writer_candidates, c.operand_changed, c.operand_candidates,
                c.narrow_no_old, c.narrow_old_preserved, c.narrow_old_home,
                c.narrow_old_end, c.narrow_old_elided, c.narrow_old_ok, c.replay_skipped,
                c.fail_calls, c.fail_prepare, c.fail_transfer, c.fail_narrow,
                c.fail_writer, c.fail_operand, c.fail_none_found);
        });
    }
    return enabled;
}

ir::Value ResolveHostCoalesceBitCast(ir::Value value) {
    while (value.Defined() && value.Def()->IsBitCastOperation()) {
        value = value.Def()->GetArg<ir::Value>(0);
    }
    return value;
}

bool IsKnownHostWWrite(ir::Value value);

bool IsHostCoalesceProducer(ir::OpCode op, bool width_chain) {
    return IsGPRPublicationProducer(op, width_chain);
}

bool IsWidthChainHostWWrite(ir::Value value, u32 target,
                            const FeatureSet& features) {
    value = ResolveHostCoalesceBitCast(value);
    if (!value.Defined()) {
        return false;
    }
    auto* def = value.Def();
    if (def->GetOp() == ir::OpCode::GetHostGPR) {
        return features.ra_width_chain &&
               ir::GetValueSizeByte(def->ReturnType()) == sizeof(u32) &&
               def->GetArg<ir::Imm>(1).Get() == 0 &&
               def->GetArg<ir::Imm>(0).Get() != target;
    }
    return IsKnownHostWWrite(value);
}


bool IsHostCoalesceObserver(const ir::Inst& inst, const FeatureSet& features) {
    return IsFixedGPRPublicationObserver(inst, features);
}

bool IsKnownHostWWrite(ir::Value value) {
    value = ResolveHostCoalesceBitCast(value);
    if (!value.Defined()) {
        return false;
    }
    auto* def = value.Def();
    if (def->GetOp() == ir::OpCode::GetHostGPR) {
        return false;
    }
    if (def->GetOp() == ir::OpCode::LoadImm) {
        // See HasKnownWWrite: a 32-bit-fitting constant is a W write.
        return (def->GetArg<ir::Imm>(0).Get() >> 32) == 0;
    }
    if (def->GetOp() == ir::OpCode::BitExtract &&
        ir::GetValueSizeByte(def->ReturnType()) == sizeof(u32) &&
        def->GetArg<ir::Imm>(1).Get() == 0 &&
        def->GetArg<ir::Imm>(2).Get() == 32) {
        return IsKnownHostWWrite(def->GetArg<ir::Value>(0));
    }
    if (def->GetOp() == ir::OpCode::ZeroExtend32To64 &&
        ir::GetValueSizeByte(def->GetArg<ir::Value>(0).Type()) == sizeof(u32)) {
        return IsKnownHostWWrite(def->GetArg<ir::Value>(0));
    }
    // See HasKnownWWrite: narrow unsigned loads materialize as W writes.
    if ((def->GetOp() == ir::OpCode::LoadMemory ||
         def->GetOp() == ir::OpCode::LoadMemoryTSO) &&
        (def->ReturnType() == ir::ValueType::U8 ||
         def->ReturnType() == ir::ValueType::U16)) {
        return true;
    }
    return ir::GetValueSizeByte(def->ReturnType()) == sizeof(u32);
}

bool TerminalUsesHostCoalesceValue(const ir::Terminal& terminal_value,
                                   ir::Inst* definition) {
    return VisitVariant<bool>(terminal_value, [&](const auto& edge) {
        using T = std::decay_t<decltype(edge)>;
        if constexpr (std::is_same_v<T, ir::terminal::If>) {
            return ResolveHostCoalesceBitCast(edge.cond).Def() == definition ||
                   TerminalUsesHostCoalesceValue(edge.then_, definition) ||
                   TerminalUsesHostCoalesceValue(edge.else_, definition);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            if (ResolveHostCoalesceBitCast(edge.value).Def() == definition) {
                return true;
            }
            return std::any_of(edge.cases.begin(), edge.cases.end(),
                               [&](const auto& item) {
                                   return TerminalUsesHostCoalesceValue(item.then, definition);
                               });
        } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
            return TerminalUsesHostCoalesceValue(edge.then_, definition) ||
                   TerminalUsesHostCoalesceValue(edge.else_, definition);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            return TerminalUsesHostCoalesceValue(edge.else_, definition);
        }
        return false;
    });
}



using ::swift::runtime::backend::IsFixedGPRHome;

ir::Value ResolveBitCast(ir::Value value) {
    while (value.Defined() && value.Def()->IsBitCastOperation()) {
        value = value.Def()->GetArg<ir::Value>(0);
    }
    return value;
}

std::optional<s64> ImmediateValue(const ir::Operand& operand) {
    if (operand.GetRight().IsImm()) {
        return operand.GetRight().imm.GetSigned();
    }
    if (operand.IsImm()) {
        return operand.GetLeft().imm.GetSigned();
    }
    if (!operand.GetRight().Null() || !operand.GetLeft().IsValue()) {
        return std::nullopt;
    }
    auto value = ResolveBitCast(operand.GetLeft().value);
    if (!value.Def() || value.Def()->GetOp() != ir::OpCode::LoadImm) {
        return std::nullopt;
    }
    return static_cast<s64>(value.Def()->GetArg<ir::Imm>(0).Get());
}



bool IsPinnedMemoryTarget(u32 target) {
    return target <= 9 || target == 19 || target == 20 || target == 21 ||
           target == 22 || target == 23 || target == 29;
}

bool IsCallerSavedAddressBarrier(ir::OpCode op) {
    using O = ir::OpCode;
    return op == O::CallLambda || op == O::CallLocation ||
           op == O::CallDynamic || op == O::X87Op || op == O::Sse42Str;
}


} // namespace

ARMTargetPlanner::PendingGPRBlock ARMTargetPlanner::DeclareGPRBlock(ir::Block* block) {
    PerfScope2 perf_declare{GetPerfStats2().plan_declare};
    cur_block = block;
    building_gpr_plan = std::make_shared<GPRBlockPlan>();
    gpr_plan = building_gpr_plan.get();
    {
        PerfScope2 perf_guestmap{GetPerfStats2().plan_guestmap};
        guest_state_map.Analyze(block, features);
    }
    {
        PerfScope2 perf_builder{GetPerfStats2().plan_builder};
        gpr_builder.Prepare(block);
    }
    building_gpr_plan->pseudo_flags = gpr_builder.ReleasePseudoFlags();
    PerfScope2 perf_declare_pinned{GetPerfStats2().declare_pinned};
    const auto& skip = GetSvmConfig().skip_prep;
    if (skip.find("loadupd") == std::string::npos) PreparePinnedLoadUpdates(block);
    if (skip.find("memvals") == std::string::npos) PreparePinnedMemoryValues(block);
    if (skip.find("spillpub") == std::string::npos) gpr_builder.PrepareSpilledGPRPublications(block);
    gpr_builder.PrepareNarrowReads([&](ir::Inst* consumer) { return building_gpr_plan->pseudo_flags.For(consumer).set; });
    PreparePinnedMemoryAddresses(block);
    perf_declare_pinned.Stop();
    PerfScope2 perf_declare_tail{GetPerfStats2().declare_tail};
    {
        PerfScope2 perf_zext{GetPerfStats2().tail_zext};
        PreparePinnedZeroExtendPublications(block);
    }
    PrepareMemoryUpdates(block);
    PrepareGPRPublicationPlan(block);
    for (const auto& [publication, action] : gpr_plan->publications)
        if (action.kind == GPRPublicationKind::Skip) allocation.DeclareElidedPublication(publication->Id());
    auto analysis = gpr_builder.ReleaseUseFacts();
    if (analysis->HasVectorConstants()) SelectPshufdDirect(*analysis, allocation);
    PerfScope2 perf_finish{GetPerfStats2().tail_finish};
    auto draft = gpr_builder.FinishConstraints();
    perf_finish.Stop();
    perf_declare_tail.Stop();
    return {block, building_gpr_plan, std::move(draft), std::move(analysis)};
}

void ARMTargetPlanner::BindDeclaredGPRHomes(std::span<const PendingGPRBlock> blocks, RegAlloc& allocation,
                                            const FeatureSet& features) {
    PerfScope2 perf_bind{GetPerfStats2().plan_bind};
    const auto declarations = allocation.SaveTargetDeclarations();
    // Instruction IDs already index RegAlloc. Keep one dense fact table rather
    // than rebuilding six hash tables from its declarations and use map.
    struct BindingFacts {
        ir::Inst* definition{};
        u32 last_use{}, residence_end{};
        u32 publication{UINT32_MAX};
        u16 selected{UINT16_MAX}, requested_low{UINT16_MAX};
        bool writer{};
    };
    std::vector<BindingFacts> facts(allocation.MapCount());
    for (const auto& home : declarations.homes) {
        auto& fact = facts[home.value];
        fact.selected = home.result != UINT16_MAX ? home.result
                : home.whole != UINT16_MAX && home.begin == home.value ? home.whole : home.low;
        fact.requested_low = home.low;
    }
    for (const auto& pending : blocks) {
        for (auto& inst : pending.block->GetInstList()) {
            facts[inst.Id()].definition = &inst;
            for (auto value : inst.GetValues()) {
                if (!value.Defined()) continue;
                auto& fact = facts[value.Id()];
                fact.last_use = std::max<u32>(fact.last_use, inst.Id());
                if (fact.requested_low == UINT16_MAX) continue;
                auto home = fact.requested_low;
                const auto use = allocation.GPRUseHome(value.Id(), inst.Id(), true);
                if (use && use->width == ir::GetValueSizeByte(value.Type())) home = use->home;
                if (home == fact.selected) fact.residence_end = std::max<u32>(fact.residence_end, inst.Id());
            }
        }
        const auto& plan = pending.actions;
        for (const auto& write : pending.draft.writes) {
            auto& fact = facts[write.instruction];
            if (fact.selected == write.home) fact.writer = true;
        }
        for (const auto& [inst, action] : plan->publications) {
            if (action.kind != GPRPublicationKind::Skip) continue;
            auto& fact = facts[inst->GetArg<ir::Value>(0).Id()];
            if (fact.selected == inst->GetArg<ir::Imm>(1).Get() && fact.publication == UINT32_MAX)
                fact.publication = inst->Id();
        }
    }
    const bool census = BindCensusEnabled();
    if (census) ++g_bind_census.calls;
    // Census-only: which class added/changed each fixed definition in
    // this call (1=transfer 2=narrow 3=writer 4=operand), to attribute
    // the definitions a failed check blames.
    std::unordered_map<u32, int> added_class;
    bool changed = false;
    for (const auto& home : declarations.homes) {
        const auto& fact = facts[home.value];
        auto* definition = fact.definition;
        if (!definition || !definition->HasValue() || ir::IsFloatValueType(definition->ReturnType())) continue;
        if (home.whole != UINT16_MAX && home.begin > home.value) {
            if (definition->GetOp() == ir::OpCode::GetHostGPR && definition->ReturnType() == ir::ValueType::U64) {
                if (census) ++g_bind_census.transfer_candidates;
                const auto source_home = definition->GetArg<ir::Imm>(0).Get();
                const auto* old = allocation.FixedGPRDefinitionAt(home.value);
                if (old && !old->preserved && old->elided && old->home == source_home &&
                    old->transferred_home == home.whole && old->transfer == home.begin - 1 &&
                    old->end >= home.begin - 1 && old->last_use >= home.end) continue;
                allocation.PlanFixedGPRDefinition(home.value, {
                        .home = static_cast<u16>(definition->GetArg<ir::Imm>(0).Get()),
                        .transferred_home = home.whole, .end = home.begin - 1,
                        .transfer = home.begin - 1, .last_use = home.end,
                        .publication = home.begin - 1, .owner = home.value,
                        .width = 8, .elided = true, .version = true});
                if (census) { ++g_bind_census.transfer_changed; added_class[home.value] = 1; }
                changed = true;
            }
            continue;
        }
        if (fact.selected == UINT16_MAX) continue;
        // A low-bit or post-publication alias is a use residence, not a new
        // canonical SSA definition. Bind real writers and proven reads only.
        const bool narrow_read = definition->GetOp() == ir::OpCode::GetHostGPR &&
                ir::GetValueSizeByte(definition->ReturnType()) <= 4 && home.low != UINT16_MAX;
        if (!fact.writer && !narrow_read && definition->GetOp() != ir::OpCode::GetOperand) continue;
        if (census && narrow_read) {
            const auto* old = allocation.FixedGPRDefinitionAt(home.value);
            const u32 want_last = std::max(home.value, fact.residence_end);
            if (!old) ++g_bind_census.narrow_no_old;
            else if (old->preserved) ++g_bind_census.narrow_old_preserved;
            else if (old->home != fact.selected) ++g_bind_census.narrow_old_home;
            else if (old->end < want_last) ++g_bind_census.narrow_old_end;
            else if (old->elided != !fact.writer) ++g_bind_census.narrow_old_elided;
            else ++g_bind_census.narrow_old_ok;
        }
        const bool bind_changed = allocation.BindTargetDefinition(home.value, fact.selected,
                static_cast<u8>(ir::GetValueSizeByte(definition->ReturnType())), !fact.writer,
                std::max(home.value, narrow_read ? fact.residence_end : fact.last_use), fact.publication);
        if (census) {
            if (narrow_read) {
                ++g_bind_census.narrow_candidates;
                if (bind_changed) { ++g_bind_census.narrow_changed; added_class[home.value] = 2; }
            } else if (fact.writer) {
                ++g_bind_census.writer_candidates;
                if (bind_changed) { ++g_bind_census.writer_changed; added_class[home.value] = 3; }
            } else {
                ++g_bind_census.operand_candidates;
                if (bind_changed) { ++g_bind_census.operand_changed; added_class[home.value] = 4; }
            }
        }
        changed |= bind_changed;
    }
    if (census && changed) ++g_bind_census.changed_calls;
    if (changed) {
        // Replay exists to make the allocation honour the fixed
        // definitions just recorded. When the current allocation
        // already satisfies every one of them (the common case:
        // Declare proposed homes it observed in this allocation),
        // replaying can only reproduce an equivalent allocation,
        // so verify instead and keep this one.
        bool satisfied = true;
        for (const auto& pending : blocks) {
            u32 fail_kind = 0;
            if (!CheckFixedGPRConstraints(pending.block, allocation, features, pending.draft.writes, &fail_kind)) {
                if (GetSvmConfig().density_prof) {
                    std::fprintf(stderr, "[bind-check] fail kind=%u\n", fail_kind);
                }
                satisfied = false;
                break;
            }
        }
        if (census && satisfied) ++g_bind_census.replay_skipped;
        if (census && !satisfied) {
            ++g_bind_census.fail_calls;
            bool found = false;
            for (const auto& pending : blocks) {
                for (auto& inst : pending.block->GetInstList()) {
                    const auto* fixed = allocation.FixedGPRDefinitionAt(inst.Id());
                    if (!fixed || fixed->preserved) continue;
                    const bool placed = allocation.ValueType(ir::Value{&inst}) == RegAlloc::GPR &&
                            allocation.ValueGPR(ir::Value{&inst}).id == fixed->home;
                    if (placed) continue;
                    found = true;
                    const auto it = added_class.find(inst.Id());
                    switch (it == added_class.end() ? 0 : it->second) {
                        case 1: ++g_bind_census.fail_transfer; break;
                        case 2: ++g_bind_census.fail_narrow; break;
                        case 3: ++g_bind_census.fail_writer; break;
                        case 4: ++g_bind_census.fail_operand; break;
                        default: ++g_bind_census.fail_prepare; break;
                    }
                }
            }
            if (!found) ++g_bind_census.fail_none_found;
        }
        if (!satisfied) allocation.ReplayAllocation();
        allocation.RestoreTargetDeclarations(declarations);
    }
}

void ARMTargetPlanner::Complete(ir::Block* block, ir::HIRFunction* function, bool can_replay) {
    auto rollback = ScopeExit([&] { allocation.AbortTargetPlanning(); });
    allocation.BeginTargetPlanning();
    std::vector<ir::Block*> blocks;
    if (function) {
        guest_state_map.AnalyzeFunction(function, features);
        for (auto& item : function->GetHIRBlocksRPO()) blocks.push_back(item.GetBlock());
    } else blocks.push_back(block);
    std::vector<PendingGPRBlock> pending;
    for (auto* current : blocks) pending.push_back(DeclareGPRBlock(current));
    if (can_replay && GetSvmConfig().skip_prep.find("targetbind") == std::string::npos)
        BindDeclaredGPRHomes(pending, allocation, features);
    const auto prepare_targets = [&] {
        PerfScope2 perf_targets{GetPerfStats2().plan_targets};
        u8 rejected = 0;
        for (auto& item : pending) {
            auto fpr = item.analysis->HasFPRInstructions()
                    ? BuildFPRAllocationPlan(item.block, allocation, features,
                                              config.sse_scalar_insert, features.sse_scalar_tie, item.analysis.get())
                    : FPRAllocationPlan{};
            if (!fpr.valid) rejected |= 1;
            if (fpr.valid && !fpr.selected) {
                item.actions->fpr.reset();
            } else item.actions->fpr = std::make_shared<const FPRAllocationPlan>(std::move(fpr));
        }
        return rejected;
    };
    for (unsigned retry = 0;; ++retry) {
        const auto rejected = prepare_targets();
        if (!rejected) break;
        ASSERT_MSG(can_replay && retry < 2, "target contract rejected without a reallocation path");
        if (rejected & 1) {
            ASSERT(!allocation.FPRCoalescingDisabled());
            allocation.DisableFPRCoalescing();
        }
        allocation.ReplayAllocation();
        pending.clear();
        for (auto* current : blocks) pending.push_back(DeclareGPRBlock(current));
        if (GetSvmConfig().skip_prep.find("targetbind") == std::string::npos)
            BindDeclaredGPRHomes(pending, allocation, features);
    }
    std::vector<GPRPlanVerificationInput> verification;
    std::vector<u32> zero_stores;
    const auto unit_scratch = PrepareUnitScratch(allocation.GetGprs(), features);
    BitVector flag_coverage(allocation.MapCount());
    PerfScope2 perf_perblock{GetPerfStats2().plan_perblock};
    for (auto& item : pending) {
        if (FlagsRegsEnabled()) {
            auto tokens = BuildFlagsTokenPlan(*item.analysis, allocation, features, &item.actions->publications, &item.actions->pseudo_flags);
            if (!tokens.homes.empty() || !tokens.point_homes.empty())
                item.actions->flags_tokens = std::make_shared<const FlagsTokenPlan>(std::move(tokens));
        }
        std::optional<ConsumerPlan> consumers;
        if (item.analysis->HasBooleanSelects() &&
            GetSvmConfig().skip_prep.find("boolsel") == std::string::npos) {
            auto selects = BuildBooleanSelectPlan(item.block, *item.analysis, [&](const ir::Inst& inst) {
                // Pure IR needs only the target encoding contract. Reuse the
                // helper/effect contracts for operations with observable state.
                return inst.GetEffects() == ir::IREffect::None
                        ? TargetPreservesHostNZCV(inst.GetOp())
                        : TargetRetainsPendingHostNZCV(inst, features) && !TargetMayFaultOrObserve(inst, guest_state_map);
            });
            if (!selects.actions.empty())
                item.actions->boolean_selects = std::make_shared<const BooleanSelectPlan>(std::move(selects));
        }
        const bool alu_split_audit = GetSvmConfig().density_prof && GetSvmConfig().ra_hot_coalesce_all;
        if (item.analysis->HasConsumerDecisions() || alu_split_audit) {
            const auto terminal = item.block->GetTerminal();
            consumers.emplace(BuildConsumerPlan(*item.analysis,
                    [&](const ir::Inst& inst) { return TargetRetainsPendingHostNZCV(inst, features); },
                    [&](const ir::Inst& inst) { return TargetMayFaultOrObserve(inst, guest_state_map); },
                    &terminal, alu_split_audit));
            ASSERT_MSG(consumers->valid, "invalid pair-result relationship before emission");
        }
        auto scalar = item.analysis->HasFPRInstructions()
                ? BuildScalarFPRPublicationPlan(item.block, allocation, item.analysis.get())
                : ScalarFPRPublicationPlan{};
        if (scalar.loads.empty() && scalar.values.empty()) {
            item.actions->scalar_fpr.reset();
        } else item.actions->scalar_fpr = std::make_shared<const ScalarFPRPublicationPlan>(std::move(scalar));
        auto memory = item.analysis->HasMemoryInstructions()
                ? BuildMemoryLoweringPlan(item.block, allocation, features,
                *item.actions, item.draft.facts,
                {.narrow_fuse = features.mem_narrow_fuse,
                 .address_tie = features.addr_ea_tie,
                 .biased = use_memory_base,
                 .rcpc = True(config.arm64_features & Arm64Features::RCpc)}, item.analysis.get())
                : MemoryLoweringPlan{};
        if (!memory.covered.empty()) {
            for (const auto& [inst, writeback] : memory.writebacks)
                item.draft.writes.push_back({inst->Id(), writeback.base, writeback.consumed_value, writeback.update->Id()});
            item.actions->memory = std::make_shared<const MemoryLoweringPlan>(std::move(memory));
        }
        if (item.block->HasDeadEdgeIntegerBranchProof()) {
            auto branch = BuildIntegerBranchPlan(item.block, *item.analysis,
                    allocation, features, guest_state_map);
            if (branch) item.actions->integer_branch =
                    std::make_shared<const IntegerBranchPlan>(std::move(*branch));
        }
        const auto cover = [&](const auto& nodes) {
            for (auto* node : nodes) flag_coverage.set(node->Id());
        };
        if (item.actions->memory) cover(item.actions->memory->covered);
        if (item.actions->scalar_fpr) cover(item.actions->scalar_fpr->covered);
        if (item.actions->integer_branch) {
            cover(item.actions->integer_branch->edge.discarded);
            if (item.actions->integer_branch->narrow)
                flag_coverage.set(item.actions->integer_branch->narrow->immediate_load->Id());
        }
        auto integer_flags = GetSvmConfig().skip_prep.find("intflags") != std::string::npos
                ? IntegerFlagPreparationPlan{}
                : BuildIntegerFlagPreparation(*item.analysis, allocation,
                item.actions->pseudo_flags, item.actions->integer_branch.get(), flag_coverage,
                consumers && consumers->pstate ? &*consumers->pstate : nullptr,
                GetSvmConfig().flags_cfinv && True(config.arm64_features & Arm64Features::FlagM));
        if (!integer_flags.inputs.empty() || !integer_flags.integer.carries.empty() ||
            !integer_flags.integer.comparisons.empty())
            item.actions->integer_flags = std::make_shared<const IntegerFlagPreparationPlan>(std::move(integer_flags));
        ScalarPreparationPlan scalar_preparation;
        scalar_preparation.resident.Analyze(item.block);
        scalar_preparation.copies.Analyze(item.block);
        // A raw carry action suppresses CFINV and later consumes that exact
        // pending host-C representation. Other configurations use normal flags.
        if (consumers && FlagsRegsEnabled() && GetSvmConfig().flags_cfinv &&
            True(config.arm64_features & Arm64Features::FlagM))
            scalar_preparation.raw_carry.Analyze(item.block, features, *consumers, item.analysis.get());
        if (!scalar_preparation.Empty())
            item.actions->scalar_preparation = std::make_shared<const ScalarPreparationPlan>(std::move(scalar_preparation));
        // Match preparation-time coverage; later Backedge/PFAF stays excluded.
        if (item.actions->integer_flags) cover(item.actions->integer_flags->integer.covered);
        if (item.actions->boolean_selects) cover(item.actions->boolean_selects->covered);
        if (item.actions->scalar_preparation) cover(item.actions->scalar_preparation->resident.Discarded());
        NarrowBitInputs bit_inputs;
        bit_inputs.earlier_coverage = &flag_coverage;
        for (auto* inst : item.analysis->Instructions()) {
            const auto op = inst->GetOp();
            const bool copy_discarded = item.actions->scalar_preparation &&
                    item.actions->scalar_preparation->copies.InputDiscarded(inst);
            if (op == ir::OpCode::BitExtract &&
                (allocation.GPRLowView(inst->Id()) ||
                 (item.actions->integer_flags && item.actions->integer_flags->inputs.contains(inst)) ||
                 allocation.IsWidthChainCoalesced(inst->Id()) ||
                 allocation.IsLow32CopyCoalesced(inst->Id()) || copy_discarded))
                bit_inputs.blocked_extracts.insert(inst);
            if ((op == ir::OpCode::LsrImm || op == ir::OpCode::AsrImm) && copy_discarded)
                bit_inputs.discarded.insert(inst);
            const bool branch_producer = item.actions->integer_branch &&
                    item.actions->integer_branch->edge.producer == inst;
            if ((op == ir::OpCode::And || op == ir::OpCode::LsrImm || op == ir::OpCode::AsrImm) &&
                (branch_producer || !item.actions->pseudo_flags.For(inst).Null()))
                bit_inputs.flagged.insert(inst);
        }
        auto bits = BuildNarrowBitPlan(item.block, allocation, bit_inputs);
        if (!bits.extensions.empty() || !bits.masks.empty() || !bits.shift_masks.empty() ||
            !bits.tied_load_extensions.empty())
            item.actions->narrow_bits = std::make_shared<const NarrowBitPlan>(std::move(bits));
        if (features.zero_store_zr) {
            std::vector<const ir::Inst*> incoming;
            const auto local = VerifyZeroStoreValues(*item.analysis, allocation, &incoming);
            zero_stores.insert(zero_stores.end(), local.begin(), local.end());
            if (!incoming.empty()) {
                if (!consumers) consumers.emplace();
                consumers->incoming_zeros.insert(incoming.begin(), incoming.end());
            }
        }
        if (consumers && (!consumers->alu_split.empty() || !consumers->pair_results.empty() || !consumers->booleans.empty() || !consumers->string_flags.empty() || !consumers->comparisons.empty() ||
            consumers->pstate || !consumers->local_conditions.empty() || !consumers->compound_clears.empty() ||
            !consumers->incoming_zeros.empty()))
            item.actions->consumers = std::make_shared<const ConsumerPlan>(std::move(*consumers));
        {
            PerfScope2 perf_seal{GetPerfStats2().perblock_seal};
            item.actions->pinned = FrozenGPRPlan::SealForUnit(allocation, std::move(item.draft), features, unit_scratch, *item.analysis);
        }
        allocation.StoreGPRPlan(item.block, item.actions);
        verification.push_back({item.block, item.actions->pinned.get(), item.analysis.get(),
                                &item.actions->flags_only_spills});
    }
    GPRSMask uniforms{};
    for (u16 reg = 0; reg < 31; ++reg) if (uniform_info.uni_gprs.Get(reg)) uniforms.Mark(reg);
    perf_perblock.Stop();
    PerfScope2 perf_verify{GetPerfStats2().plan_verify};
    GPRWidthCertificates certificates;
    if (GetSvmConfig().verify_plan)
        VerifyGPRAllocationPlan(verification, allocation, features, function,
                               config.region_edges, &certificates, uniforms, true);
    else
        VerifyGPRWidthElisionsOnly(verification, allocation, features, &certificates, uniforms);
    for (auto id : certificates.width) allocation.CertifyWidthElision(id, false);
    for (auto id : certificates.low32) allocation.CertifyWidthElision(id, true);
    for (auto id : certificates.reads) allocation.CertifyHostRead(id);
    for (auto id : zero_stores) allocation.CertifyZeroStore(id);
    perf_verify.Stop();
    auto region = std::make_shared<const RegionPreparationPlan>(
            BuildRegionPreparationPlan(function ? nullptr : block, function, allocation, config, features));
    for (auto& item : pending) {
        item.actions->region = region;
        if (item.actions->integer_branch) continue;
        const auto& options = region->options;
        BackedgeFlagsInputs inputs{features};
        inputs.self_enabled = (options.backedge_latch || options.region_edges) &&
                (options.backedge_flags || options.flags_loop_lazy);
        inputs.region_enabled = options.flags_region_branch;
        inputs.canonical_carry = options.flags_cfinv && options.flag_m;
        if (!inputs.self_enabled && !inputs.region_enabled) continue;
        if (inputs.region_enabled && region->active) {
            const auto mark_target = [&](const ir::Terminal& edge) {
                VisitVariant<void>(edge, [&](const auto& target) {
                    using T = std::decay_t<decltype(target)>;
                    if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                                  std::is_same_v<T, ir::terminal::LinkBlockFast>) {
                        const auto found = region->blocks.find(target.next.Value());
                        if (region->ContainsInternalBlock(target.next.Value()) &&
                            found != region->blocks.end() && found->second.edge_flags.Accepts(
                                    EdgeFlagsState::Pending(kEdgeNZCVMask,
                                            EdgeCarryPolarity::Unknown, EdgeFlagsProducer::Restore)))
                            inputs.flags_dead_targets.insert(target.next.Value());
                    }
                });
            };
            VisitVariant<void>(item.block->GetTerminal(), [&](const auto& terminal) {
                using T = std::decay_t<decltype(terminal)>;
                if constexpr (std::is_same_v<T, ir::terminal::If>) {
                    mark_target(terminal.then_); mark_target(terminal.else_);
                }
            });
        }
        if (auto plan = BuildBackedgeFlagsPlan(*item.block, inputs))
            item.actions->backedge_flags = std::make_shared<const BackedgeFlagsPlan>(std::move(*plan));
    }
    allocation.EndTargetPlanning();
    rollback.Cancel();
}

bool ARMTargetPlanner::ReproveCoalescedHostWrite(ir::Inst* inst) const {
    if (!inst || inst->GetOp() != ir::OpCode::SetHostGPR ||
        inst->GetArg<ir::Imm>(2).Get() != 0) {
        return false;
    }
    const u32 target = inst->GetArg<ir::Imm>(1).Get();
    auto stored = ResolveHostCoalesceBitCast(inst->GetArg<ir::Value>(0));
    auto* wrapper = stored.Def();
    const bool width_wrapper = stored.Defined() &&
                               allocation.IsWidthChainCoalesced(stored.Id());
    const bool live_publish =
            features.ra_coalesce_live && !width_wrapper;
    if (!wrapper || (!width_wrapper && !live_publish && wrapper->GetUses() != 1)) {
        return false;
    }
    auto produced = stored;
    bool zero_extend_chain = false;
    bool low32_copy = false;
    if (wrapper->GetOp() == ir::OpCode::ZeroExtend32To64) {
        produced = ResolveHostCoalesceBitCast(wrapper->GetArg<ir::Value>(0));
        zero_extend_chain = true;
        if (produced.Def() &&
            allocation.IsLow32CopyCoalesced(produced.Id()) &&
            produced.Def()->GetOp() == ir::OpCode::BitExtract &&
            produced.Def()->GetArg<ir::Imm>(1).Get() == 0 &&
            produced.Def()->GetArg<ir::Imm>(2).Get() == 32) {
            auto source = ResolveHostCoalesceBitCast(
                    produced.Def()->GetArg<ir::Value>(0));
            low32_copy = source.Defined() &&
                    allocation.Low32CopySource(produced.Id()) == source.Id() &&
                    SharesAllocatedGPR(allocation, source, produced);
        }
        const bool width_root = produced.Defined() &&
                                allocation.IsWidthChainCoalesced(produced.Id()) &&
                                allocation.WidthChainAnchor(produced.Id()) == produced.Id();
        if (!produced.Def() ||
            (!width_root && !live_publish && produced.Def()->GetUses() != 1) ||
            (!low32_copy &&
             !IsWidthChainHostWWrite(produced, target, features))) {
            return false;
        }
    }
    auto* producer = produced.Def();
    if (!producer ||
        !IsHostCoalesceProducer(producer->GetOp(),
                                features.ra_width_chain) ||
        (!low32_copy && !IsGPRMappedTo(produced, target)) ||
        (zero_extend_chain && !IsGPRMappedTo(stored, target))) {
        return false;
    }
    const u32 width = ir::GetValueSizeByte(produced.Type());
    if ((width != sizeof(u32) && width != sizeof(u64)) ||
        (width == sizeof(u32) &&
         !low32_copy &&
         !IsWidthChainHostWWrite(produced, target, features))) {
        return false;
    }
    if (width_wrapper) {
        const u32 anchor = allocation.WidthChainAnchor(stored.Id());
        if (allocation.HasWidthComponentOwner(anchor)) {
            const bool actual_high_zero = zero_extend_chain ||
                    ir::GetValueSizeByte(produced.Type()) == sizeof(u32);
            if (!allocation.WidthComponentOwnerCommitted(anchor) ||
                allocation.WidthComponentOwnerTarget(anchor) != target ||
                allocation.WidthComponentOwnerHighZero(anchor) != actual_high_zero) {
                return false;
            }
        }
    }
    ir::Inst* component_source = nullptr;
    if (producer->GetOp() == ir::OpCode::SignExtend) {
        auto source = ResolveHostCoalesceBitCast(producer->GetArg<ir::Value>(0));
        auto* source_def = source.Def();
        if (source_def && allocation.IsWidthChainCoalesced(source.Id())) {
            if ((source_def->GetOp() != ir::OpCode::LoadMemory &&
                 source_def->GetOp() != ir::OpCode::LoadMemoryTSO) ||
                ir::GetValueSizeByte(source.Type()) > sizeof(u16) ||
                source_def->GetUses() != 1 ||
                allocation.WidthChainAnchor(source.Id()) !=
                        allocation.WidthChainAnchor(produced.Id()) ||
                !SharesAllocatedGPR(allocation, source, produced)) {
                return false;
            }
            component_source = source_def;
        }
    }
    const u32 proof_start = component_source ? component_source->Id()
                                             : producer->Id();
    const auto& analysis = gpr_builder.UseFacts();
    auto last_use = [&](ir::Inst* definition) {
        u32 end = analysis.LastInstructionUse(definition);
        if (TerminalUsesHostCoalesceValue(cur_block->GetTerminal(), definition) &&
            !analysis.Instructions().empty()) {
            end = std::max<u32>(end, analysis.Instructions().back()->Id());
        }
        return end;
    };

    for (auto input : producer->GetValues()) {
        auto root = ResolveHostCoalesceBitCast(input);
        if (low32_copy && root.Defined() &&
            allocation.Low32CopySource(produced.Id()) == root.Id()) {
            continue;
        }
        if (!root.Defined() || !SharesAllocatedGPR(allocation, root, produced)) {
            continue;
        }
        if (last_use(root.Def()) > producer->Id()) {
            return false;
        }
    }

    u32 live_end = inst->Id();
    if (live_publish) {
        live_end = std::max<u32>(live_end, last_use(wrapper));
        if (zero_extend_chain) {
            live_end = std::max<u32>(live_end, last_use(producer));
        }
    }
    for (auto* node : analysis.Instructions()) {
        auto& other = *node;
        if (other.Id() > live_end) break;
        if (&other == component_source || &other == producer ||
            &other == wrapper || &other == inst ||
            !other.HasValue() || other.IsBitCastOperation()) {
            continue;
        }
        const bool low32_copy_alias =
                allocation.IsLow32CopyCoalesced(other.Id()) &&
                other.GetOp() == ir::OpCode::BitExtract &&
                other.GetArg<ir::Imm>(1).Get() == 0 &&
                other.GetArg<ir::Imm>(2).Get() == 32 &&
                (allocation.Low32CopySource(other.Id()) == stored.Id() ||
                 allocation.Low32CopySource(other.Id()) == produced.Id());
        if (low32_copy_alias) {
            continue;
        }
        if (other.Id() > inst->Id() &&
            other.GetOp() == ir::OpCode::GetHostGPR &&
            other.GetArg<ir::Imm>(0).Get() == target) {
            continue;
        }
        ir::Value value{&other};
        auto& allocation = this->allocation;
        if (allocation.FixedGPRPublicationOwns(inst->Id(), value.Id())) continue;
        const bool fixed = allocation.HasFixedGPRDefinition(value.Id());
        if (!fixed && !IsGPRMappedTo(value, target)) continue;
        // Fixed segments carry exact ends; ordinary ends come from shared uses.
        const u32 end = fixed ? value.Id() : last_use(&other);
        if (allocation.FixedGPRHomeIntersects(
                    value, target, proof_start, live_end, end)) {
            return false;
        }
    }

    // An incoming definition has no local start node, matching the original
    // local-window contract. Ordinary local proofs inspect only their window.
    const auto* start = component_source ? component_source : producer;
    const auto window = analysis.Contains(start) ? analysis.Between(proof_start, inst->Id())
                                                : std::span<ir::Inst* const>{};
    for (auto* node : window) {
        auto& scan = *node;
        if (&scan == producer || &scan == wrapper) {
            continue;
        }
        const bool flags_only = scan.GetOp() == ir::OpCode::SaveFlags;
        if ((!flags_only && IsHostCoalesceObserver(scan, features)) ||
            (scan.GetOp() == ir::OpCode::GetHostGPR &&
             scan.GetArg<ir::Imm>(0).Get() == target) ||
            (scan.GetOp() == ir::OpCode::SetHostGPR &&
             scan.GetArg<ir::Imm>(1).Get() == target)) {
            return false;
        }
    }
    if (!live_publish) {
        if (!width_wrapper && analysis.LastInstructionUse(wrapper) > inst->Id()) return false;
        if (TerminalUsesHostCoalesceValue(cur_block->GetTerminal(), wrapper)) {
            return false;
        }
        return true;
    }
    for (auto* node : analysis.Between(inst->Id(), live_end + 1)) {
        auto& scan = *node;
        if (scan.GetOp() == ir::OpCode::SetHostGPR &&
            scan.GetArg<ir::Imm>(1).Get() == target) {
            return false;
        }
        if (target <= 9 && HelperCallContract::InstructionClobbersGPR(
                    scan, target, features)) {
            return false;
        }
    }
    return true;
}

void ARMTargetPlanner::PrepareGPRPublicationPlan(ir::Block* block) {
    for (auto& publication : block->GetInstList()) {
        auto* inst = &publication;
        if (inst->GetOp() != ir::OpCode::SetHostGPR) continue;
        const auto target = inst->GetArg<ir::Imm>(1).Get();
        const auto byte_offset = inst->GetArg<ir::Imm>(2).Get();
        const auto width = ir::GetValueSizeByte(inst->GetArg<ir::Value>(0).Type());
        ASSERT_MSG(target < 31 && byte_offset < 8 && width <= 8 &&
                           byte_offset + width <= 8,
                   "invalid GPR publication at IR {}", inst->Id());
        GPRPublicationAction action{};
        const auto& recipes = gpr_builder.GetRecipes();
        if (const auto direct = recipes.select_publications.find(inst);
            direct != recipes.select_publications.end()) {
        } else if (const auto spill = recipes.spilled_publications.find(inst);
                   spill != recipes.spilled_publications.end()) {
        } else if (const auto update = gpr_plan->load_update_instructions.find(inst);
                   update != gpr_plan->load_update_instructions.end() &&
                   inst == update->second.publication) {
        } else if (recipes.dead_writes.contains(inst)) {
        } else if (const auto transfer = recipes.transfers.find(inst);
                   transfer != recipes.transfers.end()) {
            action.kind = GPRPublicationKind::Transfer;
            action.source_home = transfer->second.source;
            action.target = transfer->second.target;
        } else if (const auto copy = recipes.copies.find(inst);
                   copy != recipes.copies.end()) {
            if (copy->second.source) {
                action.kind = GPRPublicationKind::Copy;
                action.source_home = copy->second.source;
                action.target = copy->second.target;
                action.width = copy->second.width * 8;
            }
        } else {
            const auto value = inst->GetArg<ir::Value>(0);
            // PrepareMemoryUpdates just ran the identical matches for
            // every instruction in this block; reuse its results keyed
            // by the update instruction instead of re-matching (each
            // match re-walks operands and queries preservation
            // conflicts).
            const MemoryUpdate* biased{};
            const MemoryUpdate* preindex{};
            if (auto* update = value.Def()) {
                if (const auto found = gpr_plan->biased_updates.find(update);
                    found != gpr_plan->biased_updates.end()) biased = &found->second;
                if (const auto found = gpr_plan->preindex_updates.find(update);
                    found != gpr_plan->preindex_updates.end()) preindex = &found->second;
            }
            const bool coalesced = allocation.IsHostWriteCoalesced(inst->Id());
            if (biased && biased->publication == inst) {
                action.kind = GPRPublicationKind::BiasedUpdate;
                action.target = biased->base.GetCode();
                action.decrement = static_cast<u64>(-biased->offset);
            } else if (preindex && preindex->publication == inst) {
                // The memory instruction performs the publication.
            } else if (coalesced && ReproveCoalescedHostWrite(inst)) {
                // The producer already writes the planned home.
            } else {
                ASSERT_MSG(!coalesced || features.ra_coalesce_live,
                           "SetHostGPR coalescing proof diverged at IR {}", inst->Id());
                action.kind = GPRPublicationKind::Value;
                action.target = target;
                action.bit_offset = byte_offset * 8;
                action.width = width * 8;
                const bool fused = value.Def() && IsGPRZeroExtendElided(value.Def());
                const auto residence = FixedGPRHomeForUse(value, inst);
                action.source = fused ? value.Def()->GetArg<ir::Value>(0) : value;
                if (!fused && residence) action.source_home = residence->home;
                action.clear_high = IsFixedGPRHome(action.target) &&
                        (action.width == 32 || fused ||
                         (value.Def() && value.Def()->GetOp() == ir::OpCode::ZeroExtend32To64));
                action.adjust_resident_home = residence &&
                        !residence->extension.KnownZeroAbove(32);
                action.adopts_spill = !coalesced && !fused && !residence;
            }
        }
        MutableGPRPlan().publications.emplace(inst, action);
    }
}

std::optional<ARMTargetPlanner::MemoryUpdate>
ARMTargetPlanner::MatchMemoryUpdate(ir::Inst* update) const {
    if (!update || update->GetOp() != ir::OpCode::Sub ||
        ir::GetValueSizeByte(update->ReturnType()) != sizeof(u64) ||
        update->GetUses(false) != 2) {
        return std::nullopt;
    }

    const auto right = update->GetArg<ir::Operand>(1);
    if (!right.GetRight().Null() || !right.GetLeft().IsImm()) {
        return std::nullopt;
    }
    const u64 decrement = right.GetLeft().imm.Get();
    if (decrement == 0 || decrement > 256) {
        return std::nullopt;
    }

    const auto source = update->GetArg<ir::Value>(0);
    auto* source_def = source.Def();
    if (!source_def || source_def->GetOp() != ir::OpCode::GetHostGPR ||
        source_def->GetArg<ir::Imm>(1).Get() != 0 ||
        ir::GetValueSizeByte(source.Type()) != sizeof(u64)) {
        return std::nullopt;
    }
    const u32 target = source_def->GetArg<ir::Imm>(0).Get();
    const auto base = XRegister(target);
    if (!IsGPRMappedTo(source, target)) {
        return std::nullopt;
    }

    auto& instructions = cur_block->GetInstList();
    auto memory_it = std::next(instructions.iterator_to(*update));
    if (memory_it == instructions.end() ||
        memory_it->GetOp() != ir::OpCode::StoreMemory) {
        return std::nullopt;
    }
    const auto memory_operand = memory_it->GetArg<ir::Operand>(0);
    const auto stored = memory_it->GetArg<ir::Value>(1);
    if (!memory_operand.GetRight().Null() ||
        !memory_operand.GetLeft().IsValue() ||
        memory_operand.GetLeft().value.Def() != update ||
        ir::IsFloatValueType(stored.Type()) ||
        ir::GetValueSizeByte(stored.Type()) != decrement) {
        return std::nullopt;
    }
    if (IsGPRMappedTo(stored, target)) {
        return std::nullopt;
    }

    auto publication_it = std::next(memory_it);
    if (publication_it == instructions.end() ||
        publication_it->GetOp() != ir::OpCode::SetHostGPR ||
        publication_it->GetArg<ir::Value>(0).Def() != update ||
        publication_it->GetArg<ir::Imm>(1).Get() != target ||
        publication_it->GetArg<ir::Imm>(2).Get() != 0) {
        return std::nullopt;
    }

    if (allocation.GPRPreservationConflict(
                target, memory_it->Id(), publication_it->Id())) return std::nullopt;
    return MemoryUpdate{
            .memory = memory_it.operator->(),
            .publication = publication_it.operator->(),
            .base = base,
            .offset = -static_cast<s64>(decrement),
    };
}

void ARMTargetPlanner::PrepareMemoryUpdates(ir::Block* block) {
    MutableGPRPlan().preindex_updates.clear(); MutableGPRPlan().biased_updates.clear();
    for (auto& inst : block->GetInstList()) {
        if (const auto update = MatchPreIndexMemoryUpdate(&inst)) {
            MutableGPRPlan().preindex_updates.emplace(&inst, *update);
            gpr_builder.RecordPhysicalWrite(update->memory, update->base.GetCode());
        }
        if (const auto update = MatchBiasedMemoryUpdate(&inst)) MutableGPRPlan().biased_updates.emplace(&inst, *update);
    }
}

std::optional<ARMTargetPlanner::PinnedLoadUpdate>
ARMTargetPlanner::MatchPinnedLoadUpdate(ir::Inst* update) {
    if (use_memory_base || !update || update->GetOp() != ir::OpCode::Add ||
        update->ReturnType() != ir::ValueType::U64 ||
        !GetPseudoFlags(update).Null()) {
        return std::nullopt;
    }
    auto base = ResolveBitCast(update->GetArg<ir::Value>(0));
    auto* base_read = base.Def();
    const auto offset = ImmediateValue(update->GetArg<ir::Operand>(1));
    if (!base_read || base_read->GetOp() != ir::OpCode::GetHostGPR ||
        base_read->GetArg<ir::Imm>(1).Get() != 0 || !offset ||
        *offset != 1) {
        return std::nullopt;
    }
    const u32 target = base_read->GetArg<ir::Imm>(0).Get();
    if (!IsFixedGPRHome(target) || base_read->GetUses(false) != 2) {
        return std::nullopt;
    }

    ir::Inst* load{};
    const auto& analysis = gpr_builder.UseFacts();
    for (const auto& use : analysis.ResolvedUses(base_read)) {
        auto& scan = *use.consumer;
        if (scan.Id() >= update->Id()) {
            break;
        }
        if (scan.GetOp() != ir::OpCode::LoadMemory) {
            continue;
        }
        const auto address = scan.GetArg<ir::Operand>(0);
        if (address.GetOp() != ir::OperandOp::Plus ||
            !address.GetLeft().IsValue() ||
            ResolveBitCast(address.GetLeft().value).Def() != base_read) {
            continue;
        }
        const auto memory_offset = ImmediateValue(address);
        if (memory_offset && *memory_offset == *offset) {
            load = &scan;
        }
    }
    ir::Inst* publication{};
    for (const auto& use : analysis.Uses(update)) {
        auto& scan = *use.consumer;
        if (scan.Id() <= update->Id()) {
            continue;
        }
        if (use.count == 1 && scan.GetOp() == ir::OpCode::SetHostGPR &&
            scan.GetArg<ir::Value>(0).Def() == update &&
            scan.GetArg<ir::Imm>(1).Get() == target &&
            scan.GetArg<ir::Imm>(2).Get() == 0) {
            publication = &scan;
        }
        break;
    }
    if (!load || !publication || load->Id() >= update->Id() ||
        IsGPRMappedTo(ir::Value{load}, target)) {
        return std::nullopt;
    }
    u32 ordinary_uses{};
    for (const auto& use : analysis.Uses(update)) {
        auto& scan = *use.consumer;
        if (scan.GetOp() == ir::OpCode::SaveFlags ||
            scan.GetOp() == ir::OpCode::BranchOnlyFlags ||
            scan.GetOp() == ir::OpCode::ClearFlags) {
            continue;
        }
        ordinary_uses += use.count;
    }
    if (ordinary_uses != 1) {
        return std::nullopt;
    }
    for (auto* node : analysis.Between(load->Id(), update->Id())) {
        auto& scan = *node;
        if (MayFaultOrObserve(scan) ||
            (scan.GetOp() == ir::OpCode::GetHostGPR &&
             scan.GetArg<ir::Imm>(0).Get() == target) ||
            (scan.GetOp() == ir::OpCode::SetHostGPR &&
             scan.GetArg<ir::Imm>(1).Get() == target)) {
            return std::nullopt;
        }
    }
    return PinnedLoadUpdate{
            .load = load,
            .update = update,
            .publication = publication,
            .base_read = base_read,
            .target = static_cast<u16>(target),
            .offset = static_cast<s16>(*offset),
    };
}

void ARMTargetPlanner::PreparePinnedLoadUpdates(ir::Block* block) {
    for (auto& inst : block->GetInstList()) {
        auto candidate = MatchPinnedLoadUpdate(&inst);
        if (!candidate || allocation.GPRTargetWriteConflict(
                    candidate->target, candidate->load->Id(), candidate->publication->Id(),
                    candidate->base_read->Id(), candidate->update->Id())) {
            continue;
        }
        gpr_builder.AssignLowView(candidate->base_read, candidate->target);
        if (allocation.TargetPlanning()) {
            if (const auto* current = allocation.FixedGPRDefinitionAt(candidate->base_read->Id());
                current && current->elided && !current->preserved) {
                auto residence = *current;
                residence.end = candidate->load->Id();
                allocation.PlanFixedGPRDefinition(candidate->base_read->Id(), residence);
            }
        }
        gpr_builder.RecordPhysicalWrite(candidate->load, candidate->target);
        MutableGPRPlan().load_updates.emplace(candidate->load, *candidate);
        MutableGPRPlan().load_update_instructions.emplace(candidate->update, *candidate);
        MutableGPRPlan().load_update_instructions.emplace(candidate->publication, *candidate);
    }
}

std::optional<ARMTargetPlanner::PinnedMemorySource>
ARMTargetPlanner::MatchPinnedMemorySource(ir::Value source) const {
    auto* read = source.Def();
    if (!read || ir::GetValueSizeByte(source.Type()) != sizeof(u64)) {
        return std::nullopt;
    }

    u32 target{};
    u32 live_begin{};
    if (read->GetOp() == ir::OpCode::GetHostGPR) {
        if (read->GetArg<ir::Imm>(1).Get() != 0) {
            return std::nullopt;
        }
        target = read->GetArg<ir::Imm>(0).Get();
        live_begin = read->Id();
    } else if (read->GetOp() == ir::OpCode::BitCast) {
        const auto published = read->GetArg<ir::Value>(0);
        auto* producer = published.Def();
        if (!producer || (producer->GetOp() != ir::OpCode::LoadMemory &&
                           producer->GetOp() != ir::OpCode::LoadMemoryTSO) ||
            ir::GetValueSizeByte(published.Type()) != sizeof(u64) ||
            read->GetUses(false) != 1) {
            return std::nullopt;
        }
        ir::Inst* publication = nullptr;
        u32 producer_uses = 0;
        for (const auto& use : gpr_builder.UseFacts().Uses(producer)) {
            auto& scan = *use.consumer;
            producer_uses += use.count;
            if (&scan == read) continue;
            if (use.count != 1 || publication || scan.GetOp() != ir::OpCode::SetHostGPR ||
                scan.GetArg<ir::Value>(0).Def() != producer ||
                scan.GetArg<ir::Imm>(2).Get() != 0 ||
                !allocation.IsHostWriteCoalesced(scan.Id())) {
                return std::nullopt;
            }
            publication = &scan;
        }
        if (!publication || producer_uses != producer->GetUses() ||
            publication->Id() >= read->Id()) {
            return std::nullopt;
        }
        target = publication->GetArg<ir::Imm>(1).Get();
        live_begin = publication->Id();
        if (!IsGPRMappedTo(published, target)) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    if (!IsPinnedMemoryTarget(target) || !IsGPRMappedTo(source, target)) {
        return std::nullopt;
    }
    return PinnedMemorySource{static_cast<u16>(target), live_begin};
}

std::optional<ARMTargetPlanner::PinnedMemoryAddress>
ARMTargetPlanner::MatchPinnedMemoryAddress(ir::Inst* address) const {
    if (!address || address->GetOp() != ir::OpCode::GetOperand ||
        address->GetUses(false) != 1) {
        return std::nullopt;
    }
    const auto operand = address->GetArg<ir::Operand>(0);
    if (!operand.GetLeft().IsValue()) {
        return std::nullopt;
    }
    s64 offset{};
    if (!operand.GetRight().Null()) {
        if (operand.GetOp() != ir::OperandOp::Plus ||
            !operand.GetRight().IsImm()) {
            return std::nullopt;
        }
        offset = operand.GetRight().imm.GetSigned();
        if (offset == std::numeric_limits<s64>::min()) {
            return std::nullopt;
        }
    }
    const auto source = MatchPinnedMemorySource(operand.GetLeft().value);
    if (!source) {
        return std::nullopt;
    }

    ir::Inst* memory = nullptr;
    const auto& analysis = gpr_builder.UseFacts();
    for (const auto& use : analysis.Uses(address)) {
        auto& scan = *use.consumer;
        if (scan.Id() <= address->Id() ||
            (scan.GetOp() != ir::OpCode::LoadMemory &&
             scan.GetOp() != ir::OpCode::StoreMemory)) {
            continue;
        }
        const auto memory_operand = scan.GetArg<ir::Operand>(0);
        if (memory_operand.GetRight().Null() &&
            memory_operand.GetLeft().IsValue() &&
            memory_operand.GetLeft().value.Def() == address) {
            memory = &scan;
            break;
        }
    }
    if (!memory) {
        return std::nullopt;
    }

    if (offset != 0 && !use_memory_base) {
        const auto type = memory->GetOp() == ir::OpCode::LoadMemory
                ? memory->ReturnType()
                : memory->GetArg<ir::Value>(1).Type();
        const u32 access_size = ir::GetValueSizeByte(type);
        if (!vixl::aarch64::Assembler::IsImmLSUnscaled(offset) &&
            !vixl::aarch64::Assembler::IsImmLSScaled(offset, access_size)) {
            return std::nullopt;
        }
    }

    for (auto* node : analysis.Between(source->live_begin, memory->Id())) {
        auto& scan = *node;
        if ((scan.GetOp() == ir::OpCode::SetHostGPR &&
             scan.GetArg<ir::Imm>(1).Get() == source->target) ||
            (source->target <= 9 && IsCallerSavedAddressBarrier(scan.GetOp()))) {
            return std::nullopt;
        }
    }
    return PinnedMemoryAddress{memory, source->target, offset};
}

std::optional<u16> ARMTargetPlanner::MatchPinnedMemoryValue(ir::Inst* extract) const {
    if (!extract || extract->GetOp() != ir::OpCode::BitExtract ||
        extract->GetUses(false) != 1) {
        return std::nullopt;
    }
    const auto source = extract->GetArg<ir::Value>(0);
    const u32 width = ir::GetValueSizeByte(extract->ReturnType());
    if (!source.Def() || (width != sizeof(u8) && width != sizeof(u16)) ||
        extract->GetArg<ir::Imm>(1).Get() != 0 ||
        extract->GetArg<ir::Imm>(2).Get() != width * 8 ||
        ir::GetValueSizeByte(source.Type()) < width || IsSpilled(source)) {
        return std::nullopt;
    }

    ir::Inst* store = nullptr;
    ir::Inst* publication = nullptr;
    const auto& analysis = gpr_builder.UseFacts();
    for (const auto& use : analysis.Uses(source.Def())) {
        auto& scan = *use.consumer;
        if (scan.GetOp() == ir::OpCode::SetHostGPR &&
            scan.GetArg<ir::Value>(0).Def() == source.Def()) {
            if (publication) {
                return std::nullopt;
            }
            publication = &scan;
        }
    }
    for (const auto& use : analysis.Uses(extract)) {
        auto& scan = *use.consumer;
        if (store || scan.GetOp() != ir::OpCode::StoreMemory ||
            scan.GetArg<ir::Value>(1).Def() != extract ||
            ir::GetValueSizeByte(scan.GetArg<ir::Value>(1).Type()) != width) {
            return std::nullopt;
        }
        store = &scan;
    }
    if (!store || !publication || publication->Id() >= extract->Id() ||
        extract->Id() >= store->Id() ||
        publication->GetArg<ir::Imm>(2).Get() != 0 ||
        !allocation.IsHostWriteCoalesced(publication->Id()) ||
        !ReproveCoalescedHostWrite(publication)) {
        return std::nullopt;
    }

    const u32 target = publication->GetArg<ir::Imm>(1).Get();
    if (!IsPinnedMemoryTarget(target) || !IsGPRMappedTo(source, target)) {
        return std::nullopt;
    }
    for (auto* node : analysis.Between(publication->Id(), store->Id())) {
        auto& scan = *node;
        if ((scan.GetOp() == ir::OpCode::SetHostGPR &&
             scan.GetArg<ir::Imm>(1).Get() == target) ||
            (target <= 9 && IsCallerSavedAddressBarrier(scan.GetOp()))) {
            return std::nullopt;
        }
    }
    return static_cast<u16>(target);
}

void ARMTargetPlanner::PreparePinnedMemoryValues(ir::Block* block) {
    MutableGPRPlan().memory_values.clear();
    for (auto& inst : block->GetInstList()) {
        if (auto target = MatchPinnedMemoryValue(&inst)) {
            MutableGPRPlan().memory_values.emplace(&inst, *target);
            gpr_builder.AssignLowView(&inst, *target);
        }
    }
}

void ARMTargetPlanner::PreparePinnedMemoryAddresses(ir::Block* block) {
    MutableGPRPlan().addresses.clear();
    for (auto& inst : block->GetInstList()) {
        if (const auto address = MatchPinnedMemoryAddress(&inst)) {
            MutableGPRPlan().addresses.emplace(&inst, *address);
            if (address->offset == 0)
                gpr_builder.AssignValueHome(&inst, address->target);
        }
    }
}

void ARMTargetPlanner::PreparePinnedZeroExtendPublications(ir::Block* block) {
    if (GetSvmConfig().zext_no_elide) return;
    for (auto& candidate : block->GetInstList()) {
        auto* inst = &candidate;
        if (inst->GetOp() != ir::OpCode::ZeroExtend32To64 ||
            IsGPRZeroExtendElided(inst)) continue;
        const auto source = inst->GetArg<ir::Value>(0);
        // Keep the existing lowering priority: a tied W write still clears
        // the upper half; the publication-only case comes after those ties.
        if (CanUseZeroStoreRegister(ir::Value{inst}) ||
            allocation.IsWidthChainCoalesced(inst->Id()) ||
            (source.Def() && allocation.IsLow32CopyCoalesced(source.Id())) ||
            (ir::GetValueSizeByte(source.Type()) == sizeof(u32) &&
             SharesAllocatedGPR(allocation, source, ir::Value{inst})) ||
            inst->GetUses() != 1) continue;
        auto& list = block->GetInstList();
        for (auto it = std::next(list.iterator_to(*inst)); it != list.end(); ++it) {
            bool names_value = false;
            for (auto used : it->GetValues()) names_value |= used.Def() == inst;
            if (!names_value) continue;
            if (it->GetOp() == ir::OpCode::SetHostGPR &&
                it->GetArg<ir::Value>(0).Def() == inst &&
                it->GetArg<ir::Imm>(2).Get() == 0) {
                const auto target = it->GetArg<ir::Imm>(1).Get();
                bool source_survives = true;
                const auto& allocation = this->allocation;
                // Elision moves the parent's machine use from the extension
                // to this publication. RA still expires it at the original
                // use, so an intervening definition or pre-publication capture
                // can reuse its register (or spill slot).
                const auto kind = allocation.ValueType(source);
                const auto slot = kind == RegAlloc::GPR ? allocation.ValueGPR(source).id
                        : kind == RegAlloc::MEM ? allocation.ValueMem(source).offset : UINT16_MAX;
                for (auto scan = std::next(list.iterator_to(*inst)); scan != std::next(it); ++scan) {
                    if (kind == RegAlloc::GPR) {
                        if (backend::FixedGPRClobbers(*scan, features) & (1u << slot)) source_survives = false;
                        if (scan.operator->() != it.operator->() && scan->GetOp() == ir::OpCode::SetHostGPR &&
                            scan->GetArg<ir::Imm>(1).Get() == slot) source_survives = false;
                        if (const auto helper = HelperCallContract::Resolve(*scan, features);
                            helper && helper->ClobbersGPR(slot)) source_survives = false;
                    }
                    for (const auto& capture : allocation.GPRPreservationsBefore(scan->Id()))
                        if (capture.destination_kind == kind && capture.destination == slot) source_survives = false;
                    if (scan->HasValue()) {
                        const ir::Value result{scan.operator->()};
                        if (allocation.ValueType(result) == kind &&
                            ((kind == RegAlloc::GPR && allocation.ValueGPR(result).id == slot) ||
                             (kind == RegAlloc::MEM && allocation.ValueMem(result).offset == slot))) source_survives = false;
                    }
                }
                if (source_survives && (target <= 9 || target == 22 || target == 23 || target == 29))
                    gpr_builder.ElideZeroExtend(inst);
            }
            break;
        }
    }
}

std::optional<ARMTargetPlanner::MemoryUpdate>
ARMTargetPlanner::MatchPreIndexMemoryUpdate(ir::Inst* update) const {
    return use_memory_base ? std::nullopt : MatchMemoryUpdate(update);
}
std::optional<ARMTargetPlanner::MemoryUpdate>
ARMTargetPlanner::MatchBiasedMemoryUpdate(ir::Inst* update) const {
    if (!use_memory_base) return std::nullopt;
    auto match = MatchMemoryUpdate(update);
    return match && IsSpilled(ir::Value{update}) ? match : std::nullopt;
}

void CompleteARM64TargetPlanning(const std::shared_ptr<Module>& module, RegAlloc& allocation,
                                ir::Block* block, ir::HIRFunction* function, bool can_replay) {
    ARMTargetPlanner planner{allocation, module->GetAddressSpace().GetConfig(),
                             ResolveFeatureSet(module->GetModuleConfig()),
                             module->GetAddressSpace().GetUniformInfo()};
    planner.Complete(block, function, can_replay);
}

bool ARM64UnitReadyForEmission(const std::shared_ptr<Module>& module,
        const RegAlloc& allocation, ir::Block* block, ir::HIRFunction* function) {
    if (!module || (!!block == !!function) || !allocation.GPRAllocationComplete() ||
        !allocation.GPRLocationsFrozen()) return false;
    const ir::Block* first = block;
    if (function) {
        if (function->GetHIRBlocksRPO().empty()) return false;
        first = function->GetHIRBlocksRPO().front().GetBlock();
    }
    const auto* actions = allocation.GPRPlanViewFor(first);
    if (!actions || !actions->region) return false;
    const auto& region = *actions->region;
    return region.valid && region.standalone == block && region.function == function &&
        region.generation == allocation.GPRLocationGeneration() &&
        region.allocation_attempt == allocation.AllocationAttempt() &&
        region.MatchesTargetInputs(allocation, module->GetAddressSpace().GetConfig(),
                ResolveFeatureSet(module->GetModuleConfig()));
}

bool CompleteARM64UnitForEmission(const std::shared_ptr<Module>& module,
        RegAlloc& allocation, ir::Block* block, ir::HIRFunction* function) {
    if (!module || (!!block == !!function)) return false;
    allocation.CompleteGPRAllocation(module);
    if (GetSvmConfig().density_prof) {
        std::fprintf(stderr, "[replay-census] replays=%u\n", allocation.TargetReplayCount());
    }
    return ARM64UnitReadyForEmission(module, allocation, block, function);
}

std::optional<u32> ARM64StandaloneHIRCapacity(const ir::Block& block) {
    if (!block.HasTerminal()) return std::nullopt;
    auto& instructions = block.GetInstList();
    if (instructions.size() != block.MaxInstrId()) return std::nullopt;
    std::vector<const ir::Inst*> local;
    local.reserve(instructions.size());
    u32 capacity = 0;
    const auto valid_type = [](ir::ValueType type) {
        return type > ir::ValueType::VOID && type <= ir::ValueType::V256;
    };
    for (auto& inst : instructions) {
        if (inst.Id() != capacity++ || inst.IsPseudoOperation() || inst.HasPseudoOperations() ||
            (inst.HasValue() && !valid_type(inst.ReturnType()))) return std::nullopt;
        switch (inst.GetOp()) {
        case ir::OpCode::Goto: case ir::OpCode::NotGoto:
        case ir::OpCode::BindLabel: case ir::OpCode::AddPhi:
            return std::nullopt;
        default: break;
        }
        local.push_back(&inst);
    }
    const auto owned = [&](const ir::Inst* inst) {
        return inst && inst->Id() < local.size() && local[inst->Id()] == inst;
    };
    const auto valid_value = [&](ir::Value value, u32 before) {
        return value.Defined() && valid_type(value.Type()) && owned(value.Def()) &&
            value.Id() < before && value.Def()->HasValue() && valid_type(value.Def()->ReturnType());
    };
    for (auto& inst : instructions) {
        bool valid = true;
        inst.ForEachValue([&](ir::Value value) { valid &= valid_value(value, inst.Id()); });
        if (!valid) return std::nullopt;
    }
    const auto& hoist = block.GetLoopHoistMetadata();
    if (hoist.anchors.size() != static_cast<size_t>(hoist.gpr_count) + hoist.const_count ||
        (!hoist.anchors.empty() && !hoist.prefix_end) ||
        (hoist.prefix_end && !owned(hoist.prefix_end))) return std::nullopt;
    for (const auto* anchor : hoist.anchors)
        if (!owned(anchor) || !ir::meta::HasValue(ir::GetIRMetaInfo(anchor->GetOp()).return_type) ||
            !valid_type(anchor->ReturnType())) return std::nullopt;
    const auto terminal_valid = [&](const auto& self, const ir::Terminal& terminal) -> bool {
        return VisitVariant<bool>(terminal, [&](const auto& term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::Invalid>) return false;
            else if constexpr (std::is_same_v<T, ir::terminal::If>) {
                return valid_value(term.cond, capacity) && term.cond.Type() == ir::ValueType::U8 &&
                    self(self, term.then_) && self(self, term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
                return self(self, term.then_) && self(self, term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                if (!valid_value(term.value, capacity) || term.value.Type() > ir::ValueType::S64) return false;
                return std::all_of(term.cases.begin(), term.cases.end(),
                        [&](const auto& item) { return self(self, item.then); });
            } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) return self(self, term.else_);
            else return true;
        });
    };
    return terminal_valid(terminal_valid, block.GetTerminal()) ? std::optional<u32>{capacity} : std::nullopt;
}
} // namespace swift::runtime::backend::arm64

namespace swift::runtime::backend::arm64 {
bool ARMTargetPlanner::CanUseZeroStoreRegister(ir::Value value) const {
    return TargetCanUseZeroStoreRegister(value, allocation, features, gpr_plan->consumers.get());
}
} // namespace swift::runtime::backend::arm64
