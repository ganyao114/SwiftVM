#include <array>
#include "aarch64/assembler-aarch64.h"
#include "lower_predicates.h"
#include "runtime/backend/arm64/compile/effects.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/allocate/home_index.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/workspace.h"

namespace swift::runtime::backend::arm64 {

namespace {
bool IsImmediate(ir::Value value, u64 expected) {
    return value.Def() && value.Def()->GetOp() == ir::OpCode::LoadImm &&
           value.Def()->GetArg<ir::Imm>(0).Get() == expected;
}
bool IsAdjustedBoolean(ir::Value value, u32 depth = 0) {
    if (!value.Def() || depth == 4) return false;
    if (value.Def()->HasBooleanResult()) return true;
    switch (value.Def()->GetOp()) {
        case ir::OpCode::And:
        case ir::OpCode::Or: {
            u32 inputs = 0;
            bool boolean = true;
            value.Def()->ForEachValue([&](ir::Value input) {
                ++inputs;
                boolean &= IsAdjustedBoolean(input, depth + 1);
            });
            return inputs == 2 && boolean;
        }
        default:
            return false;
    }
}
}  // namespace

SelectActions LowerSelects(compile::BlockAnalysisIndex& analysis,
                           compile::CompileWorkspace& workspace,
                           const std::function<bool(const ir::Inst&)>& can_defer_condition) {
    SelectActions lowered;
    const auto instructions = analysis.Instructions();
    ASSERT(instructions.size() <= size_t{UINT16_MAX} + 1);
    std::optional<compile::LoweringCandidateScope> constant_uses;
    const auto cover_constant = [&](const ir::Inst* constant) {
        const auto position = analysis.InstructionPosition(constant);
        if (!position) return;
        // This Select already accounts for the sole use, including pseudos
        // and nonlocal references. Only shared constants need an accumulator.
        if (constant->GetUses(false) == 1)
            lowered.covered.push_back(constant->Id());
        else {
            if (!constant_uses) constant_uses.emplace(workspace);
            constant_uses->Entries().push_back({static_cast<u16>(*position), 0});
        }
    };
    for (size_t selected = 0; selected < instructions.size(); ++selected) {
        const auto* inst = instructions[selected];
        if (inst->GetOp() != ir::OpCode::Select) continue;
        const auto condition = inst->GetArg<ir::Value>(0);
        const bool boolean = inst->ReturnType() == ir::ValueType::U8 &&
                             IsAdjustedBoolean(condition) &&
                             IsImmediate(inst->GetArg<ir::Value>(1), 1) &&
                             IsImmediate(inst->GetArg<ir::Value>(2), 0);
        std::optional<ir::Cond> direct;
        auto* producer = condition.Def();
        if (producer && producer->GetOp() == ir::OpCode::CondSet && producer->GetUses(false) == 1 &&
            producer->Id() < inst->Id()) {
            const auto position = analysis.InstructionPosition(producer);
            bool safe = position && *position < selected;
            for (size_t next = position.value_or(selected) + 1; safe && next < selected; ++next) {
                // A producer can overwrite host NZCV before its flags pseudo
                // is reached, even when that publication follows the Select.
                if (analysis.PrepareHasPseudoUse(instructions[next]) ||
                    !can_defer_condition(*instructions[next])) {
                    safe = false;
                    break;
                }
            }
            if (safe) {
                direct = producer->GetArg<ir::Cond>(0);
                lowered.covered.push_back(producer->Id());
            }
        }
        if (!boolean && !direct) continue;
        ASSERT(lowered.actions.empty() || lowered.actions.back().instruction < inst->Id());
        lowered.actions.push_back(
                {inst->Id(), boolean, direct.value_or(static_cast<ir::Cond>(UINT8_MAX))});
        if (boolean) {
            cover_constant(inst->GetArg<ir::Value>(1).Def());
            cover_constant(inst->GetArg<ir::Value>(2).Def());
        }
    }
    if (constant_uses) {
        auto& entries = constant_uses->Entries();
        std::ranges::sort(entries, {}, &compile::LoweringCandidate::position);
        for (size_t first = 0; first < entries.size();) {
            size_t last = first + 1;
            while (last < entries.size() && entries[last].position == entries[first].position)
                ++last;
            const auto* constant = instructions[entries[first].position];
            // Count selected arm occurrences, not all operands of a Select:
            // a constant may also be its condition or have an uncovered user.
            if (constant->GetUses(false) == last - first) lowered.covered.push_back(constant->Id());
            first = last;
        }
    }
    return lowered;
}
}  // namespace swift::runtime::backend::arm64

