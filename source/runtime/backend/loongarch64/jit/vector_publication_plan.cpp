#include "vector_publication_plan.h"

#include <algorithm>
#include <deque>
#include <stdexcept>

namespace swift::runtime::backend::loongarch64 {
namespace {
using O = ir::OpCode;
using Bits = std::vector<u64>;

bool Branch(ir::Inst* inst) { return inst->GetOp() == O::Goto || inst->GetOp() == O::NotGoto; }

void Set(Bits& bits, u32 index) { bits[index / 64] |= u64{1} << (index % 64); }
void Clear(Bits& bits, u32 index) { bits[index / 64] &= ~(u64{1} << (index % 64)); }
bool Test(const Bits& bits, u32 index) { return (bits[index / 64] & (u64{1} << (index % 64))) != 0; }

struct Effect {
    std::vector<u32> publications, dirty_definitions, clean_definitions, phi_definitions;
};

void Unique(std::vector<u32>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}
} // namespace

VectorPublicationPlan VectorPublicationPlan::Build(ir::Block* block, const VectorAllocationPlan& allocation,
        const RegisterPlan::Targets& targets, const RegisterPlan::Stops& stops,
        const RegisterPlan::Exits& exits) {
    if (!block) throw std::invalid_argument("LoongArch vector publication plan needs an IR block");
    VectorPublicationPlan result;
    std::vector<ir::Inst*> instructions;
    std::unordered_map<ir::Inst*, u32> bound;
    for (auto& inst : block->GetInstList()) {
        const u32 position = instructions.size();
        result.positions.emplace(&inst, position); instructions.push_back(&inst);
        if (inst.GetOp() == O::BindLabel && !bound.emplace(inst.GetArg<ir::Value>(0).Def(), position).second)
            throw std::invalid_argument("LoongArch vector publication plan has a duplicate label");
        if (allocation.Register(&inst)) result.values.emplace(&inst, result.values.size());
    }
    const u32 count = instructions.size(), value_count = result.values.size();
    result.stats.instructions = count; result.stats.mapped_values = value_count;
    if (!count || !value_count) { result.stats.accepted = true; return result; }
    const u32 words = (value_count + 63) / 64;
    // Rejection changes no IR or assignments and simply retains every store.
    constexpr u32 max_cells = 1u << 20;
    if (count > max_cells / words) return result;
    const auto position = [&](ir::Inst* inst) {
        const auto found = result.positions.find(inst);
        if (found == result.positions.end())
            throw std::invalid_argument("LoongArch vector publication plan has a foreign CFG coordinate");
        return found->second;
    };
    for (auto* inst : stops) (void)position(inst);
    for (const auto& exit : exits) (void)position(exit.first);
    for (const auto& [from, to] : targets) {
        if (!Branch(instructions[position(from)]))
            throw std::invalid_argument("LoongArch vector publication plan redirects a non-branch");
        (void)position(to);
    }
    std::vector<bool> leaders(count + 1);
    leaders.front() = leaders.back() = true;
    std::vector<u32> branch_targets(count, UINT32_MAX);
    for (u32 i = 0; i < count; ++i) {
        auto* inst = instructions[i];
        if (inst->GetOp() == O::BindLabel) leaders[i] = true;
        if (Branch(inst) || stops.contains(inst) || exits.contains(inst)) leaders[i + 1] = true;
        if (!Branch(inst)) continue;
        auto* anchor = targets.contains(inst) ? targets.at(inst) : inst;
        const auto found = bound.find(anchor);
        const u32 target = found != bound.end() ? found->second :
                anchor && anchor->GetOp() == O::BindLabel ? position(anchor) : UINT32_MAX;
        if (target == UINT32_MAX)
            throw std::invalid_argument("LoongArch vector publication plan has an unbound branch");
        branch_targets[i] = target; leaders[target] = true;
    }
    std::vector<std::vector<u32>> successors(count), predecessors(count);
    const auto edge = [&](u32 from, u32 to) {
        successors[from].push_back(to); predecessors[to].push_back(from);
        ++result.stats.cfg_edges;
    };
    for (u32 i = 0; i < count; ++i) {
        auto* inst = instructions[i];
        if (exits.contains(inst)) continue;
        if (Branch(inst)) edge(i, branch_targets[i]);
        if (!stops.contains(inst) && i + 1 < count) edge(i, i + 1);
    }
    result.reachable.resize(count);
    std::vector<u32> pending{0}; result.reachable.front() = true;
    for (u32 at = 0; at < pending.size(); ++at)
        for (auto next : successors[pending[at]]) if (!result.reachable[next]) {
            result.reachable[next] = true; pending.push_back(next);
        }
    result.stats.reachable = pending.size();

    std::vector<Effect> effects(count);
    const auto mapped = [&](std::vector<u32>& set, ir::Inst* value) {
        if (auto found = result.values.find(value); found != result.values.end()) set.push_back(found->second);
    };
    // Edge copies stage every source and then commit both destination parts
    // before entering the label. Generate destination cleanliness at that
    // entry, including host-entry constants and cyclic/backedge phis. Source
    // publication is conditional on the selected edge and is not an IR-wide
    // effect of Goto: ignoring its extra cleanliness avoids leaking facts onto
    // the bypassed path.
    for (u32 entry = 0; entry < count;) {
        u32 end = entry + 1;
        while (end < count && !leaders[end]) ++end;
        u32 first = entry + (instructions[entry]->GetOp() == O::BindLabel ? 1 : 0);
        while (first < end && instructions[first]->GetOp() == O::AddPhi) {
            mapped(effects[entry].phi_definitions, instructions[first]); ++first;
        }
        result.stats.phi_definitions += effects[entry].phi_definitions.size();
        entry = end;
    }
    for (u32 i = 0; i < count; ++i) {
        auto* inst = instructions[i]; auto& effect = effects[i];
        if (inst->GetOp() == O::Checkpoint) continue;
        if (allocation.WideTail(inst)) continue;
        if (allocation.RegionInstruction(inst)) {
            if (inst->GetOp() == O::VecShiftLeft || inst->GetOp() == O::VecShiftRight ||
                inst->GetOp() == O::VecShiftRightArithmetic)
                mapped(effect.publications, inst->GetArg<ir::Value>(1).Def());
            // A static Inst* in a loop produces a fresh dynamic result every
            // iteration. Both fused LASX halves are defined at the head; the
            // skipped tail marker must neither kill nor generate home facts.
            mapped(effect.dirty_definitions, inst);
            if (auto* pair = allocation.WideHead(inst)) mapped(effect.dirty_definitions, pair->high);
        } else {
            const bool preserves = VectorAllocationPlan::PreservesRegisters(inst);
            if (!preserves) {
                ++result.stats.canonical_boundaries;
                for (auto* value : allocation.At(inst)) mapped(effect.publications, value);
            } else if (inst->GetOp() != O::BindLabel && inst->GetOp() != O::AddPhi) {
                const bool direct_uniform = inst->GetOp() == O::StoreUniform &&
                        inst->GetArg<ir::Value>(1).Type() == ir::ValueType::V128 &&
                        allocation.Register(inst->GetArg<ir::Value>(1).Def()).has_value();
                if (!direct_uniform) for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
                    auto& arg = inst->ArgAt(slot);
                    if (arg.IsValue()) mapped(effect.publications, arg.Get<ir::Value>().Def());
                    else if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue())
                        mapped(effect.publications, arg.Get<ir::Lambda>().GetValue().Def());
                    else if (arg.IsParams()) for (const auto& parameter : arg.Get<ir::Params>())
                        if (parameter.data.IsValue()) mapped(effect.publications, parameter.data.value.Def());
                }
            }
            // Non-region mapped producers use canonical homes. The common
            // FinishInstruction reload follows any helper/FP/memory branches,
            // and its PublishInput also commits LocalCache GPR-produced parts.
            if (inst->GetOp() != O::AddPhi) mapped(effect.clean_definitions, inst);
        }
        Unique(effect.publications); Unique(effect.dirty_definitions); Unique(effect.clean_definitions);
        result.stats.native_definitions += effect.dirty_definitions.size();
    }

    result.before.assign(count, Bits(words)); result.after.assign(count, Bits(words));
    Bits host_dirty(words, ~u64{0});
    if (value_count % 64) host_dirty.back() = (u64{1} << (value_count % 64)) - 1;
    std::deque<u32> work{0};
    std::vector<bool> queued(count), initialized(count); queued.front() = true;
    u64 word_work{};
    constexpr u64 max_word_work = u64{1} << 26;
    while (!work.empty()) {
        const u32 i = work.front(); work.pop_front(); queued[i] = false;
        ++result.stats.iterations;
        word_work += u64(words) * (predecessors[i].size() + 2);
        if (word_work > max_word_work) return result;
        Bits incoming = i == 0 ? host_dirty : Bits(words);
        for (auto previous : predecessors[i]) if (result.reachable[previous])
            for (u32 word = 0; word < words; ++word) incoming[word] |= result.after[previous][word];
        const auto& effect = effects[i];
        for (auto value : effect.phi_definitions) Clear(incoming, value);
        result.before[i] = incoming;
        for (auto value : effect.publications) Clear(incoming, value);
        for (auto value : effect.clean_definitions) Clear(incoming, value);
        for (auto value : effect.dirty_definitions) Set(incoming, value);
        if (!initialized[i] || incoming != result.after[i]) {
            result.after[i] = std::move(incoming); initialized[i] = true;
            for (auto next : successors[i]) if (!queued[next]) { work.push_back(next); queued[next] = true; }
        }
    }
    for (u32 i = 0; i < count; ++i) if (result.reachable[i])
        for (auto value : effects[i].publications) {
            if (Test(result.before[i], value)) ++result.stats.dirty_publications;
            else ++result.stats.clean_publications;
        }
    result.stats.accepted = true;
    return result;
}

bool VectorPublicationPlan::Dirty(const std::vector<std::vector<u64>>& states,
        ir::Inst* inst, ir::Inst* value) const {
    if (!stats.accepted || !Reachable(inst)) return true;
    const auto found = values.find(value);
    if (found == values.end()) return true;
    return Test(states[positions.at(inst)], found->second);
}

bool VectorPublicationPlan::DirtyAt(ir::Inst* inst, ir::Inst* value) const { return Dirty(before, inst, value); }
bool VectorPublicationPlan::DirtyAfter(ir::Inst* inst, ir::Inst* value) const { return Dirty(after, inst, value); }

bool VectorPublicationPlan::Reachable(ir::Inst* inst) const {
    const auto found = positions.find(inst);
    return found != positions.end() && found->second < reachable.size() && reachable[found->second];
}

} // namespace swift::runtime::backend::loongarch64
