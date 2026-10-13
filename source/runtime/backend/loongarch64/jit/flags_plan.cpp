#include "flags_plan.h"
#include "sparse_homes_plan.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace swift::runtime::backend::loongarch64 {
namespace {
using O = ir::OpCode;
using F = ir::Flags;

bool ScalarNative(O op) {
    switch (op) {
#define NATIVE(name) case O::name: return true;
#include "scalar_native.inc"
#undef NATIVE
        default: return false;
    }
}
bool OptionalNative(O op) {
    switch (op) {
#define NATIVE(name) case O::name: return true;
#include "vector_native.inc"
#include "float_native.inc"
#undef NATIVE
        default: return false;
    }
}
bool TypedHelper(O op) {
    switch (op) {
#define FALLBACK(name) case O::name: return true;
#include "fallback.inc"
#undef FALLBACK
        default: return false;
    }
}

F SaveWrites(ir::Inst* inst) {
    auto* producer = inst->GetArg<ir::Value>(0).Def();
    if (!producer) throw std::runtime_error("LoongArch flags plan has an undefined producer");
    const u32 bits = ir::GetValueSizeByte(producer->ReturnType()) * 8;
    if (!bits || bits > 64) return F::None;
    const auto requested = inst->GetArg<F>(1) & F::All;
    F writes = requested & (F::NZ | F::Parity);
    switch (producer->GetOp()) {
        case O::Add: case O::Adc: case O::Sub: case O::Sbb: case O::Neg:
            writes |= requested & (F::CV | F::AuxiliaryCarry); break;
        case O::And: case O::Or: case O::Xor: case O::AndNot:
            writes |= requested & F::CV; break;
        case O::Mul:
            if (bits < 64) writes |= requested & F::CV;
            break;
        default: break;
    }
    return writes;
}

bool Branch(O op) { return op == O::Goto || op == O::NotGoto; }
}  // namespace

ir::Flags FlagsPlan::ConditionDemand(ir::Cond condition) {
    switch (condition) {
        case ir::Cond::EQ: case ir::Cond::NE: return F::Zero;
        case ir::Cond::CS: case ir::Cond::CC: return F::Carry;
        case ir::Cond::MI: case ir::Cond::PL: return F::Negate;
        case ir::Cond::VS: case ir::Cond::VC: return F::Overflow;
        case ir::Cond::HI: case ir::Cond::LS: return F::Carry | F::Zero;
        case ir::Cond::GE: case ir::Cond::LT: return F::Negate | F::Overflow;
        case ir::Cond::GT: case ir::Cond::LE: return F::Negate | F::Overflow | F::Zero;
        case ir::Cond::AL: case ir::Cond::NV: return F::None;
    }
    return F::All;
}

