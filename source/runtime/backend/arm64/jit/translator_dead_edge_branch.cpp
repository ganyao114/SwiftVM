#include "translator.h"

#include <algorithm>

namespace swift::runtime::backend::arm64 {

namespace {

struct ConditionRecipe {
    ir::Flags required{};
    ir::Cond raw_condition{};
    std::unordered_set<ir::Inst*> discarded{};
};

ir::Inst* OperandDefinition(const ir::Operand& operand) {
    const auto value = operand.GetLeft();
    return value.IsValue() ? value.value.Def() : nullptr;
}

std::optional<ConditionRecipe> AnalyzeCondition(ir::Inst* condition) {
    if (!condition || condition->GetUses() != 1) {
        return std::nullopt;
    }
    if (condition->GetOp() == ir::OpCode::LocalCondSet) {
        switch (condition->GetArg<ir::Cond>(0)) {
            case ir::Cond::EQ:
            case ir::Cond::NE:
                return ConditionRecipe{ir::Flags::Zero,
                                     condition->GetArg<ir::Cond>(0)};
            case ir::Cond::CS:
                return ConditionRecipe{ir::Flags::Carry, ir::Cond::CC};
            case ir::Cond::CC:
                return ConditionRecipe{ir::Flags::Carry, ir::Cond::CS};
            default:
                return std::nullopt;
        }
    }
    if (condition->GetOp() != ir::OpCode::And &&
        condition->GetOp() != ir::OpCode::Or) {
        return std::nullopt;
    }

    auto* left = condition->GetArg<ir::Value>(0).Def();
    auto* right = OperandDefinition(condition->GetArg<ir::Operand>(1));
    auto is_zero_cond = [](ir::Inst* inst, ir::Cond cond) {
        return inst && inst->GetOp() == ir::OpCode::CondSet &&
               inst->GetUses() == 1 && inst->GetArg<ir::Cond>(0) == cond;
    };
    ir::Inst* predicate = left;
    ir::Inst* zero_condition = right;
    if (is_zero_cond(left, ir::Cond::EQ) ||
        is_zero_cond(left, ir::Cond::NE)) {
        predicate = right;
        zero_condition = left;
    }
    if (!predicate || predicate->GetUses() != 1 ||
        !zero_condition || zero_condition->GetUses() != 1) {
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
    if (!carry_test || carry_test->GetOp() != ir::OpCode::TestFlags ||
        carry_test->GetUses() != 1 ||
        carry_test->GetArg<ir::Flags>(0) != ir::Flags::Carry) {
        return std::nullopt;
    }
    ConditionRecipe recipe{ir::Flags::Carry | ir::Flags::Zero,
                       high ? ir::Cond::HI : ir::Cond::LS};
    recipe.discarded.insert(carry_test);
    recipe.discarded.insert(predicate);
    recipe.discarded.insert(zero_condition);
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

void JitTranslator::PrepareDeadEdgeIntegerBranch(ir::Block* block) {
    flag_state.dead_edge_integer_branch.reset();

    if (!block->HasDeadEdgeIntegerBranchProof()) {
        return;
    }

    auto terminal = block->GetTerminal();
    auto* branch = boost::get<ir::terminal::If>(&terminal);
    auto* condition = branch ? branch->cond.Def() : nullptr;
    auto condition_recipe = AnalyzeCondition(condition);
    if (!condition_recipe) {
        return;
    }

    std::vector<ir::Inst*> instructions;
    instructions.reserve(block->GetInstList().size());
    for (auto& inst : block->GetInstList()) {
        instructions.push_back(&inst);
    }
    const auto condition_it =
            std::find(instructions.begin(), instructions.end(), condition);
    if (condition_it == instructions.end()) {
        return;
    }
    const size_t condition_index = std::distance(instructions.begin(), condition_it);

    size_t producer_end = condition_index;
    while (producer_end > 0 &&
           instructions[producer_end - 1]->GetOp() != ir::OpCode::AdvancePC) {
        --producer_end;
    }
    if (producer_end == 0) {
        return;
    }
    const size_t producer_advance = producer_end - 1;
    size_t producer_begin = producer_advance;
    while (producer_begin > 0 &&
           instructions[producer_begin - 1]->GetOp() != ir::OpCode::AdvancePC) {
        --producer_begin;
    }

    DeadEdgeIntegerBranch recipe{
            .condition = condition,
            .required = condition_recipe->required,
            .raw_condition = condition_recipe->raw_condition,
            .discarded = std::move(condition_recipe->discarded),
    };
    u32 inverts = 0;
    u32 polarity_stores = 0;
    ir::Value zero_value{};
    auto resolve_bitcast = [](ir::Value value) {
        while (value.Def() && value.Def()->IsBitCastOperation()) {
            value = value.Def()->GetArg<ir::Value>(0);
        }
        return value;
    };
    for (size_t i = producer_begin; i < condition_index; ++i) {
        auto* inst = instructions[i];
        if (recipe.discarded.contains(inst)) {
            continue;
        }
        switch (inst->GetOp()) {
            case ir::OpCode::SaveFlags: {
                recipe.discarded.insert(inst);
                auto value = inst->GetArg<ir::Value>(0);
                if (!value.Def() ||
                    (recipe.producer && recipe.producer != value.Def())) {
                    return;
                }
                if (value.Def()->GetOp() != ir::OpCode::Sub) {
                    return;
                }
                recipe.producer = value.Def();
                break;
            }
            case ir::OpCode::BranchOnlyFlags: {
                recipe.discarded.insert(inst);
                const auto value = inst->GetArg<ir::Value>(0);
                if (recipe.producer || !value.Def() ||
                    value.Def()->GetOp() != ir::OpCode::And ||
                    recipe.required != ir::Flags::Zero ||
                    !True(inst->GetArg<ir::Flags>(1) & recipe.required)) {
                    return;
                }
                const auto left = value.Def()->GetArg<ir::Value>(0);
                const auto right = value.Def()->GetArg<ir::Operand>(1);
                if (!right.GetRight().Null() ||
                    !right.GetLeft().IsValue() ||
                    resolve_bitcast(left).Def() !=
                            resolve_bitcast(right.GetLeft().value).Def()) {
                    return;
                }
                recipe.producer = value.Def();
                zero_value = resolve_bitcast(left);
                break;
            }
            case ir::OpCode::ClearFlags:
                recipe.discarded.insert(inst);
                break;
            case ir::OpCode::InvertCarry:
                ++inverts;
                recipe.discarded.insert(inst);
                break;
            case ir::OpCode::StoreUniform:
                if (IsPolarityStore(*inst)) {
                    ++polarity_stores;
                    recipe.discarded.insert(inst);
                    auto* value = inst->GetArg<ir::Value>(1).Def();
                    if (value && value->GetUses(false) == 1) {
                        recipe.discarded.insert(value);
                    }
                }
                break;
            default:
                if (IsFlagObserver(inst->GetOp())) {
                    return;
                }
                break;
        }
    }
    const bool valid_inverts = zero_value.Defined()
            ? inverts == 0
            : (recipe.required == ir::Flags::Zero ? inverts <= 1 : inverts == 1);
    if (!recipe.producer || !valid_inverts ||
        polarity_stores > 1 ||
        recipe.producer->Id() >= condition->Id()) {
        return;
    }

    if (zero_value.Defined()) {
        const u32 width = ir::GetValueSizeByte(recipe.producer->ReturnType());
        ir::Inst* publication{};
        u16 target{};
        for (auto* inst : instructions) {
            if (inst->Id() >= recipe.producer->Id() ||
                inst->GetOp() != ir::OpCode::SetHostGPR ||
                inst->GetArg<ir::Imm>(2).Get() != 0 ||
                ir::GetValueSizeByte(inst->GetArg<ir::Value>(0).Type()) != width ||
                resolve_bitcast(inst->GetArg<ir::Value>(0)).Def() !=
                        zero_value.Def()) {
                continue;
            }
            const u32 candidate = inst->GetArg<ir::Imm>(1).Get();
            if (candidate > 9 && (candidate < 19 || candidate > 23) &&
                candidate != 29) {
                continue;
            }
            publication = inst;
            target = static_cast<u16>(candidate);
        }
        if (!publication || context.X(zero_value).GetCode() != target) {
            return;
        }
        for (auto* inst : instructions) {
            if (inst->Id() <= publication->Id() ||
                inst->Id() >= condition->Id()) {
                continue;
            }
            if (inst->GetOp() == ir::OpCode::SetHostGPR &&
                inst->GetArg<ir::Imm>(1).Get() == target) {
                return;
            }
        }
        recipe.zero_target = target;
        recipe.zero_is_64 = width == sizeof(u64);
    }

    bool after_producer = false;
    for (size_t i = producer_begin; i < condition_index; ++i) {
        auto* inst = instructions[i];
        if (inst == recipe.producer) {
            after_producer = true;
            continue;
        }
        if (!after_producer || recipe.discarded.contains(inst)) {
            continue;
        }
        if (!RetainsPendingHostNZCV(*inst) || MayFaultOrObserve(*inst)) {
            return;
        }
    }

    for (auto* inst : recipe.discarded) {
        disable_instructions.set(inst->Id());
    }
    local_conditions.emplace(condition, MapCond(recipe.raw_condition));
    flag_state.dead_edge_integer_branch = std::move(recipe);
}

bool JitTranslator::IsDeadEdgeIntegerBranchProducer(ir::Inst* inst) const {
    return flag_state.dead_edge_integer_branch && flag_state.dead_edge_integer_branch->producer == inst;
}

std::optional<ir::Cond> JitTranslator::DeadEdgeIntegerBranchCondition(
        ir::Inst* inst) const {
    if (!flag_state.dead_edge_integer_branch || flag_state.dead_edge_integer_branch->condition != inst) {
        return std::nullopt;
    }
    return flag_state.dead_edge_integer_branch->raw_condition;
}

bool JitTranslator::EmitDeadEdgeZeroBranch(ir::Value condition, Label* label,
                                           bool on_true) {
    if (!flag_state.dead_edge_integer_branch ||
        flag_state.dead_edge_integer_branch->condition != condition.Def() ||
        !flag_state.dead_edge_integer_branch->zero_target) {
        return false;
    }
    const auto target = XRegister(*flag_state.dead_edge_integer_branch->zero_target);
    const auto value = flag_state.dead_edge_integer_branch->zero_is_64
            ? Register{target.X()}
            : Register{target.W()};
    const bool zero_on_true =
            flag_state.dead_edge_integer_branch->raw_condition == ir::Cond::EQ;
    if (zero_on_true == on_true) {
        masm.Cbz(value, label);
    } else {
        masm.Cbnz(value, label);
    }
    return true;
}

std::optional<JitTranslator::DeadNarrowImmediateBranch>
JitTranslator::MatchDeadNarrowImmediateBranch(ir::Inst* inst) const {
    if (!inst || inst->GetOp() != ir::OpCode::Sub || inst->GetUses() != 0 ||
        !IsDeadEdgeIntegerBranchProducer(inst)) {
        return std::nullopt;
    }
    const auto required = flag_state.dead_edge_integer_branch->required;
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
    if (!load || load->GetOp() != ir::OpCode::LoadImm ||
        load->GetUses(false) != 1) {
        return std::nullopt;
    }
    const u64 immediate = load->GetArg<ir::Imm>(0).Get();
    if (!masm.IsImmAddSub(immediate)) {
        return std::nullopt;
    }
    return DeadNarrowImmediateBranch{
            .producer = inst,
            .immediate_load = load,
            .immediate = immediate,
            .required = required,
            .width = static_cast<u8>(width),
    };
}

void JitTranslator::PrepareDeadNarrowImmediateBranch() {
    flag_state.dead_narrow_immediate_branch.reset();
    if (!flag_state.dead_edge_integer_branch) {
        return;
    }
    flag_state.dead_narrow_immediate_branch =
            MatchDeadNarrowImmediateBranch(flag_state.dead_edge_integer_branch->producer);
    if (flag_state.dead_narrow_immediate_branch) {
        disable_instructions.set(
                flag_state.dead_narrow_immediate_branch->immediate_load->Id());
    }
}

}  // namespace swift::runtime::backend::arm64