namespace swift::runtime::backend::arm64 {

namespace {
struct EdgeCandidate {
    const ir::Inst* producer{};
    const ir::Inst* condition{};
    ir::Flags required{};
    ir::Cond raw_condition{};
    std::optional<u16> zero_target{};
    bool zero_is_64{};
    std::span<const compile::LoweringCandidate> discarded;
};
struct NarrowBranchCandidate {
    const ir::Inst* producer{};
    const ir::Inst* immediate_load{};
    u64 immediate{};
    ir::Flags required{};
    u8 width{};
};
struct ConditionRecipe {
    ir::Flags required{};
    ir::Cond raw_condition{};
    std::array<const ir::Inst*, 3> discarded{};
};

const ir::Inst* OperandDefinition(const ir::Operand& operand) {
    const auto value = operand.GetLeft();
    return value.IsValue() ? value.value.Def() : nullptr;
}

std::optional<ConditionRecipe> AnalyzeCondition(const ir::Inst* condition) {
    if (!condition || condition->GetUses() != 1) {
        return std::nullopt;
    }
    if (condition->GetOp() == ir::OpCode::LocalCondSet) {
        switch (condition->GetArg<ir::Cond>(0)) {
            case ir::Cond::EQ:
            case ir::Cond::NE:
                return ConditionRecipe{ir::Flags::Zero, condition->GetArg<ir::Cond>(0)};
            case ir::Cond::CS:
                return ConditionRecipe{ir::Flags::Carry, ir::Cond::CC};
            case ir::Cond::CC:
                return ConditionRecipe{ir::Flags::Carry, ir::Cond::CS};
            default:
                return std::nullopt;
        }
    }
    if (condition->GetOp() != ir::OpCode::And && condition->GetOp() != ir::OpCode::Or) {
        return std::nullopt;
    }

    auto* left = condition->GetArg<ir::Value>(0).Def();
    auto* right = OperandDefinition(condition->GetArg<ir::Operand>(1));
    auto is_zero_cond = [](const ir::Inst* inst, ir::Cond cond) {
        return inst && inst->GetOp() == ir::OpCode::CondSet && inst->GetUses() == 1 &&
               inst->GetArg<ir::Cond>(0) == cond;
    };
    const ir::Inst* predicate = left;
    const ir::Inst* zero_condition = right;
    if (is_zero_cond(left, ir::Cond::EQ) || is_zero_cond(left, ir::Cond::NE)) {
        predicate = right;
        zero_condition = left;
    }
    if (!predicate || predicate->GetUses() != 1 || !zero_condition ||
        zero_condition->GetUses() != 1) {
        return std::nullopt;
    }
    const bool high = condition->GetOp() == ir::OpCode::And &&
                      predicate->GetOp() == ir::OpCode::TestZero &&
                      is_zero_cond(zero_condition, ir::Cond::NE);
    const bool low_same = condition->GetOp() == ir::OpCode::Or &&
                          predicate->GetOp() == ir::OpCode::TestNotZero &&
                          is_zero_cond(zero_condition, ir::Cond::EQ);
    if (!high && !low_same) {
        return std::nullopt;
    }
    auto* carry_test = predicate->GetArg<ir::Value>(0).Def();
    if (!carry_test || carry_test->GetOp() != ir::OpCode::TestFlags || carry_test->GetUses() != 1 ||
        carry_test->GetArg<ir::Flags>(0) != ir::Flags::Carry) {
        return std::nullopt;
    }
    ConditionRecipe recipe{ir::Flags::Carry | ir::Flags::Zero, high ? ir::Cond::HI : ir::Cond::LS};
    recipe.discarded = {carry_test, predicate, zero_condition};
    return recipe;
}

bool IsPolarityStore(const ir::Inst& inst) {
    if (inst.GetOp() != ir::OpCode::StoreUniform) {
        return false;
    }
    const auto uniform = inst.GetArg<ir::Uniform>(0);
    const auto value = inst.GetArg<ir::Value>(1);
    return uniform.GetType() == ir::ValueType::U8 && value.Def() &&
           value.Def()->GetOp() == ir::OpCode::LoadImm &&
           value.Def()->GetArg<ir::Imm>(0).Get() <= 1;
}

bool IsFlagObserver(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::TestFlags:
        case ir::OpCode::TestNotFlags:
        case ir::OpCode::GetFlags:
        case ir::OpCode::Adc:
        case ir::OpCode::Sbb:
        case ir::OpCode::SetCarry:
        case ir::OpCode::SetOverflow:
        case ir::OpCode::PublishFCmpFlags:
        case ir::OpCode::PublishSse42StrFlags:
        case ir::OpCode::CondSelect:
        case ir::OpCode::CondSet:
        case ir::OpCode::LocalParitySet:
        case ir::OpCode::FCmpCondSet:
        case ir::OpCode::Goto:
        case ir::OpCode::NotGoto:
        case ir::OpCode::BindLabel:
            return true;
        default:
            return false;
    }
}

}  // namespace

