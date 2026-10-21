#include "pinned_gpr_allocation.h"
#include "runtime/common/perf_stats.h"
#include "unit_facts.h"
#include "gpr_block_plan.h"
#include "runtime/backend/arm64/jit/div128_narrowing_analysis.h"

#include "runtime/common/svm_config.h"
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::arm64 {

void GPRPlanBuilder::Prepare(ir::Block* current) {
    finished = false;
    block = current;
    block_analysis = allocation.Facts().Uses(block);
    recipes = {};
    if (!allocation.TargetPlanning()) allocation.BeginGPRLocationAnalysis();
    generation = allocation.GPRLocationGeneration();
    allocation_attempt = allocation.AllocationAttempt();
    target_writes.clear();
    facts = {};
    zero_extends.clear();
    sign_extends.clear();
    const auto& skip = GetSvmConfig().skip_prep;
    const auto want = [&](const char* name) {
        return skip.find(name) == std::string::npos;
    };
    {
        PerfScope2 perf_dead{GetPerfStats2().builder_dead};
        PrepareDeadPinnedGPRWrites(block);
        for (const auto& use : guest_state_map.FixedHomeUses()) {
            allocation.RegisterGPRUseHome(use.definition->Id(), use.consumer->Id(), use.location, false);
        }
    }
    if (want("gprcopies")) PreparePinnedGPRCopies(block);
    if (want("selectpub")) PreparePinnedSelectPublications(block);
    if (want("transfers")) PreparePinnedGPRValueTransfers(block);
    if (want("pubviews")) PreparePinnedGPRPublicationViews(block);
}

GPRPlanDraft GPRPlanBuilder::FinishConstraints() {
    ASSERT(!finished);
    for (const auto& [publication, copy] : recipes.copies)
        if (!copy.source) RecordPhysicalWrite(copy.read, copy.target);
    for (const auto& [producer, select] : recipes.select_results) {
        RecordPhysicalWrite(producer, select.target);
        facts.result_homes.emplace(producer, select.target);
    }
    for (const auto& [publication, spill] : recipes.spilled_publications)
        RecordPhysicalWrite(spill.producer, spill.target);
    for (auto& inst : block->GetInstList()) {
        if (ValueFullyResident(&inst)) facts.resident_values.insert(&inst);
    }
    finished = true;
    GPRPlanDraft draft{block, std::move(facts), std::move(target_writes)};
    recipes = {};
    block_analysis = {};
    return draft;
}

std::shared_ptr<const FrozenGPRPlan> GPRPlanBuilder::Freeze() {
    if (!allocation.TargetPlanning())
        ASSERT(generation == allocation.GPRLocationGeneration() &&
               allocation_attempt == allocation.AllocationAttempt());
    auto draft = FinishConstraints();
    if (!allocation.TargetPlanning())
        VerifyFixedGPRConstraints(block, allocation, features, draft.writes);
    auto plan = FrozenGPRPlan::Seal(allocation, std::move(draft));
    if (!allocation.TargetPlanning()) allocation.FreezeGPRLocations();
    return plan;
}

std::shared_ptr<const FrozenGPRPlan> FrozenGPRPlan::Seal(RegAlloc& allocation, GPRPlanDraft draft) {
    return std::shared_ptr<const FrozenGPRPlan>{new FrozenGPRPlan{allocation, std::move(draft), nullptr}};
}

UnitScratchFacts PrepareUnitScratch(const GPRSMask& pool, const FeatureSet& features) {
    UnitScratchFacts result;
    result.prepared = true;
    result.extended_pool = ScratchXPoolEnabled(features);
    result.scratch_only = X86PinExtScratchOnlyEnabled(pool, features);
    result.level3_indirect = X86PinExtLevel3Enabled(pool);
    if (result.scratch_only) {
        result.terminal_leases = 1u << 13;
        if (!GetSvmConfig().flags_regs) result.terminal_leases |= 1u << 12;
    }
    return result;
}

static InstructionScratchFacts BuildInstructionScratch(
        const ir::Inst& inst, const GPRSMask& pool, const FeatureSet& features,
        const UnitScratchFacts& unit, bool has_value_uses, FlagsDemand demand) {
    InstructionScratchFacts result;
    result.fixed_clobbers = FixedGPRClobbers(inst, features, unit.scratch_only);
    // VIXL accounting has always excluded the default fixed-clobber mask,
    // which differs from the explicit level-2 scratch-only reservation mask.
    // Without XPOOL the default mask is zero except for XCHG's published
    // home, which is already included in the explicit scratch-only mask.
    result.audit_clobbers = !unit.scratch_only || inst.GetOp() == ir::OpCode::XchgBarrier
            ? result.fixed_clobbers : 0;
    result.budget = ScratchBudget(inst, features, demand);
    result.leases = unit.terminal_leases & ~result.fixed_clobbers;
    if (X86PinExtLevel3AluScratchEnabled(unit.level3_indirect && pool.Get(10), inst.GetOp()))
        result.leases |= 1u << 10;
    result.vixl_x87 = inst.GetOp() == ir::OpCode::X87Op;
    result.has_value_uses = has_value_uses;
    result.spill_consumer = ClassifySpillForwardConsumer(inst.GetOp());
    if (inst.GetOp() == ir::OpCode::Mul) {
        const auto right = inst.GetArg<ir::Operand>(1);
        const auto input = right.GetLeft();
        result.unobserved_mul_right = right.GetRight().Null() && input.IsValue() &&
                input.value.Defined() &&
                input.value.Def()->GetUses(false) == input.value.Def()->GetUses();
    }
    return result;
}

InstructionScratchFacts PrepareInstructionScratch(
        const ir::Inst& inst, const GPRSMask& pool, const FeatureSet& features) {
    return PrepareInstructionScratchWithBlock(inst, pool, features, nullptr);
}

InstructionScratchFacts PrepareInstructionScratchWithBlock(
        const ir::Inst& inst, const GPRSMask& pool, const FeatureSet& features, ir::Block* block) {
    auto result = BuildInstructionScratch(inst, pool, features, PrepareUnitScratch(pool, features), inst.GetUses() != 0,
            block ? ScanFlagsDemand(*block, inst) : FlagsDemand{});
    if (inst.GetOp() == ir::OpCode::Div128)
        result.div128_native = Div128NarrowingAnalysis{block}.CanLowerNatively(&inst);
    return result;
}

ScratchPreparationInputs CaptureScratchPreparationInputs(
        const GPRSMask& pool, const FeatureSet& features) {
    const auto& config = GetSvmConfig();
    return {pool, config.x86_pin_ext, config.exec_prof, config.static_regs, config.flags_regs,
            features.jit_scratch_xpool, features.scratch_precise,
            features.fpr_scratch_precise, features.sse_nan_coldpath};
}

bool FrozenGPRPlan::MatchesScratchInputs(const GPRSMask& pool, const FeatureSet& features) const {
    // Standalone/manual sealing retains its independent, complete input view.
    if (scratch_inputs) return *scratch_inputs == CaptureScratchPreparationInputs(pool, features);
    if (!UsesUnitScratchInputs() || !MatchesAllocation()) return false;
    const auto* actions = allocation.GPRPlanViewFor(block);
    return actions && actions->pinned.get() == this && actions->region &&
           pool == allocation.GetGprs() && actions->region->MatchesScratchInputs(pool, features);
}

std::shared_ptr<const FrozenGPRPlan> FrozenGPRPlan::SealWithSpillClobbers(
        RegAlloc& allocation, GPRPlanDraft draft, const FeatureSet& spill_features) {
    return std::shared_ptr<const FrozenGPRPlan>{new FrozenGPRPlan{allocation, std::move(draft), &spill_features}};
}

std::shared_ptr<const FrozenGPRPlan> FrozenGPRPlan::SealForUnit(
        RegAlloc& allocation, GPRPlanDraft draft, const FeatureSet& features,
        const UnitScratchFacts& unit, const BlockAnalysisIndex& analysis) {
    ASSERT(allocation.TargetPlanning() && unit.prepared);
    return std::shared_ptr<const FrozenGPRPlan>{new FrozenGPRPlan{allocation, std::move(draft), &features, &unit, &analysis}};
}

FrozenGPRPlan::FrozenGPRPlan(RegAlloc& allocation, GPRPlanDraft draft,
        const FeatureSet* spill_features, const UnitScratchFacts* unit, const BlockAnalysisIndex* analysis)
        : allocation(allocation), block(draft.block), facts(std::move(draft.facts)),
          writes(std::move(draft.writes)), generation(allocation.GPRLocationGeneration()),
          allocation_attempt(allocation.AllocationAttempt()) {
    const size_t instruction_count = block->GetInstList().size();
    if (spill_features) {
        if (!unit) scratch_inputs = CaptureScratchPreparationInputs(allocation.GetGprs(), *spill_features);
        unit_scratch = unit ? *unit : PrepareUnitScratch(allocation.GetGprs(), *spill_features);
        instruction_scratch.reserve(instruction_count);
    }
    spill_clobber_ids_ordered = spill_features != nullptr;
    bool have_scratch_id = false;
    u32 previous_scratch_id = 0;
    const Div128NarrowingAnalysis div128{block, analysis};
    // Distinct SaveFlags owners per block are few; a linear-scan
    // vector avoids a hash table allocation for every sealed block.
    std::vector<std::pair<u32, FlagsDemand>> flags_demands;
    const auto find_demand = [&](u32 id) -> FlagsDemand* {
        for (auto& [key, demand] : flags_demands)
            if (key == id) return &demand;
        return nullptr;
    };
    if (spill_features) {
        for (auto& inst : block->GetInstList()) {
            const auto op = inst.GetOp();
            if (op != ir::OpCode::SaveFlags && op != ir::OpCode::BranchOnlyFlags) continue;
            if (!inst.ArgAt(0).IsValue()) continue;
            auto* owner = inst.GetArg<ir::Value>(0).Def();
            if (!owner) continue;
            auto* found = find_demand(owner->Id());
            if (!found) {
                flags_demands.emplace_back(owner->Id(), FlagsDemand{});
                found = &flags_demands.back().second;
            }
            auto& demand = *found;
            demand.requested |= inst.GetArg<ir::Flags>(1);
            demand.pseudo_count++;
            demand.branch_only |= op == ir::OpCode::BranchOnlyFlags;
            demand.has_save |= op == ir::OpCode::SaveFlags;
        }
    }
    u32 spill_clobbers = 0;
    auto spill_consumer = SpillForwardConsumerKind::Ordinary;
    const auto capture = [&](ir::Value value, const ir::Inst& consumer, u8 argument, u32 element) {
        if (!value.Defined()) return;
        auto spill_input = OperandPlan::SpillInputState::Unprepared;
        if (analysis && argument != UINT8_MAX && allocation.ValueType(value) == RegAlloc::MEM &&
            analysis->Contains(value.Def())) {
            const auto uses = analysis->Uses(value.Def());
            const bool all_direct_uses = uses.size() == 1 && uses.front().consumer == &consumer &&
                                        uses.front().count == value.Def()->GetUses(false);
            spill_input = all_direct_uses ? OperandPlan::SpillInputState::Last : OperandPlan::SpillInputState::Live;
        }
        operands.push_back({&consumer, argument, spill_consumer, element, value, value.Type(),
                           spill_features != nullptr, spill_input, spill_clobbers});
    };
    for (auto& inst : block->GetInstList()) {
        const bool has_value_uses = inst.GetUses() != 0;
        if (spill_features) {
            // Keep the ordering certificate in this walk instead of
            // loading every ID once more in a count-only walk.
            if (have_scratch_id && previous_scratch_id >= inst.Id())
                spill_clobber_ids_ordered = false;
            previous_scratch_id = inst.Id();
            have_scratch_id = true;
            const auto* demand = find_demand(inst.Id());
            auto scratch = BuildInstructionScratch(inst, allocation.GetGprs(), *spill_features, unit_scratch, has_value_uses,
                    demand ? *demand : FlagsDemand{});
            if (inst.GetOp() == ir::OpCode::Div128) {
                scratch.div128_native = div128.CanLowerNatively(&inst);
            }
            instruction_scratch.push_back({&inst, scratch});
            spill_clobbers = scratch.fixed_clobbers;
            spill_consumer = scratch.spill_consumer;
        }
        if (inst.HasValue()) capture(ir::Value{&inst}, inst, UINT8_MAX, 0);
        for (u8 i = 0; i < ir::Inst::max_args; ++i) {
            auto& arg = inst.ArgAt(i);
            if (arg.IsValue()) capture(arg.Get<ir::Value>(), inst, i, 0);
            else if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue())
                capture(arg.Get<ir::Lambda>().GetValue(), inst, i, 0);
            else if (arg.IsParams()) {
                u32 element = 0;
                for (const auto& param : arg.Get<ir::Params>()) {
                    if (param.data.IsValue()) capture(param.data.value, inst, i, element);
                    ++element;
                }
            }
        }
    }

}

