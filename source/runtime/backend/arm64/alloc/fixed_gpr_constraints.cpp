#include "fixed_gpr_constraints.h"
#include "unit_facts.h"

#include <algorithm>
#include <bit>
#include <functional>
#include <unordered_set>

#include <cstdio>
#include <cstdlib>

#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::backend::arm64 {

namespace {
// Env-gated early-declaration census (SVM_BINDCENSUS), companion to the
// Bind census in target_allocation_planner.cpp.
struct EarlyCensus {
    unsigned long long declared{};
    unsigned long long gate_width{};
    unsigned long long gate_control{};
    unsigned long long gate_terminal{};
    unsigned long long gate_has_fixed{};
    unsigned long long gate_pool{};
    unsigned long long gate_pool_homes{};
    unsigned long long no_match{};
};
EarlyCensus g_early_census;
bool g_early_census_registered{};
bool EarlyCensusEnabled() {
    static const bool enabled = std::getenv("SVM_BINDCENSUS") != nullptr;
    if (enabled && !g_early_census_registered) {
        g_early_census_registered = true;
        std::atexit([] {
            const auto& c = g_early_census;
            std::fprintf(stderr,
                "[svm-earlycensus] narrow declared=%llu gate_width=%llu "
                "gate_control=%llu gate_terminal=%llu gate_has_fixed=%llu gate_pool=%llu pool_homes=0x%llx no_match=%llu\n",
                c.declared, c.gate_width, c.gate_control, c.gate_terminal,
                c.gate_has_fixed, c.gate_pool, c.gate_pool_homes, c.no_match);
        });
    }
    return enabled;
}
} // namespace

bool IsFixedGPRPublicationObserver(ir::OpCode op) {
    return GuestStateMap::MayFaultOrObserve(op) || op == ir::OpCode::GetUniformAddress ||
           op == ir::OpCode::UniformBarrier;
}

bool IsFixedGPRPublicationObserver(const ir::Inst& inst, const FeatureSet& features) {
    // Legacy publication coalescers do not yet carry a target-specific
    // physical helper barrier at every use of this predicate. Keep their
    // conservative call boundary; fixed residence proofs separately use the
    // precise per-register HelperCallContract through GuestStateMap.
    (void)features;
    return IsFixedGPRPublicationObserver(inst.GetOp());
}

namespace {

using TerminalValues = std::unordered_set<ir::Inst*>;

void CollectTerminalValues(const ir::Terminal& terminal, TerminalValues& values) {
    VisitVariant<void>(terminal, [&](const auto& edge) {
        using T = std::decay_t<decltype(edge)>;
        auto add = [&](ir::Value value) {
            if (value.Defined()) values.insert(value.Def());
        };
        if constexpr (std::is_same_v<T, ir::terminal::If>) {
            add(edge.cond);
            CollectTerminalValues(edge.then_, values);
            CollectTerminalValues(edge.else_, values);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            add(edge.value);
            for (const auto& item : edge.cases) CollectTerminalValues(item.then, values);
        } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
            CollectTerminalValues(edge.then_, values);
            CollectTerminalValues(edge.else_, values);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            CollectTerminalValues(edge.else_, values);
        }
    });
}

ir::Flags SetFlags(const BlockAnalysisIndex& uses, ir::Inst* producer) {
    ir::Flags result{};
    uses.ForEachPseudoUse(producer, [&](const ir::Inst& pseudo) {
        if (pseudo.GetOp() == ir::OpCode::SaveFlags ||
            pseudo.GetOp() == ir::OpCode::BranchOnlyFlags) {
            result |= pseudo.GetArg<ir::Flags>(1);
        }
    });
    return result;
}

bool IsControlBoundary(const ir::Inst& inst) {
    return inst.GetOp() == ir::OpCode::Goto || inst.GetOp() == ir::OpCode::NotGoto ||
           inst.GetOp() == ir::OpCode::BindLabel;
}

struct VersionUseEnd { u32 point{}; u32 width{}; };
using VersionUseEnds = std::unordered_map<u32, VersionUseEnd>;

