#include "scalar_register_plan.h"

#include <algorithm>
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::x64 {
namespace {
using O = ir::OpCode;
bool ControlBoundary(O op) {
    return op == O::Goto || op == O::NotGoto || op == O::BindLabel;
}
bool ScalarType(ir::ValueType type) {
    return type >= ir::ValueType::U8 && type <= ir::ValueType::S64;
}
// Slots are physical, including Operand's op/left/right expansion. Never
// infer safety from an opcode alone: extra values, Lambda/Params values and
// raw vector views must all keep the producer's canonical home. The scalar
// whitelist also promises that all these reads finish before the emitter's
// final Write. Do not classify an input read after output publication here.
bool ScalarReadConsumer(ir::Inst* inst, u32 slot, ir::Value value) {
    if (!ScalarType(value.Type())) return false;
    switch (inst->GetOp()) {
        case O::SaveFlags: case O::BranchOnlyFlags:
            return slot == 0;
        case O::StoreUniform:
            return slot == 1 && inst->ArgAt(0).GetType() == ir::ArgType::Uniform &&
                   ScalarType(inst->GetArg<ir::Uniform>(0).GetType());
        default: break;
    }
    if (!ScalarType(inst->ReturnType())) return false;
    switch (inst->GetOp()) {
        case O::Add: case O::Sub: case O::Adc: case O::Sbb:
        case O::And: case O::Or: case O::Xor: case O::AndNot: case O::Mul:
            return slot == 0 || slot == 2 || slot == 3;
        case O::GetOperand:
            return slot == 1 || slot == 2;
        case O::MulSub:
            return slot <= 2;
        case O::MulHigh: case O::BitInsert:
        case O::LslValue: case O::LsrValue: case O::AsrValue: case O::RorValue:
            return slot <= 1;
        case O::Neg:
        case O::ZeroExtend32: case O::ZeroExtend32To64: case O::ZeroExtend64: case O::SignExtend:
        case O::LslImm: case O::LsrImm: case O::AsrImm: case O::RorImm:
        case O::ByteSwap: case O::BitExtract: case O::BitClear:
        case O::TestBit: case O::TestZero: case O::TestNotZero:
            return slot == 0;
        default: return false;
    }
}
bool CacheableProducer(ir::Inst* inst) {
    if (!ScalarType(inst->ReturnType())) return false;
    // Every allowed lowering ends in scalar Write. Pair results and raw
    // 128-bit containers are intentionally absent even with scalar annotations.
    switch (inst->GetOp()) {
        case O::LoadUniform:
            return inst->ArgAt(0).GetType() == ir::ArgType::Uniform && ScalarType(inst->GetArg<ir::Uniform>(0).GetType());
        case O::LoadImm: case O::GetOperand:
        case O::Add: case O::Sub: case O::Adc: case O::Sbb:
        case O::And: case O::Or: case O::Xor: case O::AndNot:
        case O::Neg: case O::Not: case O::Mul: case O::MulSub: case O::MulHigh:
        case O::ZeroExtend32: case O::ZeroExtend32To64: case O::ZeroExtend64: case O::SignExtend:
        case O::LslImm: case O::LslValue: case O::LsrImm: case O::LsrValue:
        case O::AsrImm: case O::AsrValue: case O::RorImm: case O::RorValue:
        case O::ByteSwap: case O::BitExtract: case O::BitInsert: case O::BitClear:
        case O::TestBit: case O::TestZero: case O::TestNotZero:
            return true;
        default: return false;
    }
}
}

ScalarRegisterPlan ScalarRegisterPlan::Build(const ir::Block* block, bool has_function_plan, bool reserve_rbx) {
    if (has_function_plan) {
        ScalarRegisterPlan plan;
        plan.rejection = ScalarPlanRejection::FunctionPlan;
        return plan;
    }
    return BuildImpl(block, nullptr, reserve_rbx);
}

ScalarRegisterPlan ScalarRegisterPlan::BuildValidated(const ir::Block* block,
                                                     const FunctionConstraints& constraints, bool reserve_rbx) {
    if (!block || constraints.block != block) {
        ScalarRegisterPlan plan;
        plan.rejection = ScalarPlanRejection::FunctionPlan;
        return plan;
    }
    return BuildImpl(block, &constraints, reserve_rbx);
}

ScalarRegisterPlan ScalarRegisterPlan::BuildImpl(const ir::Block* block,
                                                const FunctionConstraints* constraints, bool reserve_rbx) {
    ScalarRegisterPlan plan;
    if (!block) return plan;
    std::vector<ir::Inst*> instructions;
    std::unordered_map<ir::Inst*, u32> positions;
    // Every control instruction is a sentinel with a distinct body on either
    // side. No runtime register reset is needed: cross-segment values never
    // receive a slot, and every body-local value executes its own definition
    // after entry before any cached use (including loop re-entry).
    std::vector<u32> segments;
    u32 segment{};
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() == O::AddPhi && !constraints) {
            plan.rejection = ScalarPlanRejection::ControlFlow; return plan;
        }
        const bool boundary = ControlBoundary(inst.GetOp()) ||
                (constraints && (inst.GetOp() == O::AddPhi || constraints->boundaries.contains(&inst)));
        if (boundary) ++segment;
        segments.push_back(segment);
        positions.emplace(&inst, instructions.size()); instructions.push_back(&inst);
        if (boundary) ++segment;
    }
    // Terminal reads belong to the final body, including an empty new tail
    // after a last control instruction. Earlier values then remain home-only.
    segments.push_back(segment);
    if (constraints) {
        for (auto* inst : constraints->home_only) if (!positions.contains(inst)) {
            plan.rejection = ScalarPlanRejection::InvalidSSA; return plan;
        }
        for (auto* inst : constraints->boundaries) if (!positions.contains(inst)) {
            plan.rejection = ScalarPlanRejection::InvalidSSA; return plan;
        }
    }
    std::vector<std::vector<u32>> uses(instructions.size());
    std::vector<bool> requires_home(instructions.size(), false);
    std::vector<bool> crosses_segment(instructions.size(), false);
    std::vector<ScalarCachePhase> last_use_phases(instructions.size(), ScalarCachePhase::Write);
    bool valid = true;
    const auto use = [&](ir::Value value, u32 position, ir::Inst* consumer, u32 slot,
                         bool scalar_terminal, bool validated_forward = true) {
        const auto found = positions.find(value.Def());
        // No dereference of a foreign or undefined producer.
        if (found == positions.end()) { valid = false; return; }
        if (value.Def()->ReturnType() == ir::ValueType::VOID || value.Type() == ir::ValueType::VOID) {
            valid = false; return;
        }
        const bool crosses = segments[found->second] != segments[position];
        if (found->second >= position) {
            // A validated CFG may place a dominating predecessor physically
            // later. Its value stays home-only; this never relaxes local-body
            // ordering or unverified block/extra-Phi-argument validation.
            if (!constraints || !crosses || !validated_forward) { valid = false; return; }
            crosses_segment[found->second] = requires_home[found->second] = true;
            return;
        }
        // Value::Type dereferences an uncast definition. Classify only after
        // proving the producer is a defined member with an earlier local use.
        const bool scalar_read = scalar_terminal ? ScalarType(value.Type()) :
                                 consumer && ScalarReadConsumer(consumer, slot, value);
        auto& producer_uses = uses[found->second];
        const auto phase = scalar_read ? ScalarCachePhase::Read : ScalarCachePhase::Write;
        if (producer_uses.empty() || producer_uses.back() != position)
            last_use_phases[found->second] = phase;
        else if (!scalar_read)
            last_use_phases[found->second] = ScalarCachePhase::Write;
        producer_uses.push_back(position);
        if (crosses)
            crosses_segment[found->second] = true;
        if (!scalar_read) requires_home[found->second] = true;
    };
    for (u32 position = 0; position < instructions.size(); ++position) {
        auto* inst = instructions[position];
        for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
            // BindLabel's first physical argument names a label, not an SSA
            // read. Forward/backedge targets may have VOID return type. Only
            // this identity slot is exempt; all extra physical/opaque values
            // retain the ordinary membership/dominance/segment checks.
            if (inst->GetOp() == O::BindLabel && slot == 0) continue;
            auto& arg = inst->ArgAt(slot);
            if (constraints && inst->GetOp() == O::AddPhi && slot == 0) {
                // ConfigurePhis validated predecessor reads and resolved the
                // copies. They are not SSA reads at this Phi's lexical point.
                if (!arg.IsParams() || !constraints->home_only.contains(inst)) { valid = false; continue; }
                for (const auto& param : arg.Get<ir::Params>())
                    if (param.data.IsValue() && !constraints->home_only.contains(param.data.value.Def())) valid = false;
                continue;
            }
            // ConfigurePhis skips label/Phi operands outside their canonical
            // slots. Those extra arguments must retain lexical validation.
            const bool validated_forward = inst->GetOp() != O::AddPhi && inst->GetOp() != O::BindLabel;
            if (arg.IsValue()) {
                const auto value = arg.Get<ir::Value>();
                use(value, position, inst, slot, false, validated_forward);
            }
            else if (arg.IsLambda()) {
                const auto lambda = arg.Get<ir::Lambda>();
                if (lambda.IsValue()) use(lambda.GetValue(), position, nullptr, 0, false, validated_forward);
            } else if (arg.IsParams()) {
                for (const auto& param : arg.Get<ir::Params>())
                    if (param.data.IsValue()) use(param.data.value, position, nullptr, 0, false, validated_forward);
            }
        }
    }
    const auto terminal_uses = [&](const auto& self, const ir::Terminal& terminal) -> void {
        VisitVariant<void>(terminal, [&](const auto& term) {
            using Term = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<Term, ir::terminal::If>) {
                use(term.cond, instructions.size(), nullptr, 0, true);
                self(self, term.then_); self(self, term.else_);
            } else if constexpr (std::is_same_v<Term, ir::terminal::Condition>) {
                self(self, term.then_); self(self, term.else_);
            } else if constexpr (std::is_same_v<Term, ir::terminal::Switch>) {
                use(term.value, instructions.size(), nullptr, 0, true);
                for (const auto& item : term.cases) self(self, item.then);
            } else if constexpr (std::is_same_v<Term, ir::terminal::CheckHalt>) self(self, term.else_);
        });
    };
    terminal_uses(terminal_uses, block->GetTerminal());
    if (!valid) { plan.rejection = ScalarPlanRejection::InvalidSSA; return plan; }
    plan.rejection = ScalarPlanRejection::None;
    std::array<std::optional<u32>, 3> occupant_storage{};
    const u32 first_slot = reserve_rbx ? 1 : 0;
    std::span<std::optional<u32>> occupants{occupant_storage.data() + first_slot, 3 - first_slot};
    u32 allocation_segment{};
    for (u32 position = 0; position < instructions.size(); ++position) {
        auto* inst = instructions[position];
        if (segments[position] != allocation_segment) {
            std::fill(occupants.begin(), occupants.end(), std::nullopt);
            allocation_segment = segments[position];
        }
        if (uses[position].empty() || crosses_segment[position] ||
            (constraints && constraints->home_only.contains(inst)) || !CacheableProducer(inst)) continue;
        for (auto& occupant : occupants)
            if (occupant) {
                const auto& interval = plan.intervals[*occupant];
                if (interval.last_use < position ||
                    (interval.last_use == position && interval.last_use_phase == ScalarCachePhase::Read))
                    occupant.reset();
            }
        auto available = std::find(occupants.begin(), occupants.end(), std::nullopt);
        if (available == occupants.end()) {
            // This is a static whole-interval choice, before any machine code
            // is emitted. A displaced producer uses its canonical home at
            // every definition/use; no run-time eviction or reload is needed.
            // Prefer short intervals when forwarding creates long live ranges
            // that would otherwise monopolize the three available registers.
            auto victim = occupants.begin();
            for (auto candidate = occupants.begin() + 1; candidate != occupants.end(); ++candidate)
                if (plan.intervals[**candidate].last_use > plan.intervals[**victim].last_use)
                    victim = candidate;
            auto& interval = plan.intervals[**victim];
            // Same-endpoint ties keep the existing assignment. The phase rule
            // above still governs reuse after an audited final input read.
            if (uses[position].back() >= interval.last_use) continue;
            interval.slot = u32(host_register_indices.size()); // planning tombstone
            available = victim;
        }
        const auto slot = first_slot + u32(available - occupants.begin());
        const auto index = u32(plan.intervals.size());
        plan.assignment_indices.emplace(inst, index);
        plan.intervals.push_back({inst, slot, position, uses[position].back(), last_use_phases[position],
                                  std::move(uses[position]), requires_home[position], segments[position]});
        *available = index;
    }
    // Occupants retain append-only indices throughout planning. Compact only
    // once all choices are final, then rebuild the lookup map from the stable
    // definition order so displaced values have no stale cache assignment.
    std::erase_if(plan.intervals, [](const auto& interval) {
        return interval.slot >= host_register_indices.size();
    });
    plan.assignment_indices.clear();
    for (u32 index = 0; index < plan.intervals.size(); ++index)
        plan.assignment_indices.emplace(plan.intervals[index].producer, index);
    return plan;
}

