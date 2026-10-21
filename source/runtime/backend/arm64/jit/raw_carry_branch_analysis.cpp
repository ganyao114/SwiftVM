#include "raw_carry_branch_analysis.h"

#include <algorithm>
#include <iterator>
#include "runtime/backend/arm64/alloc/consumer_plan.h"
#include "runtime/backend/arm64/alloc/target_analysis_traits.h"
#include "block_analysis_index.h"

namespace swift::runtime::backend::arm64 {

namespace {

ir::Inst* SoleUser(ir::Block* block, ir::Inst* definition, const BlockAnalysisIndex* analysis) {
    if (!definition || definition->GetUses(false) != 1) {
        return nullptr;
    }
    if (analysis) {
        const auto uses = analysis->Uses(definition);
        return uses.size() == 1 && uses.front().count == 1 ? uses.front().consumer : nullptr;
    }
    ir::Inst* result{};
    for (auto& inst : block->GetInstList()) {
        for (ir::Value value : inst.GetValues()) {
            if (value.Def() != definition) {
                continue;
            }
            if (result) {
                return nullptr;
            }
            result = &inst;
        }
    }
    return result;
}

ir::Inst* OtherOperand(ir::Inst* combine, ir::Inst* known) {
    if (!combine || (combine->GetOp() != ir::OpCode::And &&
                     combine->GetOp() != ir::OpCode::Or)) {
        return nullptr;
    }
    if (combine->GetArg<ir::Value>(0).Def() == known) {
        const auto right = combine->GetArg<ir::Operand>(1).GetLeft();
        return right.IsValue() ? right.value.Def() : nullptr;
    }
    return combine->GetArg<ir::Value>(0).Def();
}

}  // namespace

void RawCarryBranchAnalysis::Analyze(ir::Block* block, const FeatureSet& features,
                                    const ConsumerPlan& consumers,
                                    const BlockAnalysisIndex* analysis) {
    folds.clear();
    test_inverts.clear();
    test_conditions.clear();
    if (!consumers.valid) return;
    auto& instructions = block->GetInstList();
    for (auto test = instructions.begin(); test != instructions.end(); ++test) {
        if (test->GetOp() != ir::OpCode::TestFlags ||
            test->GetArg<ir::Flags>(0) != ir::Flags::Carry || test->HasPseudoOperations()) {
            continue;
        }
        auto* predicate = SoleUser(block, &*test, analysis);
        auto* combine = SoleUser(block, predicate, analysis);
        auto* other = OtherOperand(combine, predicate);
        auto* user = SoleUser(block, combine, analysis);
        if (!predicate || !combine || !other || other->GetOp() != ir::OpCode::CondSet ||
            SoleUser(block, other, analysis) != combine ||
            !consumers.local_conditions.contains(combine) ||
            predicate->Id() <= test->Id() || other->Id() <= test->Id() ||
            predicate->Id() >= combine->Id() || other->Id() >= combine->Id()) continue;
        const auto operand = combine->GetArg<ir::Operand>(1);
        if (operand.GetOp() != ir::OperandPlus || !operand.GetRight().Null()) continue;
        const ir::Cond zero_condition = other->GetArg<ir::Cond>(0);
        const bool above = predicate->GetOp() == ir::OpCode::TestZero &&
                combine->GetOp() == ir::OpCode::And && zero_condition == ir::Cond::NE;
        const bool below_equal = predicate->GetOp() == ir::OpCode::TestNotZero &&
                combine->GetOp() == ir::OpCode::Or && zero_condition == ir::Cond::EQ;
        if (!above && !below_equal) continue;
        if (user && user->Id() <= combine->Id()) continue;
        const auto end = user ? user->Id() : UINT32_MAX;
        const auto keeps_carrier = [&](const ir::Inst& inst) {
            if (inst.HasPseudoOperations()) return false;
            // These three nodes execute only TickIR/preservation transport
            // after this action records the logical user's local condition.
            if (&inst == predicate || &inst == other || &inst == combine) return true;
            const auto effects = AnalyzeTargetWindowEffects(inst, features, true, std::nullopt);
            return effects.retains_nzcv && !effects.observes_guest;
        };
        bool keeps = true;
        if (analysis) {
            for (auto* inst : analysis->Between(test->Id(), end))
                if (!keeps_carrier(*inst)) { keeps = false; break; }
        } else for (auto scan = std::next(test); scan != instructions.end() && &*scan != user; ++scan)
            if (!keeps_carrier(*scan)) { keeps = false; break; }
        if (!keeps) continue;
        folds.emplace(&*test, CarryConditionAction{predicate, combine, other,
                above ? ir::Cond::EQ : ir::Cond::NE});
        auto advance = test;
        if (advance == instructions.begin() ||
            (--advance)->GetOp() != ir::OpCode::AdvancePC) {
            continue;
        }
        auto invert = advance;
        if (invert == instructions.begin() ||
            (--invert)->GetOp() != ir::OpCode::InvertCarry) {
            continue;
        }
        auto save = invert;
        if (save == instructions.begin() ||
            (--save)->GetOp() != ir::OpCode::SaveFlags ||
            !True(save->GetArg<ir::Flags>(1) & ir::Flags::Carry)) {
            continue;
        }
        auto* producer = save->GetArg<ir::Value>(0).Def();
        if (!producer || (producer->GetOp() != ir::OpCode::Sub &&
                          producer->GetOp() != ir::OpCode::Sbb)) {
            continue;
        }
        // SaveFlags is a pseudo attached to the producer, but a later helper
        // or flags producer can consume/overwrite its pending host NZCV before
        // the publication node is reached. Freeze only a preserved local window.
        bool preserved = true;
        const auto keeps_producer = [&](const ir::Inst& inst) {
            const auto effects = AnalyzeTargetWindowEffects(inst, features, true, std::nullopt);
            return !inst.HasPseudoOperations() && effects.retains_nzcv && !effects.observes_guest;
        };
        if (analysis) {
            if (!analysis->Contains(producer) || producer->Id() >= save->Id()) continue;
            for (auto* inst : analysis->Between(producer->Id(), save->Id()))
                if (!keeps_producer(*inst)) { preserved = false; break; }
        } else {
            const auto producer_it = std::find_if(instructions.begin(), save,
                    [&](const ir::Inst& inst) { return &inst == producer; });
            if (producer_it == save) continue;
            for (auto scan = std::next(producer_it); scan != save; ++scan)
                if (!keeps_producer(*scan)) { preserved = false; break; }
        }
        if (!preserved) continue;
        test_inverts.emplace(&*test, &*invert);
        test_conditions.emplace(&*test, above ? ir::Cond::HI : ir::Cond::LS);
    }
}

const CarryConditionAction* RawCarryBranchAnalysis::FoldForTest(const ir::Inst* inst) const {
    const auto found = folds.find(inst);
    return found == folds.end() ? nullptr : &found->second;
}

bool RawCarryBranchAnalysis::SuppressesInvert(const ir::Inst* inst) const {
    return std::any_of(test_inverts.begin(), test_inverts.end(),
                       [&](const auto& item) { return item.second == inst; });
}

std::optional<ir::Cond> RawCarryBranchAnalysis::ConditionForTest(
        const ir::Inst* inst) const {
    const auto condition = test_conditions.find(inst);
    return condition == test_conditions.end()
            ? std::nullopt
            : std::optional<ir::Cond>{condition->second};
}

ir::Inst* RawCarryBranchAnalysis::InvertForTest(const ir::Inst* inst) const {
    const auto invert = test_inverts.find(inst);
    return invert == test_inverts.end() ? nullptr : invert->second;
}

}  // namespace swift::runtime::backend::arm64