void CollectVersionUseEnds(ir::Block* block, VersionUseEnds& ends) {
    const auto add = [&](ir::Value value, u32 point) {
        if (!value.Defined()) return;
        const auto width = value.Type() == ir::ValueType::VOID ? 0 : ir::GetValueSizeByte(value.Type());
        while (value.Def()->IsBitCastOperation()) value = value.Def()->GetArg<ir::Value>(0);
        auto& end = ends[value.Id()];
        end.point = std::max(end.point, point);
        end.width = std::max<u32>(end.width, width);
    };
    for (auto& inst : block->GetInstList())
        for (auto value : inst.GetValues()) add(value, inst.Id());
    const u32 tail = block->IsEmptyBlock() ? 0 : block->GetInstList().back().Id();
    std::function<void(const ir::Terminal&)> walk;
    walk = [&](const ir::Terminal& terminal) {
        VisitVariant<void>(terminal, [&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, ir::terminal::If>) {
                add(node.cond, tail); walk(node.then_); walk(node.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                add(node.value, tail);
                for (const auto& item : node.cases) walk(item.then);
            } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
                walk(node.then_); walk(node.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) walk(node.else_);
        });
    };
    walk(block->GetTerminal());
    for (auto* anchor : block->GetLoopHoistMetadata().anchors) add(ir::Value{anchor}, tail);
}

void DeclareCaptureVersions(ir::Block* block, RegAlloc& allocation, const VersionUseEnds& ends,
                            GuestStateMap& state) {
    if (GetSvmConfig().skip_prep.find("preservegpr") != std::string::npos) return;
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() != ir::OpCode::GetHostGPR || allocation.HasFixedGPRDefinition(inst.Id())) continue;
        const auto home = inst.GetArg<ir::Imm>(0).Get();
        if (!IsFixedGPRHome(home) || !allocation.GetGprs().Get(home)) continue;
        const auto offset = inst.GetArg<ir::Imm>(1).Get();
        const auto width = ir::GetValueSizeByte(inst.ReturnType());
        ASSERT(offset < 8 && width <= 8 && offset + width <= 8);
        const auto found = ends.find(inst.Id());
        const u32 last = found == ends.end() ? inst.Id() : std::max<u32>(inst.Id(), found->second.point);
        if (width == 8 && offset == 0 && last <= block->GetInstList().back().Id() &&
            (found == ends.end() || found->second.width <= width) &&
            state.FixedHomeSurvives(home, inst.Id(), last)) {
            allocation.PlanFixedGPRDefinition(inst.Id(), {
                    .home = static_cast<u16>(home), .end = last, .last_use = last,
                    .owner = inst.Id(), .width = static_cast<u8>(width), .elided = true, .version = true});
            continue;
        }
        allocation.PlanFixedGPRDefinition(inst.Id(), {
                .home = static_cast<u16>(home), .end = inst.Id(), .transfer = inst.Id(),
                .last_use = last, .owner = inst.Id(), .width = static_cast<u8>(width),
                .bit_offset = static_cast<u8>(offset * 8), .elided = true, .preserved = true, .version = true});
    }
}

