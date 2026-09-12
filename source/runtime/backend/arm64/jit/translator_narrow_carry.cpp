#include "translator.h"

#include <array>
#include <functional>
#include <unordered_set>

namespace swift::runtime::backend::arm64 {

namespace {

std::optional<std::array<ir::Value, 2>> BinaryValues(ir::Inst* inst) {
    const auto right = inst->GetArg<ir::Operand>(1).GetLeft();
    if (!right.IsValue()) {
        return std::nullopt;
    }
    return std::array{inst->GetArg<ir::Value>(0), right.value};
}

bool IsNarrowPublication(ir::Inst& consumer, ir::Inst* definition) {
    if (consumer.GetOp() != ir::OpCode::SetHostGPR &&
        consumer.GetOp() != ir::OpCode::StoreUniform) {
        return false;
    }
    for (const auto value : consumer.GetValues()) {
        if (value.Def() == definition) {
            return ir::GetValueSizeByte(value.Type()) <= sizeof(u16);
        }
    }
    return false;
}

bool IsZeroInput(ir::Value value, std::vector<ir::Inst*>& chain) {
    auto* inst = value.Def();
    while (inst && (inst->GetOp() == ir::OpCode::ZeroExtend32 ||
                    inst->GetOp() == ir::OpCode::ZeroExtend32To64 ||
                    inst->GetOp() == ir::OpCode::BitCast)) {
        chain.push_back(inst);
        value = inst->GetArg<ir::Value>(0);
        inst = value.Def();
    }
    if (!inst || inst->GetOp() != ir::OpCode::LoadImm ||
        inst->GetArg<ir::Imm>(0).Get() != 0) {
        return false;
    }
    chain.push_back(inst);
    return true;
}

}  // namespace

std::optional<JitTranslator::NarrowCarryFusion>
JitTranslator::MatchNarrowCarryFusion(ir::Inst* inst) {
    if (!cur_block || !inst || inst->GetOp() != ir::OpCode::Add ||
        ir::GetValueSizeByte(inst->ReturnType()) != sizeof(u32) ||
        !GetPseudoFlags(inst).Null()) {
        return std::nullopt;
    }

    const auto final_values = BinaryValues(inst);
    if (!final_values) {
        return std::nullopt;
    }
    ir::Inst* carry_add{};
    ir::Value value{};
    for (const auto candidate : *final_values) {
        if (candidate.Def() && candidate.Def()->GetOp() == ir::OpCode::Add) {
            carry_add = candidate.Def();
        } else {
            value = candidate;
        }
    }
    if (!carry_add || !value.Def() || carry_add->GetUses() != 1 ||
        carry_add->Id() + 1 != inst->Id() ||
        !GetPseudoFlags(carry_add).Null()) {
        return std::nullopt;
    }

    const auto carry_values = BinaryValues(carry_add);
    if (!carry_values) {
        return std::nullopt;
    }
    ir::Inst* carry_test{};
    ir::Value zero{};
    for (const auto candidate : *carry_values) {
        if (candidate.Def() &&
            candidate.Def()->GetOp() == ir::OpCode::TestFlags) {
            carry_test = candidate.Def();
        } else {
            zero = candidate;
        }
    }
    std::vector<ir::Inst*> zero_chain;
    if (!carry_test || carry_test->GetUses() != 1 ||
        carry_test->GetArg<ir::Flags>(0) != ir::Flags::Carry ||
        carry_test->Id() + 1 != carry_add->Id() ||
        !CarryCanStayInPstate(carry_test) ||
        !IsZeroInput(zero, zero_chain)) {
        return std::nullopt;
    }

    bool narrow_publication = false;
    for (auto& consumer : cur_block->GetInstList()) {
        for (const auto used : consumer.GetValues()) {
            if (used.Def() != inst) {
                continue;
            }
            if (!IsNarrowPublication(consumer, inst)) {
                return std::nullopt;
            }
            narrow_publication = true;
        }
    }
    if (!narrow_publication) {
        return std::nullopt;
    }

    auto is_dead_value_op = [](ir::OpCode op) {
        switch (op) {
            case ir::OpCode::Add:
            case ir::OpCode::Sub:
            case ir::OpCode::LslImm:
            case ir::OpCode::ZeroExtend32:
            case ir::OpCode::ZeroExtend32To64:
            case ir::OpCode::BitCast:
                return true;
            default:
                return false;
        }
    };
    std::unordered_set<ir::Inst*> visiting;
    std::function<bool(ir::Inst*)> has_only_dead_uses = [&](ir::Inst* producer) {
        if (!visiting.insert(producer).second) {
            return false;
        }
        for (auto& consumer : cur_block->GetInstList()) {
            for (const auto used : consumer.GetValues()) {
                if (used.Def() != producer || &consumer == carry_add ||
                    (consumer.Id() < disable_instructions.size() &&
                     disable_instructions.test(consumer.Id()))) {
                    continue;
                }
                if (consumer.GetOp() == ir::OpCode::SaveFlags ||
                    consumer.GetOp() == ir::OpCode::BranchOnlyFlags) {
                    if (!GetPseudoFlags(producer).Null()) {
                        return false;
                    }
                    continue;
                }
                if (!is_dead_value_op(consumer.GetOp()) ||
                    !GetPseudoFlags(&consumer).Null() ||
                    !has_only_dead_uses(&consumer)) {
                    return false;
                }
            }
        }
        visiting.erase(producer);
        return true;
    };
    std::vector<ir::Inst*> dead_inputs;
    for (auto* candidate : zero_chain) {
        visiting.clear();
        if (has_only_dead_uses(candidate)) {
            dead_inputs.push_back(candidate);
        }
    }
    return NarrowCarryFusion{
            .carry_test = carry_test,
            .carry_add = carry_add,
            .value = value,
            .dead_inputs = std::move(dead_inputs),
    };
}

void JitTranslator::PrepareNarrowCarryFusions(ir::Block* block) {
    flag_state.narrow_carry_fusions.clear();
    for (auto& inst : block->GetInstList()) {
        auto fusion = MatchNarrowCarryFusion(&inst);
        if (!fusion) {
            continue;
        }
        disable_instructions.set(fusion->carry_test->Id());
        disable_instructions.set(fusion->carry_add->Id());
        for (auto* input : fusion->dead_inputs) {
            disable_instructions.set(input->Id());
        }
        flag_state.narrow_carry_fusions.emplace(&inst, std::move(*fusion));
    }
}

}  // namespace swift::runtime::backend::arm64
