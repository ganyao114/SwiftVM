#include "retirement_phase_plan.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace swift::runtime::backend::riscv64 {

namespace {
using O = ir::OpCode;

[[noreturn]] void Reject(const char* reason) {
    throw std::runtime_error(std::string("RV64 retirement phase plan rejected: ") + reason);
}

struct LocationWrites {
    bool unknown{};
    std::set<u64> constants;
    bool Merge(const LocationWrites& other) {
        const auto old_size = constants.size();
        const auto old_unknown = unknown;
        constants.insert(other.constants.begin(), other.constants.end());
        unknown |= other.unknown;
        return constants.size() != old_size || unknown != old_unknown;
    }
};

void Hash(u64& hash, u64 value) {
    for (u32 part = 0; part < 8; ++part) {
        hash = (hash ^ (value & 255)) * 1099511628211ULL;
        value >>= 8;
    }
}
}

const RetirementPhaseHead* RetirementPhasePlan::Find(const ir::Inst* label) const {
    const auto found = std::find_if(
            heads.begin(), heads.end(), [&](const auto& head) { return head.label == label; });
    return found == heads.end() ? nullptr : &*found;
}

RetirementPhasePlan PrepareRetirementPhases(ir::Block& block, const JitContext& context,
        bool has_phi_moves, bool has_function_flow) {
    RetirementPhasePlan plan;
    std::vector<const ir::Inst*> insts;
    std::unordered_map<const ir::Inst*, u32> positions, labels;
    std::vector<u64> lexical_pc;
    u64 pc = block.GetStartLocation().Value();
    plan.ir_fingerprint = 1469598103934665603ULL;
    Hash(plan.ir_fingerprint, pc);
    for (auto& inst : block.GetInstList()) {
        positions.emplace(&inst, insts.size());
        insts.push_back(&inst);
        lexical_pc.push_back(pc);
        Hash(plan.ir_fingerprint, inst.Id());
        Hash(plan.ir_fingerprint, static_cast<u64>(inst.GetOp()));
        Hash(plan.ir_fingerprint, static_cast<u64>(inst.ReturnType()));
        if (inst.GetOp() == O::BindLabel)
            labels.emplace(inst.GetArg<ir::Value>(0).Def(), insts.size() - 1);
        if (inst.GetOp() == O::AdvancePC) {
            const auto size = inst.GetArg<ir::Imm>(0).Get();
            Hash(plan.ir_fingerprint, size);
            if (!size || size > std::numeric_limits<u64>::max() - pc) Reject("invalid-advance-pc");
            pc += size;
        }
        if (inst.GetOp() == O::SetLocation) {
            const auto target = inst.GetArg<ir::Lambda>(0);
            Hash(plan.ir_fingerprint, target.IsValue() ? UINT64_MAX : target.GetImm().Get());
        }
    }
    if (insts.empty()) return plan;
    const u32 count = insts.size();
    std::vector<std::vector<u32>> successors(count);
    std::vector<std::pair<u32, u32>> backedges;
    for (u32 i = 0; i < count; ++i) {
        if (i + 1 < count && context.FallsThrough(insts[i])) successors[i].push_back(i + 1);
        const auto op = insts[i]->GetOp();
        if (op != O::Goto && op != O::NotGoto) continue;
        const auto found = labels.find(context.BranchTarget(insts[i]));
        if (found == labels.end()) Reject("missing-label");
        successors[i].push_back(found->second);
        Hash(plan.ir_fingerprint, i);
        Hash(plan.ir_fingerprint, found->second);
        if (found->second <= i) backedges.emplace_back(i, found->second);
    }
    std::vector<bool> reachable(count);
    std::deque<u32> work{0};
    reachable[0] = true;
    while (!work.empty()) {
        const auto at = work.front(); work.pop_front();
        for (const auto next : successors[at]) if (!reachable[next]) {
            reachable[next] = true;
            work.push_back(next);
        }
    }
    plan.reachable_instructions = std::count(reachable.begin(), reachable.end(), true);
    const bool needs_internal_exit = std::any_of(backedges.begin(), backedges.end(), [&](const auto& edge) {
        return reachable[edge.first] && !context.IsGuestEntryLabel(insts[edge.second]);
    });
    if (!needs_internal_exit) return plan;
    if (context.Features().vector) Reject("rvv-outside-slice");
    if (has_phi_moves) Reject("phi-state");
    if (has_function_flow) Reject("function-flow");
    for (u32 i = 0; i < count; ++i) if (reachable[i]) {
        const auto type = insts[i]->ReturnType();
        if (ir::IsFloatValueType(type) && type != ir::ValueType::V32 &&
            type != ir::ValueType::V64 && type != ir::ValueType::V128) Reject("unsupported-typed-span");
        switch (insts[i]->GetOp()) {
            case O::AddPhi: Reject("phi-state");
            case O::PushRSB: case O::PopRSB: case O::CallReturn: Reject("call-frame");
            default: break;
        }
    }
    // Decode phase is a CFG property. AdvancePC changes that abstract cursor,
    // not State.current_loc. Every reachable predecessor must agree with it.
    for (u32 i = 0; i < count; ++i) if (reachable[i]) {
        auto outgoing_pc = lexical_pc[i];
        if (insts[i]->GetOp() == O::AdvancePC) outgoing_pc += insts[i]->GetArg<ir::Imm>(0).Get();
        for (const auto next : successors[i])
            if (outgoing_pc != lexical_pc[next]) Reject("phase-edge-conflict");
    }
    std::vector<LocationWrites> writes(count);
    std::vector<bool> queued(count), visited(count);
    work = {0}; queued[0] = true; visited[0] = true;
    while (!work.empty()) {
        const auto at = work.front(); work.pop_front(); queued[at] = false;
        auto outgoing = writes[at];
        if (insts[at]->GetOp() == O::SetLocation) {
            outgoing = {};
            const auto target = insts[at]->GetArg<ir::Lambda>(0);
            if (target.IsValue()) outgoing.unknown = true;
            else outgoing.constants.insert(target.GetImm().Get());
        }
        for (const auto next : successors[at]) {
            const auto changed = writes[next].Merge(outgoing);
            if ((!visited[next] || changed) && !queued[next]) {
                work.push_back(next); queued[next] = true;
            }
            visited[next] = true;
        }
    }
    for (const auto& [from, to] : backedges) {
        if (!reachable[from] || context.IsGuestEntryLabel(insts[to])) continue;
        if (context.IsSyntheticLabel(insts[to])) Reject("synthetic-label");
        if (writes[to].unknown) Reject("unknown-location-at-head");
        for (const auto value : writes[to].constants)
            if (value != lexical_pc[to]) Reject("location-phase-conflict");
        const auto& live = context.LiveAt(insts[to]);
        auto edge_live = context.LiveForBranch(insts[from]);
        const auto by_id = [](auto* a, auto* b) { return a->Id() < b->Id(); };
        std::vector<const ir::Inst*> ordered(live.begin(), live.end());
        std::sort(ordered.begin(), ordered.end(), by_id);
        std::sort(edge_live.begin(), edge_live.end(), by_id);
        if (ordered != edge_live) Reject("edge-live-mismatch");
        auto found = std::find_if(plan.heads.begin(), plan.heads.end(), [&](const auto& head) { return head.label == insts[to]; });
        if (found == plan.heads.end()) {
            RetirementPhaseHead head{insts[to], lexical_pc[to], {}, {}};
            for (auto* value : ordered) {
                if (!positions.contains(value) || value->ReturnType() == ir::ValueType::VOID)
                    Reject("unsupported-live-value");
                if (value->Id() > UINT32_MAX / kValueStride) Reject("live-home-overflow");
                const auto bytes = ir::GetValueSizeByte(value->ReturnType());
                const auto parts = JitContext::ZeroHigh(value) ? 1u : 2u;
                if (!bytes || bytes > kValueStride) Reject("unsupported-typed-span");
                head.live.push_back({value, value->ReturnType(), value->Id() * kValueStride, bytes, parts});
                Hash(plan.ir_fingerprint, value->Id());
                Hash(plan.ir_fingerprint, static_cast<u64>(value->ReturnType()));
                Hash(plan.ir_fingerprint, bytes);
                Hash(plan.ir_fingerprint, parts);
            }
            plan.heads.push_back(std::move(head));
            found = std::prev(plan.heads.end());
        }
        found->backedges.push_back(insts[from]);
    }
    std::sort(plan.heads.begin(), plan.heads.end(), [&](const auto& a, const auto& b) {
        return positions.at(a.label) < positions.at(b.label);
    });
    return plan;
}

}  // namespace swift::runtime::backend::riscv64