void PrepareBlock(ir::Block* block, RegAlloc& allocation, const FeatureSet& features,
                  const TerminalValues& terminal_values, BlockAnalysisIndex& uses, GuestStateMap& state) {
    const auto& skip = GetSvmConfig().skip_prep;
    if (block->IsEmptyBlock() || skip.find("fixedgpr") != std::string::npos) return;
    if (skip.find("transfers") == std::string::npos &&
        skip.find("fixedtransfers") == std::string::npos) {
        for (auto& publication : block->GetInstList()) {
            auto transfer = MatchFixedGPRTransfer(&publication, uses, state);
            if (!transfer || !allocation.GetGprs().Get(transfer->source) ||
                !allocation.GetGprs().Get(transfer->target) ||
                // Already alias-safe reads need no additional residence or
                // publication. Keep their existing allocation and dead stores.
                // Predeclaring them was measured to grow generated code in
                // 28 of 38 changed func_tests units (net +168 B) by
                // preempting coalescing's direct allocation, for about
                // 1.6% of translate time; code quality wins that trade.
                state.FixedHomeSurvives(transfer->source, transfer->read->Id(),
                                        transfer->last_use)) continue;
            bool unsafe = terminal_values.contains(transfer->read);
            for (auto* alias : transfer->aliases) unsafe |= terminal_values.contains(alias);
            for (auto* inst : uses.Between(transfer->read->Id(), transfer->last_use + 1))
                if (IsControlBoundary(*inst)) unsafe = true;
            if (unsafe) continue;
            auto plan = [&](ir::Inst* value) {
                const bool after = value->Id() > publication.Id();
                allocation.PlanFixedGPRDefinition(value->Id(), {
                        .home = after ? transfer->target : transfer->source,
                        .transferred_home = static_cast<u16>(after ? UINT16_MAX : transfer->target),
                        .end = after ? transfer->last_use : publication.Id(),
                        .transfer = after ? UINT32_MAX : publication.Id(),
                        .last_use = transfer->last_use,
                        .publication = publication.Id(),
                        .owner = transfer->read->Id(),
                        .width = sizeof(u64),
                        .elided = true,
                });
            };
            plan(transfer->read);
            for (auto* alias : transfer->aliases) plan(alias);
        }
    }
    const bool early_census = EarlyCensusEnabled();
    for (auto& read : block->GetInstList()) {
        if (skip.find("fixedreads") != std::string::npos ||
            read.GetOp() != ir::OpCode::GetHostGPR) continue;
        if (allocation.HasFixedGPRDefinition(read.Id())) {
            if (early_census) ++g_early_census.gate_has_fixed;
            continue;
        }
        if (terminal_values.contains(&read)) {
            if (early_census) ++g_early_census.gate_terminal;
            continue;
        }
        const auto home = MatchNarrowFixedGPRRead(read, uses, state,
                [&](ir::Inst* p) { return SetFlags(uses, p); }, true);
        if (!home || !allocation.GetGprs().Get(*home)) {
            if (early_census) {
                if (home) { ++g_early_census.gate_pool; g_early_census.gate_pool_homes |= 1ull << *home; }
                else ++g_early_census.no_match;
            }
            continue;
        }
        auto* consumer = uses.Uses(&read).front().consumer;
        // Target preparation binds these same matcher-proven views after a
        // replay; declaring them here only changes when the allocator first
        // sees the constraint. Narrow flags preparation is built from the
        // final allocation either way, so flags-setting consumers follow
        // the same path as exact-width ALU views, stores and extensions.
        const bool extension = consumer->GetOp() == ir::OpCode::ZeroExtend32 ||
                               consumer->GetOp() == ir::OpCode::ZeroExtend32To64 ||
                               consumer->GetOp() == ir::OpCode::SignExtend;
        if (!extension && consumer->GetOp() != ir::OpCode::StoreMemory &&
            ((consumer->GetUses() == 0 && SetFlags(uses, consumer) == ir::Flags::None) ||
             !consumer->HasValue() ||
             ir::GetValueSizeByte(consumer->ReturnType()) != ir::GetValueSizeByte(read.ReturnType()))) {
            if (early_census) ++g_early_census.gate_width;
            continue;
        }
        // Extension chains may be published directly by width coalescing,
        // and a completed width transaction replaces this elided
        // constraint with its own proof either way. Both extensions only
        // read the low bits of the W home, so they can be preallocated on
        // the same terms as an exact-width ALU view.
        bool control = false;
        for (auto* inst : uses.Between(read.Id(), consumer->Id()))
            if (IsControlBoundary(*inst)) control = true;
        if (control) {
            if (early_census) ++g_early_census.gate_control;
            continue;
        }
        if (early_census) ++g_early_census.declared;
        // Step 5 demotion rule: the matcher plus FixedHomeSurvives
        // prove a single resident definition spans the use, so this
        // is a hard pin (version=true), not a single-home candidate
        // the allocator may drop under pressure. Without it, target
        // Bind re-plans the same definition as a version and forces
        // an allocation replay to honour it.
        allocation.PlanFixedGPRDefinition(read.Id(), {
                .home = *home,
                .end = consumer->Id(),
                .last_use = consumer->Id(),
                .width = static_cast<u8>(ir::GetValueSizeByte(read.ReturnType())),
                .elided = true,
                .version = true,
        });
    }
    if (skip.find("preservegpr") == std::string::npos) {
        std::vector<u32> controls, helpers;
        std::unordered_map<u32, ir::Inst*> instructions_by_id;
        for (auto& inst : block->GetInstList()) {
            instructions_by_id.emplace(inst.Id(), &inst);
            if (IsControlBoundary(inst)) controls.push_back(inst.Id());
            if (HelperCallContract::Resolve(inst, features)) helpers.push_back(inst.Id());
        }
        for (auto& read : block->GetInstList()) {
            if (read.GetOp() != ir::OpCode::GetHostGPR ||
                read.GetArg<ir::Imm>(1).Get() != 0 ||
                read.ReturnType() != ir::ValueType::U64 ||
                allocation.HasFixedGPRDefinition(read.Id())) continue;
            const auto home = read.GetArg<ir::Imm>(0).Get();
            if (!IsFixedGPRHome(home) || !allocation.GetGprs().Get(home)) continue;
            const auto split = state.FirstFixedHomeClobber(home, read.Id());
            if (!split || instructions_by_id.at(*split)->GetOp() != ir::OpCode::SetHostGPR) continue;
            // Alias families must be wholly block-local. A terminal use or
            // branch could bypass this preservation point and needs CFG SSA.
            std::vector<ir::Inst*> family{&read};
            std::unordered_set<ir::Inst*> seen{&read};
            u32 last = read.Id();
            bool safe = true;
            for (size_t i = 0; i < family.size() && safe; ++i) {
                auto* definition = family[i];
                if (terminal_values.contains(definition)) { safe = false; break; }
                u32 count = 0;
                for (const auto& use : uses.Uses(definition)) {
                    count += use.count;
                    last = std::max<u32>(last, use.consumer->Id());
                    if (use.consumer->IsBitCastOperation()) {
                        if (ir::IsFloatValueType(use.consumer->ReturnType()) ||
                            ir::GetValueSizeByte(use.consumer->ReturnType()) > ir::GetValueSizeByte(read.ReturnType())) {
                            safe = false;
                            break;
                        }
                        if (seen.insert(use.consumer).second) family.push_back(use.consumer);
                    }
                }
                safe &= count == definition->GetUses(false);
            }
            if (!safe || last <= *split) continue;
            const auto control = std::upper_bound(controls.begin(), controls.end(), read.Id());
            const auto helper = std::upper_bound(helpers.begin(), helpers.end(), read.Id());
            if ((control != controls.end() && *control <= last) ||
                (helper != helpers.end() && *helper < *split)) continue;
            allocation.PlanFixedGPRDefinition(read.Id(), {
                    .home = static_cast<u16>(home),
                    .end = *split,
                    .transfer = *split,
                    .last_use = last,
                    .owner = read.Id(),
                    .width = sizeof(u64),
                    .elided = true,
                    .preserved = true,
            });
        }
    }

}

