#include "effects.h"
#include "constraints.h"
#include "runtime/backend/arm64/compile/register_contract.h"
#include "runtime/backend/arm64/compile/write_successors.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/compile/analysis/terminal_uses.h"

#include <algorithm>
#include <bit>
#include <functional>

#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::backend::arm64 {

bool IsFixedGPRPublicationObserver(ir::OpCode op) {
    return TargetMayFaultOrObserve(op) || op == ir::OpCode::GetUniformAddress ||
           op == ir::OpCode::UniformBarrier;
}

bool IsFixedGPRPublicationObserver(const ir::Inst& inst, const FeatureSet& features) {
    // Legacy publication coalescers do not yet carry a target-specific
    // physical helper barrier at every use of this predicate. Keep their
    // conservative call boundary; fixed residence proofs separately use the
    // precise per-register HelperCallContract through RegisterStatePreparation.
    (void)features;
    return IsFixedGPRPublicationObserver(inst.GetOp());
}

namespace {

// This collector owns only semantic declarations. Physical placement and
// retry demotions belong to AllocationWriter after the input is installed.
class InitialConstraints final {
public:
    InitialConstraints(u32 count, GPRSMask pool) : count(count), pool(pool) {}
    [[nodiscard]] GPRSMask GetGprs() const { return pool; }
    [[nodiscard]] const compile::FixedGPRDefinition* FixedGPRDefinitionAt(u32 value) const {
        if (value_to_entry.empty() || value >= count || value_to_entry[value] == UINT32_MAX)
            return nullptr;
        return &entries[value_to_entry[value]].definition;
    }
    [[nodiscard]] bool HasFixedGPRDefinition(u32 value) const {
        return FixedGPRDefinitionAt(value) != nullptr;
    }
    void PlanFixedGPRDefinition(u32 value, compile::FixedGPRDefinition definition) {
        ASSERT(value < count && definition.home < 32 && IsFixedGPRHome(definition.home) &&
               pool.Get(definition.home));
        if (value_to_entry.empty()) value_to_entry.assign(count, UINT32_MAX);
        auto& entry = value_to_entry[value];
        if (entry == UINT32_MAX) {
            entry = static_cast<u32>(entries.size());
            entries.push_back({value, definition});
        } else {
            entries[entry].definition = definition;
        }
        index_dirty = true;
    }
    [[nodiscard]] const compile::FixedGPRHomeIndex& PrepareFixedGPRHomes() {
        if (index_dirty) {
            index.Clear();
            for (const auto& entry : entries) index.Add(entry.value, entry.definition);
            index.Finish();
            index_dirty = false;
        }
        return index;
    }
    [[nodiscard]] Vector<compile::FixedGPRInput> Finish() { return std::move(entries); }

private:
    const u32 count;
    const GPRSMask pool;
    Vector<u32> value_to_entry;
    Vector<compile::FixedGPRInput> entries;
    compile::FixedGPRHomeIndex index;
    bool index_dirty{true};
};

using compile::TerminalValueUses;

ir::Flags SetFlags(const compile::BlockAnalysisIndex& uses, const ir::Inst* producer) {
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

void DeclareCaptureVersions(const ir::Block* block,
                            InitialConstraints& allocation,
                            compile::SemanticFacts& facts,
                            RegisterStatePreparation& state) {
    if (GetSvmConfig().skip_prep.find("preservegpr") != std::string::npos) return;
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() != ir::OpCode::GetHostGPR || allocation.HasFixedGPRDefinition(inst.Id()))
            continue;
        const auto home = inst.GetArg<ir::Imm>(0).Get();
        if (!IsFixedGPRHome(home) || !allocation.GetGprs().Get(home)) continue;
        const auto offset = inst.GetArg<ir::Imm>(1).Get();
        const auto width = ir::GetValueSizeByte(inst.ReturnType());
        ASSERT(offset < 8 && width <= 8 && offset + width <= 8);
        const auto demand = facts.PrepareRegisterReads().Find(inst.Id());
        const u32 last = demand ? std::max<u32>(inst.Id(), demand->last_use) : inst.Id();
        if (width == 8 && offset == 0 && last <= block->GetInstList().back().Id() &&
            (!demand || demand->max_width <= width) &&
            state.PrepareHomeSurvival(home, inst.Id(), last)) {
            allocation.PlanFixedGPRDefinition(inst.Id(),
                                              {.home = static_cast<u16>(home),
                                               .end = last,
                                               .last_use = last,
                                               .owner = inst.Id(),
                                               .width = static_cast<u8>(width),
                                               .elided = true,
                                               .version = true});
            continue;
        }
        allocation.PlanFixedGPRDefinition(inst.Id(),
                                          {.home = static_cast<u16>(home),
                                           .end = inst.Id(),
                                           .transfer = inst.Id(),
                                           .last_use = last,
                                           .owner = inst.Id(),
                                           .width = static_cast<u8>(width),
                                           .bit_offset = static_cast<u8>(offset * 8),
                                           .elided = true,
                                           .preserved = true,
                                           .version = true});
    }
}