SpillInputFacts PrepareSpillInput(ir::Inst* consumer, u32 value_id) {
    SpillInputFacts result;
    u32 direct_uses = 0;
    if (consumer) consumer->ForEachValue([&](ir::Value value) {
        if (value.Defined() && value.Id() == value_id) {
            result.definition = value.Def();
            ++direct_uses;
        }
    });
    result.last_use = result.definition && result.definition->GetUses(false) == direct_uses;
    return result;
}

SpillInputFacts FrozenGPRPlan::SpillInputFor(const ir::Inst* consumer, u32 value_id) const {
    if (!consumer || !UsesUnitScratchInputs()) return {};
    auto first = operands.begin();
    if (spill_clobber_ids_ordered)
        first = std::lower_bound(first, operands.end(), consumer->Id(),
                [](const OperandPlan& operand, u32 id) { return operand.consumer->Id() < id; });
    SpillInputFacts result;
    for (auto it = first; it != operands.end(); ++it) {
        if (spill_clobber_ids_ordered && it->consumer->Id() != consumer->Id()) break;
        if (it->consumer != consumer || it->argument == UINT8_MAX || it->value.Id() != value_id) continue;
        // A foreign producer with a colliding ID cannot certify the local one.
        if (it->spill_input == OperandPlan::SpillInputState::Unprepared) return {};
        result = {it->value.Def(), it->spill_input == OperandPlan::SpillInputState::Last};
    }
    return result;
}