void PrepareEarlyAddressAliases(ir::Block* block, RegAlloc& allocation, const FeatureSet& features,
                             const TerminalValues& terminals, BlockAnalysisIndex& index, GuestStateMap& state) {
    const auto& skip = GetSvmConfig().skip_prep;
    if (block->IsEmptyBlock() || !features.ra_coalesce ||
        skip.find("fixedgpr") != std::string::npos || skip.find("fixedresults") != std::string::npos ||
        skip.find("targetbind") != std::string::npos) return;
    const auto local = [&](ir::Inst* inst) {
        return inst && inst->Id() >= block->GetInstList().front().Id() &&
               inst->Id() <= block->GetInstList().back().Id();
    };
    const auto control_between = [&](u32 after, u32 before) {
        return std::ranges::any_of(index.Between(after, before),
                                  [](ir::Inst* inst) { return IsControlBoundary(*inst); });
    };
    // Direct address aliases can share a proven borrowed home on the first
    // allocation. Extend the original read's contract before building indices.
    for (auto& alias : block->GetInstList()) {
        if (alias.GetOp() != ir::OpCode::GetOperand || alias.ReturnType() != ir::ValueType::U64 ||
            terminals.contains(&alias) || allocation.HasFixedGPRDefinition(alias.Id())) continue;
        const auto operand = alias.GetArg<ir::Operand>(0);
        if (!operand.GetRight().Null() || !operand.GetLeft().IsValue()) continue;
        const auto value = operand.GetLeft().value;
        if (!local(value.Def()) || value.Type() != ir::ValueType::U64) continue;
        const auto* source = allocation.FixedGPRDefinitionAt(value.Id());
        if (!source || source->preserved || !source->elided || source->width != 8 ||
            source->transferred_home != UINT16_MAX) continue;
        u32 end = alias.Id(), count = 0;
        for (const auto& use : index.Uses(&alias)) {
            end = std::max<u32>(end, use.consumer->Id());
            count += use.count;
        }
        if (!count || count != alias.GetUses(false) ||
            control_between(value.Id(), end + 1) || !state.FixedHomeSurvives(source->home, value.Id(), end)) continue;
        const auto home = source->home;
        auto extended = *source;
        extended.end = std::max(extended.end, end);
        extended.last_use = std::max(extended.last_use, end);
        allocation.PlanFixedGPRDefinition(value.Id(), extended);
        allocation.PlanFixedGPRDefinition(alias.Id(), {
                .home = home, .end = end, .last_use = end, .owner = value.Id(),
                .width = 8, .elided = true, .version = true});
    }

}