FlagsEffect FlagsPlan::Effect(ir::Inst* inst) {
    if (!inst) throw std::runtime_error("LoongArch flags plan has a null instruction");
    FlagsEffect effect;
    const auto replace = [&](F bits) {
        effect.writes = effect.replaces = bits & F::All;
    };
    switch (inst->GetOp()) {
        case O::SaveFlags: case O::BranchOnlyFlags:
            replace(SaveWrites(inst));
            // Unlike ADC's value calculation, SaveFlags recomputes carry at
            // its own position. An intervening SetCarry changes this input.
            if (True(effect.writes & F::Carry)) {
                const auto producer = inst->GetArg<ir::Value>(0).Def()->GetOp();
                if (producer == O::Adc || producer == O::Sbb) effect.reads = F::Carry;
            }
            return effect;
        case O::ClearFlags: replace(inst->GetArg<F>(0)); return effect;
        case O::SetCarry: replace(F::Carry); return effect;
        case O::SetOverflow: replace(F::Overflow); return effect;
        case O::InvertCarry: replace(F::Carry); effect.reads = F::Carry; return effect;
        case O::PublishFCmpFlags: replace(F::All); return effect;
        case O::PublishSse42StrFlags: replace(inst->GetArg<F>(1)); return effect;
        case O::GetFlags:
            // The current IR returns the complete canonical word regardless
            // of its annotation mask. Preserve that observation contract.
            effect.reads = F::All; return effect;
        case O::TestFlags: case O::TestNotFlags:
            effect.reads = inst->GetArg<F>(0) & F::All; return effect;
        case O::CondSet: case O::LocalCondSet: case O::CondSelect:
            effect.reads = ConditionDemand(inst->GetArg<ir::Cond>(0)); return effect;
        case O::Adc: case O::Sbb:
            effect.reads = F::Carry; return effect;
        case O::LoadMemory: case O::LoadMemoryTSO:
        case O::StoreMemory: case O::StoreMemoryTSO:
        case O::AtomicExchange: case O::AtomicFetchAdd:
        case O::AtomicRMW: case O::CompareAndSwap:
            // Native memory publishes before AcquireMemory/GuestAddress and
            // the marked fault. Native/slow atomic paths both finish synced.
            effect.observes_before = effect.synchronized_after = effect.exits_after = true;
            return effect;
        case O::CheckMemoryAlignment:
            effect.exits_after = true; return effect;
        case O::LoadUniform: case O::StoreUniform: {
            const auto type = inst->GetOp() == O::LoadUniform ? inst->ReturnType() :
                    inst->GetArg<ir::Value>(1).Type();
            if (!ir::GetValueSizeByte(type) || ir::GetValueSizeByte(type) > 16)
                effect.observes_before = effect.synchronized_after = effect.exits_after = true;
            return effect;
        }
        case O::Checkpoint: case O::BindLabel: case O::Goto: case O::NotGoto:
            // Poll/CheckHalt can return with the incoming flags. Synthetic
            // labels and guest Goto can be overestimated safely.
            effect.exits_before = true; return effect;
        case O::AdvancePC: case O::AddPhi: case O::Nop: case O::UniformBarrier: case O::XchgBarrier:
        case O::PushRSB: case O::PopRSB: case O::CallReturn:
        case O::SetLocation: case O::GetLocation: case O::MemoryBarrierTSO:
            return effect;
        default: break;
    }
    if (TypedHelper(inst->GetOp())) {
        // The audited common helper sequence publishes, invokes its typed
        // kernel, reloads canonical flags and then checks halt.
        effect.observes_before = effect.synchronized_after = effect.exits_after = true;
    } else if (ScalarNative(inst->GetOp()) || OptionalNative(inst->GetOp())) {
        // Guarded SIMD/FP and out-of-width scalar operations can choose a
        // helper on only one path. They do not guarantee synchronization of
        // the native path, so never kill may-dirty state at the join.
        if (SparseHomesPlan::MayCallTypedHelper(inst)) {
            effect.observes_before = effect.exits_after = true;
        }
    } else {
        // A new or opaque lowering cannot prove a clean canonical word.
        // Retain all demand and dirty bits until a known synchronization.
        effect.reads = effect.writes = F::All;
        effect.observes_before = effect.exits_before = effect.exits_after = effect.opaque = true;
    }
    return effect;
}