void PrepareBlock(const ir::Block* block,
                  InitialConstraints& allocation,
                  const FeatureSet& features,
                  const TerminalValueUses& terminal_values,
                  compile::BlockAnalysisIndex& uses,
                  RegisterStatePreparation& state,
                  compile::CompileWorkspace& workspace) {
    const auto& skip = GetSvmConfig().skip_prep;
    if (block->IsEmptyBlock() || skip.find("fixedgpr") != std::string::npos) return;
    if (skip.find("transfers") == std::string::npos &&
        skip.find("fixedtransfers") == std::string::npos) {
        for (auto& publication : block->GetInstList()) {
            if (publication.GetOp() != ir::OpCode::SetHostGPR) continue;
            FixedGPRTransferUses aliases{workspace};
            auto transfer = MatchFixedGPRTransfer(&publication, uses, state, aliases);
            if (!transfer || !allocation.GetGprs().Get(transfer->source) ||
                !allocation.GetGprs().Get(transfer->target) ||
                // Already alias-safe reads need no additional residence or
                // publication. Keep their existing allocation and dead stores.
                // Predeclaring them was measured to grow generated code in
                // 28 of 38 changed func_tests units (net +168 B) by
                // preempting coalescing's direct allocation, for about
                // 1.6% of translate time; code quality wins that trade.
                state.PrepareHomeSurvival(transfer->source, transfer->read->Id(), transfer->last_use))
                continue;
            bool unsafe = terminal_values.Contains(transfer->read);
            for (const auto& entry : transfer->aliases)
                unsafe |= terminal_values.Contains(uses.Instructions()[entry.position]);
            for (auto* inst : uses.Between(transfer->read->Id(), transfer->last_use + 1))
                if (IsControlBoundary(*inst)) unsafe = true;
            if (unsafe) continue;
            auto plan = [&](const ir::Inst* value) {
                const bool after = value->Id() > publication.Id();
                allocation.PlanFixedGPRDefinition(
                        value->Id(),
                        {
                                .home = after ? transfer->target : transfer->source,
                                .transferred_home =
                                        static_cast<u16>(after ? UINT16_MAX : transfer->target),
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
            for (const auto& entry : transfer->aliases) plan(uses.Instructions()[entry.position]);
        }
    }
    for (auto& read : block->GetInstList()) {
        if (skip.find("fixedreads") != std::string::npos || read.GetOp() != ir::OpCode::GetHostGPR)
            continue;
        if (allocation.HasFixedGPRDefinition(read.Id())) {
            continue;
        }
        if (terminal_values.Contains(&read)) {
            continue;
        }
        const auto home = MatchNarrowFixedGPRRead(
                read, uses, state, [&](const ir::Inst* p) { return SetFlags(uses, p); }, true);
        if (!home || !allocation.GetGprs().Get(*home)) {
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
             ir::GetValueSizeByte(consumer->ReturnType()) !=
                     ir::GetValueSizeByte(read.ReturnType()))) {
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
            continue;
        }
        // Step 5 demotion rule: the matcher plus PrepareHomeSurvival
        // prove a single resident definition spans the use, so this
        // is a hard pin (version=true), not a single-home candidate
        // the allocator may drop under pressure. Without it, target
        // Bind re-plans the same definition as a version and forces
        // an allocation replay to honour it.
        allocation.PlanFixedGPRDefinition(
                read.Id(),
                {
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
        for (auto& inst : block->GetInstList()) {
            if (IsControlBoundary(inst)) controls.push_back(inst.Id());
            if (HelperCallContract::Resolve(inst, features)) helpers.push_back(inst.Id());
        }
        for (auto& read : block->GetInstList()) {
            if (read.GetOp() != ir::OpCode::GetHostGPR || read.GetArg<ir::Imm>(1).Get() != 0 ||
                read.ReturnType() != ir::ValueType::U64 ||
                allocation.HasFixedGPRDefinition(read.Id()))
                continue;
            const auto home = read.GetArg<ir::Imm>(0).Get();
            if (!IsFixedGPRHome(home) || !allocation.GetGprs().Get(home)) continue;
            const auto split = state.PrepareFirstClobber(home, read.Id());
            if (!split) continue;
            const auto* split_instruction = uses.FindInstruction(*split);
            ASSERT(split_instruction != nullptr);
            if (split_instruction->GetOp() != ir::OpCode::SetHostGPR) continue;
            // Alias families must be wholly block-local. A terminal use or
            // branch could bypass this preservation point and needs CFG SSA.
            uses.PrepareUses();
            std::vector<const ir::Inst*> family{&read};
            u32 last = read.Id();
            bool safe = true;
            for (size_t i = 0; i < family.size() && safe; ++i) {
                auto* definition = family[i];
                if (terminal_values.Contains(definition)) {
                    safe = false;
                    break;
                }
                u32 count = 0;
                for (const auto& use : uses.Uses(definition)) {
                    count += use.count;
                    last = std::max<u32>(last, use.consumer->Id());
                    if (use.consumer->IsBitCastOperation()) {
                        if (ir::IsFloatValueType(use.consumer->ReturnType()) ||
                            ir::GetValueSizeByte(use.consumer->ReturnType()) >
                                    ir::GetValueSizeByte(read.ReturnType())) {
                            safe = false;
                            break;
                        }
                        // A unary BitCast has one parent, so a family rooted
                        // at a host read cannot reconverge or contain a cycle.
                        ASSERT(use.count == 1 &&
                               use.consumer->GetArg<ir::Value>(0).Def() == definition);
                        family.push_back(use.consumer);
                    }
                }
                safe &= count == definition->GetUses(false);
            }
            if (!safe || last <= *split) continue;
            const auto control = std::upper_bound(controls.begin(), controls.end(), read.Id());
            const auto helper = std::upper_bound(helpers.begin(), helpers.end(), read.Id());
            if ((control != controls.end() && *control <= last) ||
                (helper != helpers.end() && *helper < *split))
                continue;
            allocation.PlanFixedGPRDefinition(read.Id(),
                                              {
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

void PrepareEarlyAddressAliases(const ir::Block* block,
                                InitialConstraints& allocation,
                                const FeatureSet& features,
                                const TerminalValueUses& terminals,
                                compile::BlockAnalysisIndex& index,
                                RegisterStatePreparation& state) {
    const auto& skip = GetSvmConfig().skip_prep;
    if (block->IsEmptyBlock() || !features.ra_coalesce ||
        skip.find("fixedgpr") != std::string::npos ||
        skip.find("fixedresults") != std::string::npos)
        return;
    const auto local = [&](const ir::Inst* inst) {
        return inst && inst->Id() >= block->GetInstList().front().Id() &&
               inst->Id() <= block->GetInstList().back().Id();
    };
    const auto control_between = [&](u32 after, u32 before) {
        return std::ranges::any_of(index.Between(after, before),
                                   [](const ir::Inst* inst) { return IsControlBoundary(*inst); });
    };
    // Direct address aliases can share a proven borrowed home on the first
    // allocation. Extend the original read's contract before building indices.
    for (auto& alias : block->GetInstList()) {
        if (alias.GetOp() != ir::OpCode::GetOperand || alias.ReturnType() != ir::ValueType::U64 ||
            terminals.Contains(&alias) || allocation.HasFixedGPRDefinition(alias.Id()))
            continue;
        const auto operand = alias.GetArg<ir::Operand>(0);
        if (!operand.GetRight().Null() || !operand.GetLeft().IsValue()) continue;
        const auto value = operand.GetLeft().value;
        if (!local(value.Def()) || value.Type() != ir::ValueType::U64) continue;
        const auto* source = allocation.FixedGPRDefinitionAt(value.Id());
        if (!source || source->preserved || !source->elided || source->width != 8 ||
            source->transferred_home != UINT16_MAX)
            continue;
        u32 end = alias.Id(), count = 0;
        index.PrepareUses();
        for (const auto& use : index.Uses(&alias)) {
            end = std::max<u32>(end, use.consumer->Id());
            count += use.count;
        }
        if (!count || count != alias.GetUses(false) || control_between(value.Id(), end + 1) ||
            !state.PrepareHomeSurvival(source->home, value.Id(), end))
            continue;
        const auto home = source->home;
        auto extended = *source;
        extended.end = std::max(extended.end, end);
        extended.last_use = std::max(extended.last_use, end);
        allocation.PlanFixedGPRDefinition(value.Id(), extended);
        allocation.PlanFixedGPRDefinition(alias.Id(),
                                          {.home = home,
                                           .end = end,
                                           .last_use = end,
                                           .owner = value.Id(),
                                           .width = 8,
                                           .elided = true,
                                           .version = true});
    }
}

void PrepareEarlyResultHomes(const ir::Block* block,
                             InitialConstraints& allocation,
                             const FeatureSet& features,
                             const TerminalValueUses& terminals,
                             const compile::BlockAnalysisIndex& index,
                             RegisterStatePreparation& state,
                             std::vector<std::pair<u32, compile::FixedGPRDefinition>>& proposals) {
    const auto& skip = GetSvmConfig().skip_prep;
    if (block->IsEmptyBlock() || !features.ra_coalesce ||
        skip.find("fixedgpr") != std::string::npos ||
        skip.find("fixedresults") != std::string::npos)
        return;
    const auto local = [&](const ir::Inst* inst) {
        return inst && inst->Id() >= block->GetInstList().front().Id() &&
               inst->Id() <= block->GetInstList().back().Id();
    };
    const auto control_between = [&](u32 after, u32 before) {
        return std::ranges::any_of(index.Between(after, before),
                                   [](const ir::Inst* inst) { return IsControlBoundary(*inst); });
    };
    GPRWriteSuccessors successors;
    successors.Build(
            index,
            [&](const ir::Inst& inst) {
                return TargetMayFaultOrObserve(inst, features) || IsControlBoundary(inst);
            },
            [](const ir::Inst& inst) {
                return inst.GetArg<ir::Imm>(2).Get() == 0 &&
                       ir::GetValueSizeByte(inst.GetArg<ir::Value>(0).Type()) >= 4;
            });
    std::array<u32, 32> previous_end{};
    // Collect against one immutable home index, then commit as a batch.
    // Same-home proof windows must be disjoint; each publication also checks
    // guest reads, writes, observation/fault and physical clobber boundaries.
    for (auto& publication : block->GetInstList()) {
        if (publication.GetOp() != ir::OpCode::SetHostGPR ||
            publication.GetArg<ir::Imm>(2).Get() != 0 || successors.FollowingWrite(&publication))
            continue;
        const auto value = publication.GetArg<ir::Value>(0);
        const auto target = publication.GetArg<ir::Imm>(1).Get();
        if (!value.Defined() || !local(value.Def()) || value.Type() != value.Def()->ReturnType() ||
            !IsFixedGPRHome(target) || !allocation.GetGprs().Get(target))
            continue;
        const auto width = ir::GetValueSizeByte(value.Type());
        if (width != 4 && width != 8) continue;
        auto* producer = value.Def();
        bool wrapped = false, valid = true;
        while (producer->GetOp() == ir::OpCode::ZeroExtend32To64 ||
               producer->GetOp() == ir::OpCode::ZeroExtend32 ||
               producer->GetOp() == ir::OpCode::SignExtend) {
            if (producer->GetUses(false) != 1 || producer->HasPseudoOperations() ||
                terminals.Contains(producer)) {
                valid = false;
                break;
            }
            const auto input = producer->GetArg<ir::Value>(0);
            if (!input.Defined() || !local(input.Def()) ||
                input.Type() != input.Def()->ReturnType()) {
                valid = false;
                break;
            }
            producer = input.Def();
            wrapped = true;
        }
        if (!valid || producer->GetUses(false) != 1 || terminals.Contains(producer) ||
            allocation.HasFixedGPRDefinition(producer->Id()) || producer->Id() >= publication.Id())
            continue;
        const auto op = producer->GetOp();
        // Wrapped arithmetic has an allocation-dependent publication policy:
        // the existing direct-Add recipe is selected for a spilled result.
        // Leave that quality choice to the bounded late phase rather than
        // charging every unpressured arithmetic publication for speculation.
        if (wrapped && (op == ir::OpCode::Add || op == ir::OpCode::Sub || op == ir::OpCode::And))
            continue;
        if (op != ir::OpCode::LoadImm && op != ir::OpCode::LoadMemory && op != ir::OpCode::Add &&
            op != ir::OpCode::Xor && op != ir::OpCode::Or && op != ir::OpCode::AndNot &&
            op != ir::OpCode::Not && op != ir::OpCode::Neg && op != ir::OpCode::Mul &&
            op != ir::OpCode::LslImm && op != ir::OpCode::LsrImm && op != ir::OpCode::AsrImm &&
            op != ir::OpCode::RorImm && op != ir::OpCode::ByteSwap && op != ir::OpCode::BitClear &&
            op != ir::OpCode::LslValue && op != ir::OpCode::LsrValue &&
            op != ir::OpCode::AsrValue && op != ir::OpCode::RorValue && op != ir::OpCode::Adc &&
            op != ir::OpCode::Sbb && op != ir::OpCode::CondSelect && op != ir::OpCode::MulHigh &&
            op != ir::OpCode::LoadUniform && op != ir::OpCode::Select && op != ir::OpCode::Div &&
            op != ir::OpCode::Sub && op != ir::OpCode::And && op != ir::OpCode::SelectZero &&
            op != ir::OpCode::GetOperand && op != ir::OpCode::BitExtract)
            continue;
        const auto result_width = ir::GetValueSizeByte(producer->ReturnType());
        if (!result_width || result_width > 8 || (!wrapped && result_width != width) ||
            ((wrapped || std::next(block->GetInstList().iterator_to(*producer)) !=
                                 block->GetInstList().iterator_to(publication)) &&
             producer->HasPseudoOperations()))
            continue;
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
                unsafe |= TargetMayFaultOrObserve(*inst, features) ||
                          inst->GetOp() == ir::OpCode::GetUniformAddress ||
                          inst->GetOp() == ir::OpCode::UniformBarrier ||
                          (inst->GetOp() == ir::OpCode::GetHostGPR &&
                           inst->GetArg<ir::Imm>(0).Get() == target) ||
                          (inst->GetOp() == ir::OpCode::SetHostGPR &&
                           inst->GetArg<ir::Imm>(1).Get() == target);
            }
        }
        if (unsafe ||
            (!short_window &&
             !state.PreparePublicationWindow(
                     target, ir::Value{producer}, producer->Id(), publication.Id())) ||
            allocation.PrepareFixedGPRHomes().Conflicts(
                    target, producer->Id(), publication.Id(), producer->Id()))
            continue;
        previous_end[target] = publication.Id();
        proposals.push_back({producer->Id(),
                             {.home = static_cast<u16>(target),
                              .end = publication.Id(),
                              .last_use = publication.Id(),
                              .publication = publication.Id(),
                              .owner = producer->Id(),
                              .width = static_cast<u8>(result_width),
                              .version = true}});
    }
}

}  // namespace

Vector<compile::FixedGPRInput> BuildFixedGPRInputs(const ir::Block* block,
                                                   GPRSMask pool,
                                                   const FeatureSet& features,
                                                   compile::SemanticFacts& facts,
                                                   compile::CompileWorkspace& workspace) {
    InitialConstraints allocation{block->MaxInstrId(), pool};
    const TerminalValueUses terminal_values{block, TerminalValueUses::Aliases::Raw};
    auto& index = facts.PrepareIndex(block);
    RegisterStatePreparation state;
    state.BeginBlock(block, features, facts);
    PrepareBlock(block, allocation, features, terminal_values, index, state, workspace);
    DeclareCaptureVersions(block, allocation, facts, state);
    std::vector<std::pair<u32, compile::FixedGPRDefinition>> proposals;
    PrepareEarlyAddressAliases(block, allocation, features, terminal_values, index, state);
    PrepareEarlyResultHomes(block, allocation, features, terminal_values, index, state, proposals);
    for (const auto& [value, definition] : proposals)
        allocation.PlanFixedGPRDefinition(value, definition);
    return allocation.Finish();
}

Vector<compile::FixedGPRInput> BuildFixedGPRInputs(const ir::HIRFunction* function,
                                                   GPRSMask pool,
                                                   const FeatureSet& features,
                                                   compile::SemanticFacts& semantic_facts,
                                                   compile::CompileWorkspace& workspace) {
    InitialConstraints allocation{function->MaxInstrCount(), pool};
    const TerminalValueUses terminal_values{function, TerminalValueUses::Aliases::Raw};
    struct Analysis {
        const ir::Block* block;
        compile::BlockAnalysisIndex* index;
        RegisterStatePreparation state;
    };
    std::vector<Analysis> analyses;
    analyses.reserve(function->GetHIRBlocksRPO().size());
    for (auto& block : function->GetHIRBlocksRPO()) {
        auto* current = block.GetBlock();
        analyses.emplace_back();
        auto& facts = analyses.back();
        facts.block = current;
        facts.index = &semantic_facts.PrepareIndex(current);
        facts.state.BeginBlock(current, features, semantic_facts);
        PrepareBlock(current,
                     allocation,
                     features,
                     terminal_values,
                     *facts.index,
                     facts.state,
                     workspace);
    }
    for (auto& facts : analyses)
        DeclareCaptureVersions(facts.block, allocation, semantic_facts, facts.state);
    std::vector<std::pair<u32, compile::FixedGPRDefinition>> proposals;
    for (auto& facts : analyses)
        PrepareEarlyAddressAliases(
                facts.block, allocation, features, terminal_values, *facts.index, facts.state);
    for (auto& facts : analyses)
        PrepareEarlyResultHomes(facts.block,
                                allocation,
                                features,
                                terminal_values,
                                *facts.index,
                                facts.state,
                                proposals);
    for (const auto& [value, definition] : proposals)
        allocation.PlanFixedGPRDefinition(value, definition);
    return allocation.Finish();
}

}  // namespace swift::runtime::backend::arm64