void PrepareEarlyResultHomes(ir::Block* block, RegAlloc& allocation, const FeatureSet& features,
                             const TerminalValues& terminals, BlockAnalysisIndex& index, GuestStateMap& state,
                             std::vector<std::pair<u32, RegAlloc::FixedGPRDefinition>>& proposals) {
    const auto& skip = GetSvmConfig().skip_prep;
    if (block->IsEmptyBlock() || !features.ra_coalesce ||
        skip.find("fixedgpr") != std::string::npos || skip.find("fixedresults") != std::string::npos ||
        skip.find("targetbind") != std::string::npos) return;
    const auto local = [&](ir::Inst* inst) {
        return inst && inst->Id() >= block->GetInstList().front().Id() &&
               inst->Id() <= block->GetInstList().back().Id();
    };
    const auto control_between = [&](u32 after, u32 before) {
        return std::ranges::any_of(index.Between(after, before),
                                  [](ir::Inst* inst) { return IsControlBoundary(*inst); });
    };
    index.BuildWriteSuccessors([&](const ir::Inst& inst) {
        return state.MayFaultOrObserve(inst) || IsControlBoundary(inst);
    }, [](const ir::Inst& inst) {
        return inst.GetArg<ir::Imm>(2).Get() == 0 &&
               ir::GetValueSizeByte(inst.GetArg<ir::Value>(0).Type()) >= 4;
    });
    std::array<u32, 32> previous_end{};
    // Collect against one immutable home index, then commit as a batch.
    // Same-home proof windows must be disjoint; each publication also checks
    // guest reads, writes, observation/fault and physical clobber boundaries.
    for (auto& publication : block->GetInstList()) {
        if (publication.GetOp() != ir::OpCode::SetHostGPR ||
            publication.GetArg<ir::Imm>(2).Get() != 0 || index.FollowingWrite(&publication)) continue;
        const auto value = publication.GetArg<ir::Value>(0);
        const auto target = publication.GetArg<ir::Imm>(1).Get();
        if (!value.Defined() || !local(value.Def()) ||
            value.Type() != value.Def()->ReturnType() || !IsFixedGPRHome(target) ||
            !allocation.GetGprs().Get(target)) continue;
        const auto width = ir::GetValueSizeByte(value.Type());
        if (width != 4 && width != 8) continue;
        auto* producer = value.Def();
        bool wrapped = false, valid = true;
        while (producer->GetOp() == ir::OpCode::ZeroExtend32To64 ||
               producer->GetOp() == ir::OpCode::ZeroExtend32 ||
               producer->GetOp() == ir::OpCode::SignExtend) {
            if (producer->GetUses(false) != 1 || producer->HasPseudoOperations() ||
                terminals.contains(producer)) { valid = false; break; }
            const auto input = producer->GetArg<ir::Value>(0);
            if (!input.Defined() || !local(input.Def()) || input.Type() != input.Def()->ReturnType()) {
                valid = false; break;
            }
            producer = input.Def();
            wrapped = true;
        }
        if (!valid || producer->GetUses(false) != 1 || terminals.contains(producer) ||
            allocation.HasFixedGPRDefinition(producer->Id()) || producer->Id() >= publication.Id()) continue;
        const auto op = producer->GetOp();
        // Wrapped arithmetic has an allocation-dependent publication policy:
        // the existing direct-Add recipe is selected for a spilled result.
        // Leave that quality choice to the bounded late phase rather than
        // charging every unpressured arithmetic publication for speculation.
        if (wrapped && (op == ir::OpCode::Add || op == ir::OpCode::Sub || op == ir::OpCode::And)) continue;
        if (op != ir::OpCode::LoadImm && op != ir::OpCode::LoadMemory && op != ir::OpCode::Add &&
            op != ir::OpCode::Xor && op != ir::OpCode::Or && op != ir::OpCode::AndNot &&
            op != ir::OpCode::Not && op != ir::OpCode::Neg && op != ir::OpCode::Mul &&
            op != ir::OpCode::LslImm && op != ir::OpCode::LsrImm && op != ir::OpCode::AsrImm &&
            op != ir::OpCode::RorImm && op != ir::OpCode::ByteSwap && op != ir::OpCode::BitClear &&
            op != ir::OpCode::LslValue && op != ir::OpCode::LsrValue && op != ir::OpCode::AsrValue &&
            op != ir::OpCode::RorValue && op != ir::OpCode::Adc && op != ir::OpCode::Sbb &&
            op != ir::OpCode::CondSelect && op != ir::OpCode::MulHigh && op != ir::OpCode::LoadUniform &&
            op != ir::OpCode::Select && op != ir::OpCode::Div &&
            op != ir::OpCode::Sub && op != ir::OpCode::And && op != ir::OpCode::SelectZero &&
            op != ir::OpCode::GetOperand && op != ir::OpCode::BitExtract) continue;
        const auto result_width = ir::GetValueSizeByte(producer->ReturnType());
        if (!result_width || result_width > 8 || (!wrapped && result_width != width) ||
            ((wrapped || std::next(block->GetInstList().iterator_to(*producer)) !=
                         block->GetInstList().iterator_to(publication)) &&
             producer->HasPseudoOperations())) continue;
        if (producer->Id() < previous_end[target]) continue;
        const auto window = index.Between(producer->Id(), publication.Id());
        const bool short_window = window.size() <= 8;
        bool unsafe = false;
        for (auto* inst : window) {
            unsafe |= IsControlBoundary(*inst) ||
                      (FixedGPRClobbers(*inst, features, true) & (1u << target)) != 0;
            if (short_window) {
                // Initial analysis has no fault-capture witnesses. A tiny
                // window can be checked directly without building its event
                // index; keep exactly the same read/observer boundaries.
                unsafe |= state.MayFaultOrObserve(*inst) ||
                          inst->GetOp() == ir::OpCode::GetUniformAddress ||
                          inst->GetOp() == ir::OpCode::UniformBarrier ||
                          (inst->GetOp() == ir::OpCode::GetHostGPR && inst->GetArg<ir::Imm>(0).Get() == target) ||
                          (inst->GetOp() == ir::OpCode::SetHostGPR && inst->GetArg<ir::Imm>(1).Get() == target);
            }
        }
        if (unsafe || (!short_window && !state.PublicationWindowSafe(
                    target, ir::Value{producer}, producer->Id(), publication.Id())) ||
            allocation.GPRTargetWriteConflict(target, producer->Id(), publication.Id(), producer->Id())) continue;
        previous_end[target] = publication.Id();
        proposals.push_back({producer->Id(), {.home = static_cast<u16>(target),
                .end = publication.Id(), .last_use = publication.Id(),
                .publication = publication.Id(), .owner = producer->Id(),
                .width = static_cast<u8>(result_width), .version = true}});
    }
}

}  // namespace