namespace {
std::optional<EdgeCandidate> BuildEdgeCandidate(const ir::Block* block,
                                                const compile::BlockAnalysisIndex& analysis,
                                                compile::AllocationView allocation,
                                                const FeatureSet& features,
                                                const compile::GPRHomeIndex& homes,
                                                compile::LoweringCandidateScope& candidates) {
    const auto& terminal = block->GetTerminal();
    auto* branch = boost::get<ir::terminal::If>(&terminal);
    auto* condition = branch ? branch->cond.Def() : nullptr;
    auto condition_recipe = AnalyzeCondition(condition);
    if (!condition_recipe) {
        return std::nullopt;
    }

    const auto instructions = analysis.Instructions();
    ASSERT(instructions.size() <= size_t{UINT16_MAX} + 1);
    const auto position = analysis.InstructionPosition(condition);
    if (!position) {
        return std::nullopt;
    }
    const size_t condition_index = *position;
    auto& discarded = candidates.Entries();
    for (const auto* inst : condition_recipe->discarded) {
        if (!inst) continue;
        const auto at = analysis.InstructionPosition(inst);
        if (!at) return std::nullopt;
        discarded.push_back({static_cast<u16>(*at), 0});
    }

    size_t producer_end = condition_index;
    while (producer_end > 0 && instructions[producer_end - 1]->GetOp() != ir::OpCode::AdvancePC) {
        --producer_end;
    }
    if (producer_end == 0) {
        return std::nullopt;
    }
    const size_t producer_advance = producer_end - 1;
    size_t producer_begin = producer_advance;
    while (producer_begin > 0 &&
           instructions[producer_begin - 1]->GetOp() != ir::OpCode::AdvancePC) {
        --producer_begin;
    }

    EdgeCandidate recipe{
            .condition = condition,
            .required = condition_recipe->required,
            .raw_condition = condition_recipe->raw_condition,
    };
    u32 inverts = 0;
    u32 polarity_stores = 0;
    const ir::Inst* polarity_constant{};
    ir::Value zero_value{};
    auto resolve_bitcast = [](ir::Value value) {
        while (value.Def() && value.Def()->IsBitCastOperation()) {
            value = value.Def()->GetArg<ir::Value>(0);
        }
        return value;
    };
    for (size_t i = producer_begin; i < condition_index; ++i) {
        auto* inst = instructions[i];
        // Other insertions name the current roster node, so they cannot
        // recur later in this scan. The polarity constant can be a forward
        // reference in manually constructed IR and retains that exclusion.
        if (std::ranges::find(condition_recipe->discarded, inst) !=
                    condition_recipe->discarded.end() ||
            inst == polarity_constant) {
            continue;
        }
        switch (inst->GetOp()) {
            case ir::OpCode::SaveFlags: {
                discarded.push_back({static_cast<u16>(i), 0});
                auto value = inst->GetArg<ir::Value>(0);
                if (!value.Def() || (recipe.producer && recipe.producer != value.Def())) {
                    return std::nullopt;
                }
                if (value.Def()->GetOp() != ir::OpCode::Sub) {
                    return std::nullopt;
                }
                recipe.producer = value.Def();
                break;
            }
            case ir::OpCode::BranchOnlyFlags: {
                discarded.push_back({static_cast<u16>(i), 0});
                const auto value = inst->GetArg<ir::Value>(0);
                if (recipe.producer || !value.Def() || value.Def()->GetOp() != ir::OpCode::And ||
                    recipe.required != ir::Flags::Zero ||
                    !True(inst->GetArg<ir::Flags>(1) & recipe.required)) {
                    return std::nullopt;
                }
                const auto left = value.Def()->GetArg<ir::Value>(0);
                const auto right = value.Def()->GetArg<ir::Operand>(1);
                if (!right.GetRight().Null() || !right.GetLeft().IsValue() ||
                    resolve_bitcast(left).Def() != resolve_bitcast(right.GetLeft().value).Def()) {
                    return std::nullopt;
                }
                recipe.producer = value.Def();
                zero_value = resolve_bitcast(left);
                break;
            }
            case ir::OpCode::ClearFlags:
                discarded.push_back({static_cast<u16>(i), 0});
                break;
            case ir::OpCode::InvertCarry:
                ++inverts;
                discarded.push_back({static_cast<u16>(i), 0});
                break;
            case ir::OpCode::StoreUniform:
                if (IsPolarityStore(*inst)) {
                    if (++polarity_stores > 1) return std::nullopt;
                    discarded.push_back({static_cast<u16>(i), 0});
                    auto* value = inst->GetArg<ir::Value>(1).Def();
                    if (value && value->GetUses(false) == 1) {
                        const auto at = analysis.InstructionPosition(value);
                        if (!at) return std::nullopt;
                        discarded.push_back({static_cast<u16>(*at), 0});
                        polarity_constant = value;
                    }
                }
                break;
            default:
                if (IsFlagObserver(inst->GetOp())) {
                    return std::nullopt;
                }
                break;
        }
    }
    const bool valid_inverts =
            zero_value.Defined()
                    ? inverts == 0
                    : (recipe.required == ir::Flags::Zero ? inverts <= 1 : inverts == 1);
    if (!recipe.producer || !valid_inverts || polarity_stores > 1 ||
        recipe.producer->Id() >= condition->Id()) {
        return std::nullopt;
    }

    const ir::Inst* zero_publication{};
    if (zero_value.Defined()) {
        const u32 width = ir::GetValueSizeByte(recipe.producer->ReturnType());
        // CBZ.W tests all 32 bits; partial 8/16-bit homes need an explicit
        // extension certificate before they can replace the normalized test.
        if (width != sizeof(u32) && width != sizeof(u64)) return std::nullopt;
        const ir::Inst* publication{};
        u16 target{};
        for (auto* inst : instructions) {
            if (inst->Id() >= recipe.producer->Id() || inst->GetOp() != ir::OpCode::SetHostGPR ||
                inst->GetArg<ir::Imm>(2).Get() != 0 ||
                ir::GetValueSizeByte(inst->GetArg<ir::Value>(0).Type()) != width ||
                resolve_bitcast(inst->GetArg<ir::Value>(0)).Def() != zero_value.Def()) {
                continue;
            }
            const u32 candidate = inst->GetArg<ir::Imm>(1).Get();
            if (candidate > 9 && (candidate < 19 || candidate > 23) && candidate != 29) {
                continue;
            }
            publication = inst;
            target = static_cast<u16>(candidate);
        }
        // Both execution points must already hold the whole value in this
        // physical home. A reload or scratch materialization cannot certify CBZ.
        const auto at_producer = allocation.GPRLocationAt(
                zero_value, recipe.producer->Id(), compile::AllocationView::GPRView::WholeValue);
        const auto at_condition = allocation.GPRLocationAt(
                zero_value, condition->Id(), compile::AllocationView::GPRView::WholeValue);
        if (!publication || at_producer.kind != compile::AllocationView::GPR ||
            at_condition.kind != compile::AllocationView::GPR || at_producer.slot != target ||
            at_condition.slot != target ||
            homes.Conflicts(target,
                            publication->Id(),
                            instructions.back()->Id(),
                            zero_value.Id(),
                            recipe.producer->Id())) {
            return std::nullopt;
        }
        zero_publication = publication;
        recipe.zero_target = target;
        recipe.zero_is_64 = width == sizeof(u64);
    }

    std::ranges::sort(discarded, {}, &compile::LoweringCandidate::position);
    discarded.erase(
            std::unique(discarded.begin(),
                        discarded.end(),
                        [](auto left, auto right) { return left.position == right.position; }),
            discarded.end());
    recipe.discarded = discarded;

    // One traversal retains two distinct proof windows: the physical CBZ
    // home from publication to terminal, and host NZCV after its producer.
    bool after_producer = false;
    auto covered = discarded.begin();
    for (size_t i = 0; i < instructions.size(); ++i) {
        auto* inst = instructions[i];
        const bool omitted = covered != discarded.end() && covered->position == i;
        if (omitted) ++covered;
        const bool producer = inst == recipe.producer;
        const bool needs_nzcv = after_producer && inst != condition && !omitted;
        after_producer |= producer;
        const auto home = zero_publication && inst->Id() > zero_publication->Id() && !omitted
                                  ? recipe.zero_target
                                  : std::nullopt;
        if (!home && !needs_nzcv) continue;
        const auto effects = AnalyzeTargetWindowEffects(*inst, features, needs_nzcv, home);
        if (needs_nzcv && (!effects.retains_nzcv || effects.observes_guest)) return std::nullopt;
        if (!home) continue;
        // Only an imported discarded node bypasses TickIR/preservation. The
        // selected producer and condition still execute those transport actions.
        if ((inst->GetOp() == ir::OpCode::SetHostGPR && inst->GetArg<ir::Imm>(1).Get() == *home) ||
            !allocation.DirtyGPR(inst->Id()).Get(*home) || effects.clobbers_home)
            return std::nullopt;
        if (!producer && inst != condition && inst->HasValue() && !inst->IsBitCastOperation() &&
            !ir::IsFloatValueType(inst->ReturnType())) {
            const auto written = allocation.GPRLocationAt(
                    ir::Value{inst}, inst->Id(), compile::AllocationView::GPRView::WholeValue);
            if (written.kind == compile::AllocationView::GPR && written.slot == *home)
                return std::nullopt;
        }
        for (const auto& preserve : allocation.GPRPreservationsBefore(inst->Id()))
            if ((preserve.destination_kind == compile::AllocationView::GPR &&
                 preserve.destination == *home) ||
                preserve.scratch == *home)
                return std::nullopt;
    }

    return recipe;
}

std::optional<NarrowBranchCandidate> BuildNarrowBranchCandidate(const EdgeCandidate& edge) {
    auto* inst = edge.producer;
    if (!inst || inst->GetOp() != ir::OpCode::Sub || inst->GetUses() != 0) {
        return std::nullopt;
    }
    const auto required = edge.required;
    if (required != ir::Flags::Zero && required != ir::Flags::Carry &&
        required != (ir::Flags::Carry | ir::Flags::Zero)) {
        return std::nullopt;
    }
    const u32 width = ir::GetValueSizeByte(inst->ReturnType());
    if (width != sizeof(u8) && width != sizeof(u16)) {
        return std::nullopt;
    }

    const auto right = inst->GetArg<ir::Operand>(1);
    if (!right.GetRight().Null() || !right.GetLeft().IsValue() ||
        right.GetOp() != ir::OperandOp::Plus) {
        return std::nullopt;
    }
    auto* load = right.GetLeft().value.Def();
    if (!load || load->GetOp() != ir::OpCode::LoadImm || load->GetUses(false) != 1) {
        return std::nullopt;
    }
    const u64 immediate = load->GetArg<ir::Imm>(0).Get();
    if (!vixl::aarch64::Assembler::IsImmAddSub(immediate)) {
        return std::nullopt;
    }
    return NarrowBranchCandidate{
            .producer = inst,
            .immediate_load = load,
            .immediate = immediate,
            .required = required,
            .width = static_cast<u8>(width),
    };
}

}  // namespace
std::optional<IntegerBranchActions> LowerIntegerBranch(const ir::Block* block,
                                                       const compile::BlockAnalysisIndex& analysis,
                                                       compile::CompileWorkspace& workspace,
                                                       compile::AllocationView allocation,
                                                       const FeatureSet& features,
                                                       const compile::GPRHomeIndex& homes) {
    if (!homes.Ready() || !homes.Locations().RefersToSameResult(allocation)) return std::nullopt;
    if (!block->HasDeadEdgeIntegerBranchProof()) return std::nullopt;
    compile::LoweringCandidateScope candidates{workspace};
    auto edge = BuildEdgeCandidate(block, analysis, allocation, features, homes, candidates);
    if (!edge) return std::nullopt;
    auto narrow = BuildNarrowBranchCandidate(*edge);
    // A numeric link may only name a definition in this exact numbered block.
    if (!analysis.Contains(edge->producer) || !analysis.Contains(edge->condition) ||
        (narrow && !analysis.Contains(narrow->immediate_load)))
        return std::nullopt;
    ASSERT(static_cast<u64>(edge->required) <= UINT8_MAX);
    IntegerBranchActions lowered{
            {0,
             edge->producer->Id(),
             edge->condition->Id(),
             edge->raw_condition,
             static_cast<u8>(edge->required),
             static_cast<u8>(edge->zero_target.value_or(UINT8_MAX)),
             static_cast<u8>(edge->zero_target ? (edge->zero_is_64 ? 8 : 4) : 0)},
            {}};
    const auto instructions = analysis.Instructions();
    for (const auto entry : edge->discarded)
        lowered.covered.push_back(instructions[entry.position]->Id());
    if (narrow) {
        ASSERT(narrow->immediate <= UINT32_MAX);
        lowered.action.immediate = static_cast<u32>(narrow->immediate);
        lowered.action.width = narrow->width;
        lowered.covered.push_back(narrow->immediate_load->Id());
    }
    std::sort(lowered.covered.begin(), lowered.covered.end());
    lowered.covered.erase(std::unique(lowered.covered.begin(), lowered.covered.end()),
                          lowered.covered.end());
    return lowered;
}
}  // namespace swift::runtime::backend::arm64
