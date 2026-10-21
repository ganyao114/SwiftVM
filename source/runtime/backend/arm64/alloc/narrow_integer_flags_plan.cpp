#include "narrow_integer_flags_plan.h"
#include "integer_branch_plan.h"
#include "runtime/backend/reg_alloc.h"
#include <array>
#include <functional>
#include <unordered_map>

namespace swift::runtime::backend::arm64 {
namespace {
std::optional<std::array<ir::Value, 2>> BinaryValues(ir::Inst* inst) {
    const auto operand = inst->GetArg<ir::Operand>(1);
    if (!operand.GetRight().Null()) return std::nullopt;
    const auto right = operand.GetLeft();
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

ir::Flags ConditionFlags(ir::Cond cond) {
    switch (cond) {
        case ir::Cond::EQ:
        case ir::Cond::NE: return ir::Flags::Zero;
        case ir::Cond::CS:
        case ir::Cond::CC: return ir::Flags::Carry;
        case ir::Cond::MI:
        case ir::Cond::PL: return ir::Flags::Negate;
        case ir::Cond::VS:
        case ir::Cond::VC: return ir::Flags::Overflow;
        case ir::Cond::HI:
        case ir::Cond::LS: return ir::Flags::Carry | ir::Flags::Zero;
        case ir::Cond::GE:
        case ir::Cond::LT: return ir::Flags::Negate | ir::Flags::Overflow;
        case ir::Cond::GT:
        case ir::Cond::LE:
            return ir::Flags::Negate | ir::Flags::Overflow | ir::Flags::Zero;
        case ir::Cond::AL:
        case ir::Cond::NV: return ir::Flags::None;
    }
    return ir::Flags::All;
}

ir::Flags ExplicitFlagRead(const ir::Inst& inst) {
    switch (inst.GetOp()) {
        case ir::OpCode::TestFlags:
        case ir::OpCode::TestNotFlags:
            return inst.GetArg<ir::Flags>(0);
        case ir::OpCode::Adc:
        case ir::OpCode::Sbb:
        case ir::OpCode::InvertCarry:
            return ir::Flags::Carry;
        case ir::OpCode::CallLambda:
        case ir::OpCode::CallLocation:
        case ir::OpCode::CallDynamic:
        case ir::OpCode::X87Op:
            return ir::Flags::All;
        case ir::OpCode::GetFlags:
            return inst.GetArg<ir::Flags>(1);
        case ir::OpCode::BranchOnlyFlags:
            return inst.GetArg<ir::Flags>(1);
        case ir::OpCode::CondSelect:
        case ir::OpCode::CondSet:
        case ir::OpCode::LocalCondSet:
            return ConditionFlags(inst.GetArg<ir::Cond>(0));
        default:
            return ir::Flags::None;
    }
}

bool IsFaultBoundary(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::LoadMemory:
        case ir::OpCode::StoreMemory:
        case ir::OpCode::LoadMemoryTSO:
        case ir::OpCode::StoreMemoryTSO:
        case ir::OpCode::MemoryCopy:
        case ir::OpCode::MemoryCopyTSO:
        case ir::OpCode::CompareAndSwap:
        case ir::OpCode::CompareAndSwap128:
        case ir::OpCode::AtomicExchange:
        case ir::OpCode::AtomicFetchAdd:
        case ir::OpCode::AtomicRMW:
        case ir::OpCode::CheckMemoryAlignment:
            return true;
        default:
            return false;
    }
}

bool IsNarrowZeroExtended(ir::Value value, u32 width) {
    if (!value.Def() || (width != sizeof(u8) && width != sizeof(u16))) {
        return false;
    }
    auto* inst = value.Def();
    if (inst->GetOp() == ir::OpCode::LoadMemory ||
        inst->GetOp() == ir::OpCode::LoadMemoryTSO ||
        inst->GetOp() == ir::OpCode::LoadUniform ||
        inst->GetOp() == ir::OpCode::GetHostGPR) {
        return ir::GetValueSizeByte(inst->ReturnType()) == width;
    }
    if (inst->GetOp() == ir::OpCode::ZeroExtend32 ||
        inst->GetOp() == ir::OpCode::ZeroExtend32To64 ||
        inst->GetOp() == ir::OpCode::BitCast) {
        return IsNarrowZeroExtended(inst->GetArg<ir::Value>(0), width);
    }
    return inst->GetOp() == ir::OpCode::BitExtract &&
           ir::GetValueSizeByte(inst->ReturnType()) == width &&
           inst->GetArg<ir::Imm>(1).Get() == 0 &&
           inst->GetArg<ir::Imm>(2).Get() == width * 8;
}

}

namespace {
bool IsAddSubImmediate(u64 value) { return value <= 0xfff || ((value & 0xfff) == 0 && (value >> 12) <= 0xfff); }
class IntegerFlagsBuilder {
public:
    IntegerFlagsBuilder(const BlockAnalysisIndex& a, const NarrowIntegerFlagsInputs& i) : analysis(a), inputs(i) {
        if (inputs.pstate) pstate = inputs.pstate;
        else { owned_pstate.emplace(BuildPstateFacts(analysis)); pstate = &*owned_pstate; }
    }
    NarrowIntegerFlagsPlan Build();
private:
    StaticFlagDemand Demand(const ir::Inst* inst) const {
        const auto found = inputs.demands.find(inst);
        return found == inputs.demands.end() ? StaticFlagDemand{} : found->second;
    }
    bool Covered(ir::Inst* inst) const {
        return (inst->Id() < inputs.earlier_coverage.size() && inputs.earlier_coverage.test(inst->Id())) || plan.covered.contains(inst);
    }
    std::optional<NarrowCarryAction> MatchCarry(ir::Inst* inst);
    std::optional<NarrowCompareAction> MatchCompare(ir::Inst* inst,
            const std::map<ir::Inst*, NarrowCarryAction>& carries);
    const BlockAnalysisIndex& analysis;
    const NarrowIntegerFlagsInputs& inputs;
    NarrowIntegerFlagsPlan plan;
    std::optional<PstateFacts> owned_pstate;
    const PstateFacts* pstate{};
};
std::optional<NarrowCarryAction>
IntegerFlagsBuilder::MatchCarry(ir::Inst* inst) {
    if (!inst || inst->GetOp() != ir::OpCode::Add ||
        ir::GetValueSizeByte(inst->ReturnType()) != sizeof(u32) ||
        !Demand(inst).Null()) {
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
    if (!carry_add || !analysis.Contains(carry_add) || !value.Def() || carry_add->GetUses() != 1 ||
        carry_add->Id() + 1 != inst->Id() ||
        !Demand(carry_add).Null()) {
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
    if (!carry_test || !analysis.Contains(carry_test) || carry_test->GetUses() != 1 ||
        carry_test->GetArg<ir::Flags>(0) != ir::Flags::Carry ||
        carry_test->Id() + 1 != carry_add->Id() ||
        False(pstate->At(carry_test) & ir::Flags::Carry) ||
        !IsZeroInput(zero, zero_chain)) {
        return std::nullopt;
    }
    // Once AdvancePC commits a subtraction, the canonical carrier keeps
    // ARM's native C polarity while the fusion consumes live host C. Keep the
    // CFINV fusion within one guest boundary; ordinary narrow lowering handles
    // the cross-boundary case through the canonical carry value.
    if (inputs.canonical_carry && pstate->first_advance && *pstate->first_advance < carry_test->Id()) return std::nullopt;

    bool narrow_publication = false;
    for (const auto& use : analysis.Uses(inst)) {
        if (!IsNarrowPublication(*use.consumer, inst)) return std::nullopt;
        narrow_publication = true;
    }
    if (!narrow_publication) return std::nullopt;

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
    enum class State { Visiting, Accepted, Rejected };
    std::unordered_map<ir::Inst*, State> memo;
    std::function<bool(ir::Inst*)> has_only_dead_uses = [&](ir::Inst* producer) {
        if (const auto found = memo.find(producer); found != memo.end()) return found->second == State::Accepted;
        // A local use map cannot certify that a definition is dead to other
        // blocks or the terminal. Preserve it unless every global use is here.
        if (analysis.DirectUseCount(producer) != producer->GetUses(false)) {
            memo.emplace(producer, State::Rejected);
            return false;
        }
        memo.emplace(producer, State::Visiting);
        bool safe = true;
        for (const auto& use : analysis.Uses(producer)) {
            auto* consumer = use.consumer;
            if (consumer == carry_add || Covered(consumer)) continue;
            if (consumer->GetOp() == ir::OpCode::SaveFlags || consumer->GetOp() == ir::OpCode::BranchOnlyFlags) {
                safe = Demand(producer).Null();
            } else safe = is_dead_value_op(consumer->GetOp()) && Demand(consumer).Null() && has_only_dead_uses(consumer);
            if (!safe) break;
        }
        memo[producer] = safe ? State::Accepted : State::Rejected;
        return safe;
    };
    std::vector<ir::Inst*> dead_inputs;
    for (auto* candidate : zero_chain)
        if (analysis.Contains(candidate) && has_only_dead_uses(candidate)) dead_inputs.push_back(candidate);
    return NarrowCarryAction{
            .carry_test = carry_test,
            .carry_add = carry_add,
            .value = value,
            .dead_inputs = std::move(dead_inputs),
    };
}

std::optional<NarrowCompareAction>
IntegerFlagsBuilder::MatchCompare(ir::Inst* inst,
        const std::map<ir::Inst*, NarrowCarryAction>& carries) {
    if (!inst || inst->GetOp() != ir::OpCode::Sub ||
        inst->GetUses() != 0 || inputs.excluded_comparisons.contains(inst)) {
        return std::nullopt;
    }
    const u32 width = ir::GetValueSizeByte(inst->ReturnType());
    const auto flags = Demand(inst);
    if ((width != sizeof(u8) && width != sizeof(u16)) || flags.Null() ||
        flags.branch_only || flags.clear != ir::Flags::None ||
        False(flags.set & ir::Flags::Carry)) {
        return std::nullopt;
    }

    const auto extra_flags = flags.set & ~ir::Flags::Carry;
    if (extra_flags != ir::Flags::None) {
        const bool feeds_fused_carry = std::ranges::any_of(
                carries, [&](const auto& fusion) {
                    return fusion.second.carry_test->Id() > inst->Id() &&
                           True(pstate->At(fusion.second.carry_test) & ir::Flags::Carry);
                });
        if (!feeds_fused_carry) {
            return std::nullopt;
        }
        auto pending = extra_flags;
        for (auto* scan : analysis.Between(inst->Id(), UINT32_MAX)) {
            if (pending == ir::Flags::None) break;
            if (IsFaultBoundary(scan->GetOp()) ||
                True(ExplicitFlagRead(*scan) & pending)) {
                return std::nullopt;
            }
            const auto writer = Demand(&*scan);
            pending &= ~(writer.branch_only ? ir::Flags::All : writer.set);
            if (scan->GetOp() == ir::OpCode::ClearFlags) {
                pending &= ~scan->GetArg<ir::Flags>(0);
            } else if (scan->GetOp() == ir::OpCode::SetOverflow) {
                pending &= ~ir::Flags::Overflow;
            }
        }
        if (pending != ir::Flags::None) {
            return std::nullopt;
        }
    }

    const auto forwarded = inputs.comparison_inputs.find(inst);
    const auto left = forwarded == inputs.comparison_inputs.end() ? inst->GetArg<ir::Value>(0) : forwarded->second;
    if (!IsNarrowZeroExtended(left, width)) {
        return std::nullopt;
    }
    const auto operand = inst->GetArg<ir::Operand>(1);
    if (operand.IsImm()) {
        const u64 immediate = operand.GetLeft().imm.Get();
        const u64 limit = u64{1} << (width * 8);
        if (immediate < limit && IsAddSubImmediate(immediate)) {
            return NarrowCompareAction{
                    .left = left,
                    .immediate = static_cast<u32>(immediate),
                    .width = static_cast<u8>(width),
            };
        }
        return std::nullopt;
    }
    if (!operand.GetRight().Null() || !operand.GetLeft().IsValue()) {
        return std::nullopt;
    }
    const auto right = operand.GetLeft().value;
    if (right.Def() && right.Def()->GetOp() == ir::OpCode::LoadImm &&
        right.Def()->GetUses(false) == 1) {
        const u64 immediate = right.Def()->GetArg<ir::Imm>(0).Get();
        const u64 limit = u64{1} << (width * 8);
        if (immediate < limit && IsAddSubImmediate(immediate)) {
            return NarrowCompareAction{
                    .left = left,
                    .immediate_load = right.Def(),
                    .immediate = static_cast<u32>(immediate),
                    .width = static_cast<u8>(width),
            };
        }
    }
    if (!IsNarrowZeroExtended(right, width)) {
        return std::nullopt;
    }
    return NarrowCompareAction{
            .left = left,
            .right = right,
            .width = static_cast<u8>(width),
    };
}


NarrowIntegerFlagsPlan IntegerFlagsBuilder::Build() {
    for (auto* inst : analysis.Instructions()) {
        if (auto action = MatchCarry(inst)) {
            plan.covered.insert(action->carry_test); plan.covered.insert(action->carry_add);
            for (auto* input : action->dead_inputs) plan.covered.insert(input);
            plan.carries.emplace(inst, std::move(*action));
        }
    }
    for (auto* inst : analysis.Instructions()) {
        if (auto action = MatchCompare(inst, plan.carries)) {
            if (action->immediate_load) plan.covered.insert(action->immediate_load);
            plan.comparisons.emplace(inst, *action);
        }
    }
    return std::move(plan);
}
}

namespace {
template<class Instructions>
bool HasNarrowCandidates(const Instructions& instructions) {
    for (const auto& inst : instructions) {
        if (inst.GetOp() != ir::OpCode::Add && inst.GetOp() != ir::OpCode::Sub) continue;
        const auto width = ir::GetValueSizeByte(inst.ReturnType());
        if (inst.GetOp() == ir::OpCode::Sub && inst.GetUses() == 0 && (width == 1 || width == 2)) return true;
        if (inst.GetOp() != ir::OpCode::Add || width != 4) continue;
        const auto values = BinaryValues(const_cast<ir::Inst*>(&inst));
        if (!values) continue;
        for (const auto value : *values) {
            auto* carry = value.Def();
            if (!carry || carry->GetOp() != ir::OpCode::Add || carry->Id() + 1 != inst.Id()) continue;
            const auto inner = BinaryValues(carry);
            if (!inner) continue;
            for (const auto input : *inner)
                if (input.Def() && input.Def()->GetOp() == ir::OpCode::TestFlags) return true;
        }
    }
    return false;
}
} // namespace
bool HasNarrowIntegerFlagsCandidates(const ir::Block& block) {
    return HasNarrowCandidates(block.GetInstList());
}
NarrowIntegerFlagsPlan BuildNarrowIntegerFlagsPlan(const BlockAnalysisIndex& analysis, const NarrowIntegerFlagsInputs& inputs) {
    return IntegerFlagsBuilder(analysis, inputs).Build();
}


namespace {
PseudoFlagsDemand PreparationDemand(const ir::Inst* inst,
        const PseudoFlagsPlan& base, const IntegerBranchPlan* branch) {
    return branch && branch->edge.producer == inst
            ? PseudoFlagsDemand{branch->edge.required, ir::Flags::None, true}
            : base.For(inst);
}
std::optional<NarrowFlagsInput> MatchNarrowFlagsInput(
        ir::Inst* extract, ir::Inst* consumer, const RegAlloc& allocation,
        const PseudoFlagsPlan& base, const IntegerBranchPlan* branch) {
    if (extract->GetOp() != ir::OpCode::BitExtract || extract->GetUses(false) != 1)
        return std::nullopt;
    const u32 width = ir::GetValueSizeByte(extract->ReturnType());
    if ((width != sizeof(u8) && width != sizeof(u16)) ||
        extract->GetArg<ir::Imm>(1).Get() != 0 || extract->GetArg<ir::Imm>(2).Get() != width * 8)
        return std::nullopt;
    if (!consumer || (consumer->GetOp() != ir::OpCode::Add &&
                      consumer->GetOp() != ir::OpCode::Sub && consumer->GetOp() != ir::OpCode::Neg) ||
        consumer->GetArg<ir::Value>(0).Def() != extract ||
        ir::GetValueSizeByte(consumer->ReturnType()) != width ||
        !True(PreparationDemand(consumer, base, branch).set & ir::Flags::NZCV))
        return std::nullopt;
    const auto source = extract->GetArg<ir::Value>(0);
    // A reload lifetime ending at the extract cannot forward to its consumer.
    if (allocation.ValueType(source) == RegAlloc::MEM) return std::nullopt;
    // Freeze the actual operand, including the consumer's type view. A
    // selected low view keeps the original extract; otherwise read its source.
    return NarrowFlagsInput{allocation.GPRLowView(extract->Id())
            ? consumer->GetArg<ir::Value>(0) : source, consumer};
}
}
IntegerFlagPreparationPlan BuildIntegerFlagPreparation(
        const BlockAnalysisIndex& analysis, const RegAlloc& allocation,
        const PseudoFlagsPlan& base_flags, const IntegerBranchPlan* selected_branch,
        const BitVector& earlier_coverage, const PstateFacts* pstate,
        bool canonical_carry) {
    IntegerFlagPreparationPlan prepared;
    const auto instructions = analysis.Instructions();
    for (size_t i = 0; i < instructions.size(); ++i)
        if (auto input = MatchNarrowFlagsInput(instructions[i],
                i + 1 < instructions.size() ? instructions[i + 1] : nullptr,
                allocation, base_flags, selected_branch))
            prepared.inputs.emplace(instructions[i], *input);
    const auto values = instructions | std::views::transform([](const ir::Inst* inst) -> const ir::Inst& { return *inst; });
    if (!HasNarrowCandidates(values)) return prepared;
    NarrowIntegerFlagsInputs inputs{earlier_coverage};
    inputs.canonical_carry = canonical_carry; inputs.pstate = pstate;
    for (auto* inst : instructions) {
        const auto flags = PreparationDemand(inst, base_flags, selected_branch);
        if (!flags.Null()) inputs.demands.emplace(inst, StaticFlagDemand{flags.set, flags.clear, flags.branch_only});
        if (inst->GetOp() == ir::OpCode::Sub) {
            const auto value = inst->GetArg<ir::Value>(0);
            const auto found = prepared.inputs.find(value.Def());
            inputs.comparison_inputs.emplace(inst,
                    found == prepared.inputs.end() ? value : found->second.source);
        }
    }
    // Backedge/PFAF selection happens after this preparation stage. Its final
    // emitter demand must not change these qualifications or exclusion inputs.
    prepared.integer = BuildNarrowIntegerFlagsPlan(analysis, inputs);
    return prepared;
}
}