void PrepareFixedGPRConstraints(ir::Block* block, RegAlloc& allocation,
                                const FeatureSet& features) {
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() == ir::OpCode::GetHostGPR || inst.GetOp() == ir::OpCode::SetHostGPR) {
            const auto home = inst.GetArg<ir::Imm>(inst.GetOp() == ir::OpCode::GetHostGPR ? 0 : 1).Get();
            ASSERT(home < 31);
            allocation.DeclareGuestGPR(home);
        }
    }
    TerminalValues terminal_values;
    CollectTerminalValues(block->GetTerminal(), terminal_values);
    auto& index = allocation.Facts().Uses(block);
    GuestStateMap state; state.Analyze(block, features);
    PrepareBlock(block, allocation, features, terminal_values, index, state);
    VersionUseEnds ends;
    CollectVersionUseEnds(block, ends);
    DeclareCaptureVersions(block, allocation, ends, state);
    std::vector<std::pair<u32, RegAlloc::FixedGPRDefinition>> proposals;
    PrepareEarlyAddressAliases(block, allocation, features, terminal_values, index, state);
    PrepareEarlyResultHomes(block, allocation, features, terminal_values, index, state, proposals);
    for (const auto& [value, definition] : proposals) allocation.PlanFixedGPRDefinition(value, definition);
}

void PrepareFixedGPRConstraints(ir::HIRFunction* function, RegAlloc& allocation,
                                const FeatureSet& features) {
    for (auto& block : function->GetHIRBlocksRPO()) {
        for (auto& inst : block.GetBlock()->GetInstList()) {
            if (inst.GetOp() == ir::OpCode::GetHostGPR || inst.GetOp() == ir::OpCode::SetHostGPR) {
                const auto home = inst.GetArg<ir::Imm>(inst.GetOp() == ir::OpCode::GetHostGPR ? 0 : 1).Get();
                ASSERT(home < 31);
                allocation.DeclareGuestGPR(home);
            }
        }
    }
    TerminalValues terminal_values;
    for (auto& block : function->GetHIRBlocksRPO())
        CollectTerminalValues(block.GetBlock()->GetTerminal(), terminal_values);
    struct Analysis { ir::Block* block; BlockAnalysisIndex* index; GuestStateMap state; };
    std::vector<Analysis> analyses;
    analyses.reserve(function->GetHIRBlocksRPO().size());
    VersionUseEnds ends;
    for (auto& block : function->GetHIRBlocksRPO()) {
        auto* current = block.GetBlock();
        analyses.emplace_back();
        auto& facts = analyses.back(); facts.block = current;
        facts.index = &allocation.Facts().Uses(current); facts.state.Analyze(current, features);
        PrepareBlock(current, allocation, features, terminal_values, *facts.index, facts.state);
        CollectVersionUseEnds(current, ends);
    }
    for (auto& facts : analyses) DeclareCaptureVersions(facts.block, allocation, ends, facts.state);
    std::vector<std::pair<u32, RegAlloc::FixedGPRDefinition>> proposals;
    for (auto& facts : analyses)
        PrepareEarlyAddressAliases(facts.block, allocation, features, terminal_values, *facts.index, facts.state);
    for (auto& facts : analyses)
        PrepareEarlyResultHomes(facts.block, allocation, features, terminal_values, *facts.index, facts.state, proposals);
    for (const auto& [value, definition] : proposals) allocation.PlanFixedGPRDefinition(value, definition);
}