const InstructionScratchFacts* FrozenGPRPlan::ScratchFor(const ir::Inst* consumer) const {
    size_t next = instruction_scratch.size();
    return FindScratchFor(consumer, next);
}

const InstructionScratchFacts* FrozenGPRPlan::FindScratchFor(const ir::Inst* consumer, size_t& next) const {
    if (!consumer) return nullptr;
    if (!spill_clobber_ids_ordered) {
        // Non-monotonic IDs are supported by the public manual allocation API.
        for (size_t i = 0; i < instruction_scratch.size(); ++i)
            if (instruction_scratch[i].consumer == consumer) {
                next = i + 1;
                return &instruction_scratch[i].facts;
            }
        return nullptr;
    }
    const auto found = std::lower_bound(instruction_scratch.begin(), instruction_scratch.end(), consumer->Id(),
            [](const InstructionScratchRecord& record, u32 id) { return record.consumer->Id() < id; });
    if (found == instruction_scratch.end() || found->consumer != consumer) return nullptr;
    next = static_cast<size_t>(found - instruction_scratch.begin()) + 1;
    return &found->facts;
}

const FrozenGPRPlan::OperandPlan* FrozenGPRPlan::SpillConsumerFor(const ir::Inst* consumer) const {
    if (!consumer || !spill_clobber_ids_ordered) return nullptr;
    const auto found = std::lower_bound(operands.begin(), operands.end(), consumer->Id(),
            [](const OperandPlan& operand, u32 id) { return operand.consumer->Id() < id; });
    if (found == operands.end() || found->consumer != consumer || !found->spill_clobber_valid)
        return nullptr;
    return &*found;
}


