#pragma once

#include <unordered_map>
#include <vector>
#include "runtime/common/variant_util.h"
#include "runtime/ir/block.h"

namespace swift::runtime::backend {

// Fresh instruction units have only forward local branches. Validate data
// availability on both branch paths before either engine can commit a prefix.
// This deliberately does not change ordinary/function-mode Phi contracts.
inline bool SingleInstructionControlFlowValid(const ir::Block& block) {
    using O = ir::OpCode;
    if (!block.HasTerminal()) return false;
    std::vector<const ir::Inst*> body;
    std::unordered_map<const ir::Inst*, size_t> positions, targets;
    for (const auto& inst : block.GetInstList()) {
        if (body.size() >= 8192 || inst.GetOp() == O::AddPhi) return false;
        positions.emplace(&inst, body.size());
        body.push_back(&inst);
    }
    for (size_t index = 0; index < body.size(); ++index) {
        const auto& inst = *body[index];
        if (inst.GetOp() != O::BindLabel) continue;
        if (!inst.ArgAt(0).IsValue()) return false;
        const auto* branch = inst.ArgAt(0).Get<ir::Value>().Def();
        const auto found = positions.find(branch);
        if (found == positions.end() || found->second >= index ||
            (branch->GetOp() != O::Goto && branch->GetOp() != O::NotGoto) ||
            !targets.emplace(branch, index).second) return false;
    }
    std::vector<std::vector<size_t>> incoming(body.size() + 1);
    for (size_t index = 0; index < body.size(); ++index) {
        incoming[index + 1].push_back(index);
        const auto* inst = body[index];
        if (inst->GetOp() != O::Goto && inst->GetOp() != O::NotGoto) continue;
        const auto target = targets.find(inst);
        if (target == targets.end() || target->second <= index) return false;
        incoming[target->second].push_back(index);
    }
    // Lexical positions are a topological order for this finite DAG. The
    // common ancestor of all predecessors is its immediate dominator.
    std::vector<size_t> dominator(body.size() + 1);
    for (size_t index = 1; index <= body.size(); ++index) {
        size_t parent = incoming[index].front();
        for (auto other : incoming[index]) {
            while (parent != other) {
                if (parent > other) parent = dominator[parent];
                else other = dominator[other];
            }
        }
        dominator[index] = parent;
    }
    const auto available = [&](ir::Value value, size_t use) {
        const auto found = positions.find(value.Def());
        if (found == positions.end() || found->second >= use) return false;
        // Branch values identify labels; neither engine materializes a data
        // result for them, even if hand-built IR gives the token a scalar type.
        if (value.Def()->GetOp() == O::Goto || value.Def()->GetOp() == O::NotGoto ||
            value.Def()->ReturnType() == ir::ValueType::VOID) return false;
        auto cursor = use;
        while (cursor > found->second) cursor = dominator[cursor];
        return cursor == found->second;
    };
    for (size_t index = 0; index < body.size(); ++index) {
        const auto& inst = *body[index];
        for (unsigned slot = 0; slot < ir::Inst::max_args; ++slot) {
            auto& arg = inst.ArgAt(slot);
            if (arg.IsValue()) {
                // BindLabel names a control edge, not an executed data read.
                if (inst.GetOp() == O::BindLabel && slot == 0) continue;
                if (!available(arg.Get<ir::Value>(), index)) return false;
            } else if (arg.IsLambda()) {
                const auto lambda = arg.Get<ir::Lambda>();
                if (lambda.IsValue() && !available(lambda.GetValue(), index)) return false;
            } else if (arg.IsParams()) {
                for (const auto& parameter : arg.Get<ir::Params>())
                    if (parameter.data.IsValue() && !available(parameter.data.value, index)) return false;
            }
        }
    }
    const auto terminal_valid = [&](const auto& self, const ir::Terminal& terminal, unsigned depth) -> bool {
        if (depth > 64) return false;
        return VisitVariant<bool>(terminal, [&](const auto& term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::If>)
                return available(term.cond, body.size()) &&
                    self(self, term.then_, depth + 1) && self(self, term.else_, depth + 1);
            else if constexpr (std::is_same_v<T, ir::terminal::Condition>)
                return self(self, term.then_, depth + 1) && self(self, term.else_, depth + 1);
            else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>)
                return self(self, term.else_, depth + 1);
            else if constexpr (std::is_same_v<T, ir::terminal::Invalid> ||
                               std::is_same_v<T, ir::terminal::Switch>) return false;
            else return true;
        });
    };
    return terminal_valid(terminal_valid, block.GetTerminal(), 0);
}

} // namespace swift::runtime::backend