const ScalarCacheInterval* ScalarRegisterPlan::Find(ir::Inst* producer) const noexcept {
    const auto found = assignment_indices.find(producer);
    return found == assignment_indices.end() ? nullptr : &intervals[found->second];
}
std::optional<unsigned> ScalarRegisterPlan::Slot(ir::Inst* producer) const noexcept {
    if (const auto* interval = Find(producer)) return interval->slot;
    return std::nullopt;
}
bool ScalarRegisterPlan::RequiresCanonicalHome(ir::Inst* producer) const noexcept {
    if (const auto* interval = Find(producer)) return interval->requires_canonical_home;
    return true;
}
bool ScalarRegisterPlan::ValidateNonOverlapping() const noexcept {
    std::array<const ScalarCacheInterval*, 3> previous_intervals{};
    for (const auto& interval : intervals) {
        if (interval.slot >= previous_intervals.size() || interval.last_use <= interval.definition_position ||
            interval.use_positions.empty() || interval.use_positions.back() != interval.last_use)
            return false;
        if (const auto* previous = previous_intervals[interval.slot]) {
            if (previous->last_use > interval.definition_position ||
                (previous->last_use == interval.definition_position && previous->last_use_phase != ScalarCachePhase::Read))
                return false;
        }
        for (auto position : interval.use_positions)
            if (position <= interval.definition_position || position > interval.last_use) return false;
        previous_intervals[interval.slot] = &interval;
    }
    return true;
}
} // namespace swift::runtime::backend::x64