std::optional<u32> FrozenGPRPlan::SpillClobberMask(const ir::Inst* consumer) const {
    const auto* operand = SpillConsumerFor(consumer);
    return operand ? std::optional<u32>{operand->spill_clobbers} : std::nullopt;
}
bool FrozenGPRPlan::MatchesFrozenIR() const {
    // IR is immutable after target planning; the allocation generation
    // pair is the whole staleness contract (no per-token IR signature).
    return MatchesAllocation();
}

void GPRPlanBuilder::AssignValueHome(ir::Inst* value, u16 home) {
    ASSERT(!finished);
    allocation.AssignGPRValueHome(value->Id(), home);
}

void GPRPlanBuilder::AssignTransferredValue(ir::Inst* value, u16 home,
                                               u32 begin, u32 end) {
    allocation.AssignGPRTransferredHome(value->Id(), home, begin, end);
}

void GPRPlanBuilder::AssignLowView(ir::Inst* value, u16 home) {
    ASSERT(!finished);
    allocation.AssignGPRLowView(value->Id(), home);
}

std::optional<u16> GPRPlanBuilder::ValueHome(
        ir::Value value, const ir::Inst* consumer) const {
    return value.Defined() && consumer
            ? allocation.GPRValueHome(value, consumer->Id()) : std::nullopt;
}