FlagsPlan FlagsPlan::Build(ir::Block* block, const Targets& targets,
                           const Stops& stops, const Exits& exits,
                           const Instructions& halt_checks) {
    if (!block) throw std::runtime_error("LoongArch flags plan has a null block");
    FlagsPlan plan;
    std::vector<ir::Inst*> insts;
    std::unordered_map<ir::Inst*, u32> positions, bound;
    for (auto& inst : block->GetInstList()) {
        const u32 index = insts.size();
        positions.emplace(&inst, index); insts.push_back(&inst);
        if (inst.GetOp() == O::BindLabel && !bound.emplace(inst.GetArg<ir::Value>(0).Def(), index).second)
            throw std::runtime_error("LoongArch flags plan has a duplicate label");
    }
    const u32 count = insts.size();
    plan.stats.instructions = count;
    if (!count) return plan;
    const auto position = [&](ir::Inst* inst) {
        const auto found = positions.find(inst);
        if (found == positions.end()) throw std::runtime_error("LoongArch flags plan has a foreign CFG coordinate");
        return found->second;
    };
    for (auto* inst : stops) (void)position(inst);
    for (auto* inst : halt_checks) (void)position(inst);
    for (const auto& [inst, terminal] : exits) (void)position(inst);
    for (const auto& [from, to] : targets) {
        if (!Branch(insts[position(from)]->GetOp()))
            throw std::runtime_error("LoongArch flags plan redirects a non-branch");
        (void)position(to);
    }

    std::vector<std::vector<u32>> successors(count), predecessors(count);
    std::vector<bool> terminal_before(count), terminal_after(count);
    const auto edge = [&](u32 from, u32 to) {
        successors[from].push_back(to); predecessors[to].push_back(from);
    };
    for (u32 i = 0; i < count; ++i) {
        auto* inst = insts[i];
        auto effect = Effect(inst);
        if (halt_checks.contains(inst)) effect.exits_before = true;
        plan.effects.emplace(inst, effect);
        if (exits.contains(inst)) { terminal_before[i] = true; continue; }
        if (Branch(inst->GetOp())) {
            auto* anchor = inst;
            if (auto found = targets.find(inst); found != targets.end()) anchor = found->second;
            const auto found = bound.find(anchor);
            const u32 target = found != bound.end() ? found->second :
                    anchor && anchor->GetOp() == O::BindLabel ? position(anchor) : count;
            if (target == count) throw std::runtime_error("LoongArch flags plan has an unbound branch");
            edge(i, target);
        }
        if (!stops.contains(inst)) {
            if (i + 1 < count) edge(i, i + 1);
            else terminal_after[i] = true;
        } else if (!Branch(inst->GetOp())) terminal_after[i] = true;
    }
    std::vector<bool> reachable(count);
    std::vector<u32> pending{0}; reachable[0] = true;
    while (!pending.empty()) {
        const auto node = pending.back(); pending.pop_back();
        for (const auto next : successors[node]) if (!reachable[next]) {
            reachable[next] = true; pending.push_back(next);
        }
    }
    std::vector<F> before(count, F::None), after(count, F::None);
    bool changed;
    do {
        changed = false; ++plan.stats.dirty_iterations;
        for (u32 i = 0; i < count; ++i) {
            if (!reachable[i]) continue;
            F incoming = F::None;
            for (auto previous : predecessors[i]) if (reachable[previous]) incoming |= after[previous];
            const auto& effect = plan.effects.at(insts[i]);
            const F outgoing = effect.synchronized_after ? F::None : incoming | effect.writes;
            if (before[i] != incoming || after[i] != outgoing) {
                before[i] = incoming; after[i] = outgoing; changed = true;
            }
        }
    } while (changed);

    std::vector<F> needed_before(count, F::None), needed_after(count, F::None);
    do {
        changed = false; ++plan.stats.demand_iterations;
        for (u32 reverse = count; reverse; --reverse) {
            const u32 i = reverse - 1;
            if (!reachable[i]) continue;
            const auto& effect = plan.effects.at(insts[i]);
            F outgoing = terminal_after[i] || effect.exits_after ? F::All : F::None;
            for (const auto next : successors[i]) outgoing |= needed_before[next];
            F reads = effect.reads;
            // These operations read carry only to produce their carry flag.
            // When that write is dead, the demand-enabled emitter omits the
            // operation and its input cannot keep an earlier carry write live.
            // ADC/SBB value instructions still unconditionally consume carry.
            const auto op = insts[i]->GetOp();
            if ((op == O::InvertCarry || op == O::SaveFlags || op == O::BranchOnlyFlags) &&
                !True(outgoing & effect.writes & F::Carry)) {
                reads &= ~F::Carry;
            }
            F incoming = reads | (outgoing & ~effect.replaces);
            if (terminal_before[i] || effect.observes_before || effect.exits_before) incoming |= F::All;
            if (needed_before[i] != incoming || needed_after[i] != outgoing) {
                needed_before[i] = incoming; needed_after[i] = outgoing; changed = true;
            }
        }
    } while (changed);
    for (u32 i = 0; i < count; ++i) {
        auto* inst = insts[i];
        plan.before.emplace(inst, before[i]); plan.after.emplace(inst, after[i]);
        plan.needed_before.emplace(inst, needed_before[i]); plan.needed_after.emplace(inst, needed_after[i]);
        if (!reachable[i]) continue;
        plan.reachable.insert(inst); ++plan.stats.reachable;
        const auto& effect = plan.effects.at(inst);
        if (effect.observes_before) {
            if (True(before[i])) ++plan.stats.dirty_boundaries;
            else ++plan.stats.clean_boundaries;
        }
        if (effect.opaque) ++plan.stats.opaque;
        if (terminal_before[i] || effect.exits_before) {
            plan.exit_dirty |= before[i]; ++plan.stats.exit_paths;
        }
        if (terminal_after[i] || effect.exits_after) {
            plan.exit_dirty |= after[i]; ++plan.stats.exit_paths;
        }
    }
    return plan;
}

ir::Flags FlagsPlan::At(ir::Inst* inst) const {
    const auto value = before.at(inst);
    return Reachable(inst) ? value : F::All;
}
ir::Flags FlagsPlan::After(ir::Inst* inst) const {
    const auto value = after.at(inst);
    return Reachable(inst) ? value : F::All;
}

}  // namespace swift::runtime::backend::loongarch64