static bool FixedGPRConstraintsHold(ir::Block* block, const RegAlloc& allocation,
                                    const FeatureSet& features,
                                    std::span<const GPRPhysicalWrite> target_writes,
                                    bool assert_on_fail, u32* fail_kind = nullptr) {
    bool has_constraints = false;
    for (auto& inst : block->GetInstList()) has_constraints |= allocation.HasFixedGPRDefinition(inst.Id());
    if (!has_constraints) return true;
    bool ok = true;
    std::array<std::vector<u32>, 32> writers;
    std::unordered_map<u64, std::pair<u32, u32>> consumed_inputs;
    // Block-local definition lookup by id: a sorted pointer vector
    // avoids building a hash table for every checked block.
    std::vector<ir::Inst*> definitions;
    definitions.reserve(block->GetInstList().size());
    const auto find_definition = [&](u32 id) -> ir::Inst* {
        const auto it = std::lower_bound(definitions.begin(), definitions.end(), id,
                [](const ir::Inst* inst, u32 value) { return inst->Id() < value; });
        return it != definitions.end() && (*it)->Id() == id ? *it : nullptr;
    };
    for (auto& inst : block->GetInstList()) {
        definitions.push_back(&inst);
        u32 mask = FixedGPRClobbers(inst, features);
        if (inst.GetOp() == ir::OpCode::SetHostGPR && !allocation.IsHostWriteCoalesced(inst.Id()))
            mask |= 1u << inst.GetArg<ir::Imm>(1).Get();
        if (const auto helper = HelperCallContract::Resolve(inst, features)) {
            for (u32 home = 0; home <= 9; ++home)
                if (helper->ClobbersGPR(home)) mask |= 1u << home;
        }
        if (inst.HasValue() && !inst.IsBitCastOperation() &&
            !(allocation.IsWidthChainCoalesced(inst.Id()) &&
              (inst.GetOp() == ir::OpCode::ZeroExtend32To64 || inst.GetOp() == ir::OpCode::BitExtract ||
               inst.GetOp() == ir::OpCode::SignExtend)) &&
            !allocation.IsFixedGPRDefinitionElided(inst.Id()) &&
            allocation.ValueType(ir::Value{&inst}) == RegAlloc::GPR) {
            const auto home = allocation.ValueGPR(ir::Value{&inst}).id;
            const bool read_alias = inst.GetOp() == ir::OpCode::GetHostGPR &&
                    inst.GetArg<ir::Imm>(0).Get() == home &&
                    (ir::GetValueSizeByte(inst.ReturnType()) == 8 ||
                     allocation.IsHostReadCoalesced(inst.Id()));
            if (!read_alias) mask |= 1u << home;
        }
        while (mask) {
            writers[std::countr_zero(mask)].push_back(inst.Id());
            mask &= mask - 1;
        }
    }
    std::sort(definitions.begin(), definitions.end(),
              [](const ir::Inst* a, const ir::Inst* b) { return a->Id() < b->Id(); });
    for (const auto& write : target_writes) {
        ASSERT(write.home < 31);
        writers[write.home].push_back(write.instruction);
        if (write.consumed_value != UINT32_MAX) {
            ASSERT(write.instruction < write.consuming_use);
            ASSERT(find_definition(write.instruction) && find_definition(write.consuming_use));
            const auto key = (static_cast<u64>(write.home) << 32) | write.consumed_value;
            ASSERT(consumed_inputs.emplace(key, std::pair{write.instruction, write.consuming_use}).second);
        }
    }
    for (auto& ids : writers) std::sort(ids.begin(), ids.end());
    auto check = [&](u32 value, u16 home, u32 after, u32 before) {
        if (!consumed_inputs.empty()) {
            const auto found = consumed_inputs.find((static_cast<u64>(home) << 32) | value);
            if (found != consumed_inputs.end() && before == found->second.second)
                before = found->second.first;
        }
        auto next = std::upper_bound(writers[home].begin(), writers[home].end(), after);
        const auto* residence = allocation.FixedGPRDefinitionAt(value);
        for (; next != writers[home].end() && *next < before; ++next) {
            bool transparent = false;
            // A full-width residence whose value is a 32-bit-fitting
            // constant survives an extension-chain rewrite of its home:
            // the chain reproduces exactly the same bits.  This mirrors
            // the LoadImm case in the W-write predicates.
            const auto* value_definition = find_definition(value);
            const bool narrow_constant =
                    value_definition &&
                    value_definition->GetOp() == ir::OpCode::LoadImm &&
                    (value_definition->GetArg<ir::Imm>(0).Get() >> 32) == 0;
            auto* writer = find_definition(*next);
            if (residence && (residence->width <= 4 || narrow_constant) && writer) {
                ir::Value source;
                if (writer->GetOp() == ir::OpCode::SetHostGPR) source = writer->GetArg<ir::Value>(0);
                else if (writer->GetOp() == ir::OpCode::ZeroExtend32To64 || writer->GetOp() == ir::OpCode::ZeroExtend32 ||
                         writer->GetOp() == ir::OpCode::ZeroExtend64 || writer->GetOp() == ir::OpCode::SignExtend)
                    source = writer->GetArg<ir::Value>(0);
                while (source.Defined() && source.Id() != value) {
                    auto* parent = source.Def();
                    if (parent->IsBitCastOperation() || parent->GetOp() == ir::OpCode::ZeroExtend32To64 ||
                        parent->GetOp() == ir::OpCode::ZeroExtend32 || parent->GetOp() == ir::OpCode::ZeroExtend64 ||
                        parent->GetOp() == ir::OpCode::SignExtend) source = parent->GetArg<ir::Value>(0);
                    else break;
                }
                transparent = source.Defined() && source.Id() == value &&
                              ir::GetValueSizeByte(source.Type()) >= residence->width;
            }
            if (!transparent) break;
        }
        const bool clear = next == writers[home].end() || *next >= before;
        if (assert_on_fail) {
            ASSERT_MSG(clear,
                       "fixed GPR value {} ({}, width {}) in x{} is overwritten by IR {} ({}) before use {}",
                       value, find_definition(value) ? find_definition(value)->GetOp() : ir::OpCode::Void,
                       residence ? residence->width : 0, home, next == writers[home].end() ? UINT32_MAX : *next,
                       next != writers[home].end() && find_definition(*next) ? find_definition(*next)->GetOp() : ir::OpCode::Void, before);
        } else {
            if (!clear && fail_kind && !*fail_kind) *fail_kind = 2;
            ok &= clear;
        }
    };
    for (auto& inst : block->GetInstList()) {
        const auto* fixed = allocation.FixedGPRDefinitionAt(inst.Id());
        if (!fixed) continue;
        if (fixed->preserved) {
            const bool shape = fixed->end == fixed->transfer && fixed->transfer <= fixed->last_use &&
                    fixed->width <= sizeof(u64) && fixed->width * 8 + fixed->bit_offset <= 64;
            if (assert_on_fail) ASSERT(shape); else { if (!shape && fail_kind && !*fail_kind) *fail_kind = 3; ok &= shape; }
            const auto kind = allocation.ValueType(ir::Value{&inst});
            const bool kind_ok = kind == RegAlloc::GPR || kind == RegAlloc::MEM;
            if (assert_on_fail) ASSERT(kind_ok); else { if (!kind_ok && fail_kind && !*fail_kind) *fail_kind = 3; ok &= kind_ok; }
            if (kind == RegAlloc::GPR) {
                const bool displaced = allocation.ValueGPR(ir::Value{&inst}).id != fixed->home;
                if (assert_on_fail) ASSERT(displaced); else { if (!displaced && fail_kind && !*fail_kind) *fail_kind = 3; ok &= displaced; }
            }
        } else {
            const auto kind = allocation.ValueType(ir::Value{&inst});
            const bool placed = kind == RegAlloc::GPR &&
                    allocation.ValueGPR(ir::Value{&inst}).id == fixed->home;
            if (assert_on_fail) {
                ASSERT(kind == RegAlloc::GPR);
                ASSERT(allocation.ValueGPR(ir::Value{&inst}).id == fixed->home);
            } else {
                if (!placed && fail_kind && !*fail_kind) *fail_kind = 1;
                ok &= placed;
            }
        }
        check(inst.Id(), fixed->home, inst.Id(), fixed->end);
        if (fixed->transferred_home != UINT16_MAX)
            check(inst.Id(), fixed->transferred_home, fixed->transfer, fixed->last_use);
    }
    return ok;
}

void VerifyFixedGPRConstraints(ir::Block* block, const RegAlloc& allocation,
                               const FeatureSet& features,
                               std::span<const GPRPhysicalWrite> target_writes) {
    (void)FixedGPRConstraintsHold(block, allocation, features, target_writes, true);
}

bool CheckFixedGPRConstraints(ir::Block* block, const RegAlloc& allocation,
                              const FeatureSet& features,
                              std::span<const GPRPhysicalWrite> target_writes,
                              u32* fail_kind) {
    return FixedGPRConstraintsHold(block, allocation, features, target_writes, false, fail_kind);
}

}  // namespace swift::runtime::backend::arm64
