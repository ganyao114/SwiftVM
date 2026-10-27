#include "inputs.h"
#include "lower_scalar.h"
#include "scratch.h"

#include <algorithm>
#include <optional>
#include "runtime/compile/workspace.h"

#include "runtime/common/svm_config.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/analysis/facts.h"

namespace swift::runtime::backend::arm64 {

namespace {
u32 SpillScratchOnlyGPRs(const GPRSMask& pool, const ir::Inst& inst, const FeatureSet& features) {
    u32 count = backend::X86PinExtLevel3AluScratchEnabled(pool, inst.GetOp()) ? 1u : 0u;
    if (!backend::X86PinExtScratchOnlyEnabled(pool, features)) return count;
    // Spill regions historically use the instruction-specific fixed mask;
    // ordinary instruction scratch uses the opcode-only contract above it.
    const u32 fixed = backend::FixedGPRClobbers(inst, features, true);
    return count + ((fixed & (1u << 12)) || GetSvmConfig().flags_regs ? 0u : 1u) +
           ((fixed & (1u << 13)) ? 0u : 1u);
}
}  // namespace
compile::ScratchRequirements DescribeInstructionScratch(const GPRSMask& pool,
                                                        const FeatureSet& features,
                                                        const ir::Inst& inst,
                                                        FlagsDemand flags) {
    const auto need = backend::ScratchBudget(inst, features, flags);
    if (backend::FixedGPRClassEnabled(pool, features)) {
        const auto contract = backend::ClassifyGPRContract(pool, inst, features, flags);
        ASSERT(contract.hot_instruction_scratch == need.gpr);
        ASSERT(inst.GetOp() == ir::OpCode::XchgBarrier ||
               (contract.fixed_clobber_mask & backend::kX86FixedGPRHomes) == 0);
    }
    const auto outside = backend::ScratchOnlyGPRCount(pool, inst.GetOp(), features);
    const bool overlap = !backend::ScratchPreciseRequested(features) && outside &&
                         (inst.GetOp() == ir::OpCode::Add || inst.GetOp() == ir::OpCode::Sub);
    return {need.gpr,
            need.fpr,
            outside,
            backend::kDefaultScratchGPR,
            overlap,
            SpillScratchOnlyGPRs(pool, inst, features)};
}

void DescribeScratchInput(compile::AllocationInputs& input,
                          const ir::Inst& inst,
                          compile::FlagsDemand flags) {
    const auto first = DescribeInstructionScratch(input.pools[0].pool, input.features, inst, flags);
    const auto second =
            input.pools[0].pool == input.pools[1].pool
                    ? first
                    : DescribeInstructionScratch(input.pools[1].pool, input.features, inst, flags);
    ASSERT(first.gpr == second.gpr && first.fpr == second.fpr);
    const std::array requirements{first, second};
    compile::AllocationInputs::ScratchProfile profile;
    profile.need = {static_cast<u8>(first.gpr), static_cast<u8>(first.fpr)};
    profile.reload_headroom = static_cast<u8>(first.reload_headroom);
    for (u32 pool = 0; pool < requirements.size(); ++pool) {
        const auto& need = requirements[pool];
        ASSERT(need.gpr < 32 && need.fpr < 32 && need.outside_value_pool < 32);
        profile.outside_value_pool[pool] = static_cast<u8>(need.outside_value_pool);
        ASSERT(need.spill_outside_value_pool < 32);
        profile.spill_outside_value_pool[pool] = static_cast<u8>(need.spill_outside_value_pool);
        if (need.overlap_gpr_reloads) profile.overlap_pools |= 1u << pool;
        auto& maximum = input.pools[pool].max_scratch;
        maximum.gpr = std::max<u32>(
                maximum.gpr,
                need.gpr > need.outside_value_pool ? need.gpr - need.outside_value_pool : 0);
        maximum.fpr = std::max<u32>(maximum.fpr, need.fpr);
    }
    input.scratch.push_back({inst.Id(), profile});
    input.reload_bound = std::max<u32>(input.reload_bound, inst.GetValues().size() + 1);
}

static_assert(sizeof(InstructionScratchFacts) == 16);
static_assert(sizeof(ScratchRange) == 20);
static_assert(sizeof(SpillInputAction) == 16);

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

static InstructionScratchFacts BuildInstructionScratch(const ir::Inst& inst,
                                                       const GPRSMask& pool,
                                                       const FeatureSet& features,
                                                       const UnitScratchFacts& unit,
                                                       bool has_value_uses,
                                                       FlagsDemand demand) {
    InstructionScratchFacts result;
    result.fixed_clobbers = FixedGPRClobbers(inst, features, unit.scratch_only);
    // VIXL accounting has always excluded the default fixed-clobber mask,
    // which differs from the explicit level-2 scratch-only reservation mask.
    // Without XPOOL the default mask is zero except for XCHG's published
    // home, which is already included in the explicit scratch-only mask.
    result.audit_clobbers = !unit.scratch_only || inst.GetOp() == ir::OpCode::XchgBarrier
                                    ? result.fixed_clobbers
                                    : 0;
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
        result.unobserved_mul_right =
                right.GetRight().Null() && input.IsValue() && input.value.Defined() &&
                input.value.Def()->GetUses(false) == input.value.Def()->GetUses();
    }
    return result;
}