std::optional<u16> GPRPlanBuilder::LowViewHome(ir::Inst* value) const {
    return value ? allocation.GPRLowView(value->Id()) : std::nullopt;
}

bool GPRPlanBuilder::HasValueHome(ir::Inst* value) const {
    return value && allocation.HasGPRValueHome(value->Id());
}

bool GPRPlanBuilder::HasLowView(ir::Inst* value) const {
    return LowViewHome(value).has_value();
}

void GPRPlanBuilder::RegisterUseHome(ir::Inst* version,
                                        const ir::Inst* consumer,
                                        GuestStateMap::FixedHomeValue location) {
    allocation.RegisterGPRUseHome(version->Id(), consumer->Id(), location, true);
}

std::optional<GuestStateMap::FixedHomeValue> GPRPlanBuilder::FixedHomeForUse(
        ir::Value value, const ir::Inst* consumer) const {
    return value.Defined() && consumer
            ? allocation.GPRUseHome(value.Id(), consumer->Id()) : std::nullopt;
}

bool GPRPlanBuilder::ValueFullyResident(ir::Inst* definition) const {
    return definition && definition->GetUses(false) != 0 &&
           guest_state_map.ResidentUseCount(definition) +
                   allocation.AdditionalGPRResidentUses(definition->Id()) ==
           definition->GetUses(false);
}

GPRPlanBuilder::Location GPRPlanBuilder::UseLocation(
        ir::Value value, const ir::Inst* consumer, View view) const {
    ASSERT(!finished);
    return allocation.GPRLocationAt(value, consumer ? consumer->Id() : UINT32_MAX, view);
}

FrozenGPRPlan::Location FrozenGPRPlan::UseLocation(
        ir::Value value, const ir::Inst* consumer, View view) const {
    ASSERT(generation == allocation.GPRLocationGeneration() &&
           allocation_attempt == allocation.AllocationAttempt());
    return allocation.GPRLocationAt(value, consumer ? consumer->Id() : UINT32_MAX, view);
}

}  // namespace swift::runtime::backend::arm64
