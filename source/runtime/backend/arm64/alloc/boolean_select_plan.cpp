#include "boolean_select_plan.h"
#include <unordered_map>
#include "runtime/backend/arm64/jit/block_analysis_index.h"

namespace swift::runtime::backend::arm64 {
namespace {
bool IsImmediate(ir::Value value, u64 expected) {
    return value.Def() && value.Def()->GetOp() == ir::OpCode::LoadImm &&
           value.Def()->GetArg<ir::Imm>(0).Get() == expected;
}
bool IsAdjustedBoolean(ir::Value value, u32 depth = 0) {
    if (!value.Def() || depth == 4) return false;
    switch (value.Def()->GetOp()) {
        case ir::OpCode::LoadImm:
            return value.Def()->GetArg<ir::Imm>(0).Get() <= 1;
        case ir::OpCode::TestZero:
        case ir::OpCode::TestNotZero:
        case ir::OpCode::TestFlags:
        case ir::OpCode::TestNotFlags:
        case ir::OpCode::CondSet:
        case ir::OpCode::LocalCondSet:
        case ir::OpCode::LocalParitySet:
        case ir::OpCode::FCmpCondSet:
            return true;
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
        default: return false;
    }
}
}

BooleanSelectPlan BuildBooleanSelectPlan(ir::Block* block, const BlockAnalysisIndex& analysis,
        const std::function<bool(const ir::Inst&)>& can_defer_condition) {
    BooleanSelectPlan plan;
    const auto& instructions = block->GetInstList();
    std::unordered_map<ir::Inst*, u32> constant_uses;
    const auto cover_constant = [&](ir::Inst* constant) {
        if (!analysis.Contains(constant)) return;
        // This Select already accounts for the sole use, including pseudos
        // and nonlocal references. Only shared constants need an accumulator.
        if (constant->GetUses(false) == 1) plan.covered.push_back(constant);
        else ++constant_uses[constant];
    };
    for (auto* inst : analysis.Instructions()) {
        if (inst->GetOp() != ir::OpCode::Select) continue;
        const auto condition = inst->GetArg<ir::Value>(0);
        const bool boolean = inst->ReturnType() == ir::ValueType::U8 &&
                IsAdjustedBoolean(condition) && IsImmediate(inst->GetArg<ir::Value>(1), 1) &&
                IsImmediate(inst->GetArg<ir::Value>(2), 0);
        std::optional<ir::Cond> direct;
        auto* producer = condition.Def();
        if (producer && producer->GetOp() == ir::OpCode::CondSet &&
            producer->GetUses(false) == 1 && producer->Id() < inst->Id() && analysis.Contains(producer)) {
            bool safe = true;
            auto next = std::next(instructions.iterator_to(*producer));
            const auto selected = instructions.iterator_to(*inst);
            for (; next != selected && next != instructions.end(); ++next) {
                // A producer can overwrite host NZCV before its flags pseudo
                // is reached, even when that publication follows the Select.
                if (analysis.HasPseudoUse(&*next) || !can_defer_condition(*next)) { safe = false; break; }
            }
            safe &= next == selected;
            if (safe) {
                direct = producer->GetArg<ir::Cond>(0);
                plan.covered.push_back(producer);
            }
        }
        if (!boolean && !direct) continue;
        ASSERT(plan.actions.empty() || plan.actions.back().first->Id() < inst->Id());
        plan.actions.emplace_back(inst, BooleanSelectAction{boolean, direct});
        if (boolean) {
            cover_constant(inst->GetArg<ir::Value>(1).Def());
            cover_constant(inst->GetArg<ir::Value>(2).Def());
        }
    }
    for (const auto& [constant, uses] : constant_uses)
        if (constant->GetUses(false) == uses) plan.covered.push_back(constant);
    return plan;
}
}