InstructionScratchFacts PrepareInstructionScratch(const ir::Inst& inst,
                                                  const GPRSMask& pool,
                                                  const FeatureSet& features) {
    return PrepareInstructionScratchWithBlock(inst, pool, features, nullptr);
}

InstructionScratchFacts PrepareInstructionScratchWithBlock(const ir::Inst& inst,
                                                           const GPRSMask& pool,
                                                           const FeatureSet& features,
                                                           const ir::Block* block) {
    auto result = BuildInstructionScratch(inst,
                                          pool,
                                          features,
                                          PrepareUnitScratch(pool, features),
                                          inst.GetUses() != 0,
                                          block ? ScanFlagsDemand(*block, inst) : FlagsDemand{});
    if (inst.GetOp() == ir::OpCode::Div128) result.div128_native = PrepareManualDiv128(inst, block);
    return result;
}

ScratchPreparationInputs CaptureScratchPreparationInputs(const GPRSMask& pool,
                                                         const FeatureSet& features) {
    const auto& config = GetSvmConfig();
    return {pool,
            config.x86_pin_ext,
            config.exec_prof,
            config.static_regs,
            config.flags_regs,
            features.jit_scratch_xpool,
            features.scratch_precise,
            features.fpr_scratch_precise};
}

SpillInputFacts PrepareSpillInput(const ir::Inst* consumer, u32 value_id) {
    SpillInputFacts result;
    u32 direct_uses = 0;
    if (consumer)
        consumer->ForEachValue([&](ir::Value value) {
            if (value.Defined() && value.Id() == value_id) {
                result.definition = value.Def();
                ++direct_uses;
            }
        });
    result.last_use = result.definition && result.definition->GetUses(false) == direct_uses;
    return result;
}

const InstructionScratchFacts* InstructionScratchView::For(const ir::Inst* consumer,
                                                           size_t& next) const {
    if (!consumer || consumer->GetBlock() != block) return nullptr;
    const auto id = consumer->Id();
    const auto contains = [id](const ScratchRange& row) {
        return row.first <= id && id <= row.last;
    };
    if (next < ranges.size()) {
        if (contains(ranges[next])) return &ranges[next].facts;
        if (id > ranges[next].last && next + 1 < ranges.size() && contains(ranges[next + 1]))
            return &ranges[++next].facts;
    }
    const auto found =
            std::lower_bound(ranges.begin(), ranges.end(), id, [](const ScratchRange& row, u32 id) {
                return row.last < id;
            });
    if (found == ranges.end() || !contains(*found)) return nullptr;
    next = static_cast<size_t>(found - ranges.begin());
    return &found->facts;
}

