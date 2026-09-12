#include "scalar_copy_analysis.h"

#include <algorithm>

namespace swift::runtime::backend::arm64 {

namespace {

bool Equivalent(ir::Value left, ir::Value right, u32 depth = 0) {
    if (left.Type() != right.Type() || depth == 8) {
        return false;
    }
    if (left == right) {
        return true;
    }
    auto* left_def = left.Def();
    auto* right_def = right.Def();
    if (!left_def || !right_def || left_def->GetOp() != right_def->GetOp() ||
        left_def->ReturnType() != right_def->ReturnType()) {
        return false;
    }
    if (left_def->GetOp() == ir::OpCode::BitCast) {
        return Equivalent(
                left_def->GetArg<ir::Value>(0), right_def->GetArg<ir::Value>(0), depth + 1);
    }
    if (left_def->GetOp() != ir::OpCode::BitExtract ||
        left_def->GetArg<ir::Imm>(1).Get() != right_def->GetArg<ir::Imm>(1).Get() ||
        left_def->GetArg<ir::Imm>(2).Get() != right_def->GetArg<ir::Imm>(2).Get()) {
        return false;
    }
    return Equivalent(left_def->GetArg<ir::Value>(0), right_def->GetArg<ir::Value>(0), depth + 1);
}

u32 References(ir::Inst& user, const ir::Inst* value) {
    return std::ranges::count_if(user.GetValues(),
                                 [&](ir::Value input) { return input.Def() == value; });
}

}  // namespace

void ScalarCopyAnalysis::Analyze(ir::Block* block) {
    self_xors.clear();
    discarded_inputs.clear();
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() != ir::OpCode::Xor) {
            continue;
        }
        const auto left = inst.GetArg<ir::Value>(0);
        const auto operand = inst.GetArg<ir::Operand>(1);
        if (operand.GetOp() != ir::OperandPlus || !operand.GetRight().Null() ||
            !operand.GetLeft().IsValue()) {
            continue;
        }
        const auto right = operand.GetLeft().value;
        if (!Equivalent(left, right)) {
            continue;
        }
        self_xors.insert(&inst);
        for (auto value : {left, right}) {
            auto* def = value.Def();
            if (def && def->GetOp() == ir::OpCode::BitExtract &&
                def->GetUses(false) == References(inst, def)) {
                discarded_inputs.insert(def);
            }
        }
    }
}

bool ScalarCopyAnalysis::IsSelfXor(ir::Inst* inst) const { return self_xors.contains(inst); }

bool ScalarCopyAnalysis::InputDiscarded(ir::Inst* inst) const {
    return discarded_inputs.contains(inst);
}

}  // namespace swift::runtime::backend::arm64