namespace detail {
ScratchActions LowerScratch(const ir::Block* block,
                            compile::AllocationView allocation,
                            const FeatureSet& features,
                            const UnitScratchFacts& unit,
                            const compile::BlockAnalysisIndex& analysis,
                            compile::CompileWorkspace& workspace) {
    ASSERT(block && unit.prepared);
    ScratchActions result;
    result.unit = unit;
    const auto nodes = analysis.Instructions();
    std::optional<compile::InstructionRowScope> occurrences;
    size_t position{};
    for (const auto& instruction : block->GetInstList()) {
        const auto* inst = &instruction;
        ASSERT(position < nodes.size() && nodes[position] == inst);
        ++position;
        compile::FlagsDemand demand;
        // Only local SaveFlags/BranchOnlyFlags contribute. None still has
        // presence/count, and external pseudo uses remain outside this block.
        if (inst->GetUses(false) != inst->GetUses()) {
            ASSERT(analysis.UsesReady());
            analysis.ForEachPseudoUse(inst, [&](const ir::Inst& pseudo) {
                const auto op = pseudo.GetOp();
                if ((op != ir::OpCode::SaveFlags && op != ir::OpCode::BranchOnlyFlags) ||
                    !pseudo.ArgAt(0).IsValue() || pseudo.GetArg<ir::Value>(0).Def() != inst)
                    return;
                demand.requested |= pseudo.GetArg<ir::Flags>(1);
                ++demand.pseudo_count;
                demand.branch_only |= op == ir::OpCode::BranchOnlyFlags;
                demand.has_save |= op == ir::OpCode::SaveFlags;
            });
        }
        auto facts = BuildInstructionScratch(
                *inst, allocation.GetGprs(), features, unit, inst->GetUses() != 0, demand);
        if (inst->GetOp() == ir::OpCode::Div128)
            facts.div128_native = SelectNativeDiv128(*inst, analysis);
        if (!result.ranges.empty() && u32{result.ranges.back().last} + 1 == inst->Id() &&
            result.ranges.back().facts == facts)
            result.ranges.back().last = inst->Id();
        else {
            // A small reduction in rows must not round temporary capacity up
            // past the old one-row-per-instruction bound.
            if (result.ranges.size() == result.ranges.capacity()) {
                const auto next =
                        result.ranges.capacity() ? result.ranges.capacity() * 2 : size_t{4};
                result.ranges.reserve(std::min(nodes.size(), next));
            }
            result.ranges.push_back({facts, inst->Id(), inst->Id()});
        }

        const auto first_input = result.inputs.size();
        inst->ForEachValue([&](ir::Value value) {
            if (!value.Defined() || value.Id() >= allocation.MapCount() ||
                allocation.ValueType(value) != compile::Allocation::MEM)
                return;
            const auto producer = analysis.InstructionPosition(value.Def());
            if (!producer) {
                // Same numeric ID cannot let a foreign Def inherit a local
                // certificate. Sealing still rejects the entire conflicting key.
                const auto* local = analysis.FindInstruction(value.Id());
                if (local && local->HasValue())
                    result.inputs.push_back({nullptr, inst->Id(), value.Id(), false});
                return;
            }
            if (!occurrences) occurrences.emplace(workspace, nodes.size());
            auto& count = (*occurrences)[*producer];
            if (count == UINT32_MAX) {
                count = 0;
                result.inputs.push_back({value.Def(), inst->Id(), value.Id(), false});
            }
            ++count;
        });
        for (auto& input : std::span{result.inputs}.subspan(first_input)) {
            if (!input.definition) continue;
            const auto producer = analysis.InstructionPosition(input.definition);
            ASSERT(producer && occurrences);
            auto& count = (*occurrences)[*producer];
            // GetUses(false) is the total occurrence count, including pseudos
            // and external uses. Equality proves this consumer owns every use.
            input.last_use = count == input.definition->GetUses(false);
            count = UINT32_MAX;
        }
    }
    ASSERT(position == nodes.size());
    return result;
}
}  // namespace detail
}  // namespace swift::runtime::backend::arm64
