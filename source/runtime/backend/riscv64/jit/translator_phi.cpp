#include "translator.h"

#include <algorithm>
#include <stdexcept>

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;
using O = ir::OpCode;

void JitTranslator::ConfigurePhis() {
    struct Edge { ir::Inst* source; bool taken; u32 at; };
    std::vector<ir::Inst*> insts, phis;
    std::unordered_map<ir::Inst*, u32> positions, targets;
    for (auto& inst : block->GetInstList()) { positions[&inst] = insts.size(); insts.push_back(&inst); }
    if (insts.empty()) return;
    std::vector<bool> leaders(insts.size() + 1);
    leaders[0] = leaders.back() = true;
    for (u32 i = 0; i < insts.size(); ++i) {
        const auto op = insts[i]->GetOp();
        if (op == O::Goto || op == O::NotGoto || !context.FallsThrough(insts[i])) leaders[i + 1] = true;
        else if (op == O::BindLabel) { leaders[i] = true; targets[insts[i]->GetArg<ir::Value>(0).Def()] = i; }
    }
    for (auto* inst : insts) if (inst->GetOp() == O::Goto || inst->GetOp() == O::NotGoto)
        if (auto found = targets.find(context.BranchTarget(inst)); found != targets.end()) targets[inst] = found->second;
    std::vector<u32> starts, node(insts.size());
    for (u32 i = 0; i < insts.size(); ++i) {
        if (leaders[i]) starts.push_back(i);
        node[i] = starts.size() - 1;
    }
    const u32 count = starts.size(); starts.push_back(insts.size());
    std::vector<std::vector<Edge>> incoming(count);
    incoming[0].push_back({nullptr, false, 0});
    // Parameters follow incoming edges in source instruction order, with
    // fallthrough before taken when both edges have the same predecessor.
    for (u32 b = 0; b < count; ++b) {
        auto* last = insts[starts[b + 1] - 1];
        if (b + 1 < count && context.FallsThrough(last)) incoming[b + 1].push_back({last, false, starts[b + 1]});
        if (last->GetOp() == O::Goto || last->GetOp() == O::NotGoto) {
            if (!targets.contains(last)) throw std::runtime_error("RV64 phi CFG has an unbound edge");
            incoming[node[targets.at(last)]].push_back({last, true, starts[b + 1]});
        }
    }
    for (u32 b = 0; b < count; ++b) {
        u32 first = starts[b];
        if (insts[first]->GetOp() == O::BindLabel) ++first;
        u32 end = first;
        while (end < starts[b + 1] && insts[end]->GetOp() == O::AddPhi) ++end;
        for (u32 i = end; i < starts[b + 1]; ++i)
            if (insts[i]->GetOp() == O::AddPhi) throw std::runtime_error("RV64 phi must precede non-phi instructions at a CFG entry");
        for (u32 i = first; i < end; ++i) {
            auto* phi = insts[i];
            const u32 size = ir::GetValueSizeByte(phi->ReturnType());
            if (!size || size > 16) throw std::runtime_error("invalid RV64 phi width");
            std::vector<ir::DataClass> inputs;
            for (const auto& param : phi->GetArg<ir::Params>(0)) {
                if (param.data.IsValue()) {
                    auto* definition = param.data.value.Def();
                    if (!positions.contains(definition) || ir::GetValueSizeByte(param.data.value.Type()) != size)
                        throw std::runtime_error("RV64 phi input must have the result width and a local definition");
                } else if (!param.data.IsImm()) throw std::runtime_error("invalid RV64 phi input");
                inputs.push_back(param.data);
            }
            if (inputs.size() != incoming[b].size()) throw std::runtime_error("RV64 phi input count differs from incoming edge count");
            phis.push_back(phi);
            for (u32 p = 0; p < inputs.size(); ++p) {
                const auto& edge = incoming[b][p];
                if (inputs[p].IsValue()) context.AddPhiEdgeUse(inputs[p].value, edge.at);
                PhiMove move{phi, inputs[p]};
                if (!edge.source) {
                    if (inputs[p].IsValue()) throw std::runtime_error("RV64 entry phi requires an immediate for the host-entry edge");
                    entry_phis.push_back(move);
                } else if (edge.taken) taken_phis[edge.source].push_back(move);
                else fallthrough_phis[insts[starts[b]]].push_back(move);
            }
        }
    }
    context.ConfigurePhiRegisters(phis);
}

bool JitTranslator::EmitPhi(ir::Inst* inst) {
    switch (inst->GetOp()) {
        case O::AddPhi:
            // Every incoming edge assigned its result in parallel. The marker
            // itself neither selects an arbitrary input nor destroys that value.
            return true;
        default: return false;
    }
}

void JitTranslator::EmitPhiMoves(const std::vector<PhiMove>& moves) {
    auto pending = moves;
    std::erase_if(pending, [](const auto& move) { return move.source.IsValue() && move.source.value.Def() == move.destination; });
    auto& as = context.GetMasm();
    ir::Inst* saved{};
    const auto before = context.CurrentBufferSize();
    while (!pending.empty()) {
        auto ready = std::find_if(pending.begin(), pending.end(), [&](const auto& candidate) {
            return std::none_of(pending.begin(), pending.end(), [&](const auto& other) {
                return other.source.IsValue() && other.source.value.Def() == candidate.destination && other.source.value.Def() != saved;
            });
        });
        if (ready == pending.end()) {
            // Break one copy cycle with two caller-saved GPRs, independent of
            // the number of phis. No transient stack allocation is needed.
            ASSERT(!saved); saved = pending.front().destination;
            context.ReadPart(a6, ir::Value{saved}, 0); context.ReadPart(a7, ir::Value{saved}, 1);
            continue;
        }
        const auto move = *ready;
        const u32 size = ir::GetValueSizeByte(move.destination->ReturnType());
        if (move.source.IsValue() && move.source.value.Def() != saved && context.AssignPhiValue(move.destination, move.source.value)) {
            pending.erase(ready);
            continue;
        }
        if (move.source.IsImm()) { as.LI(t0, move.source.imm.Get()); as.MV(t1, x0); }
        else if (move.source.value.Def() == saved) { as.MV(t0, a6); as.MV(t1, a7); }
        else { context.ReadPart(t0, move.source.value, 0); if (size > 8) context.ReadPart(t1, move.source.value, 1); }
        if (size <= 8) { context.Mask(t0, size * 8); as.MV(t1, x0); }
        context.AssignPhi(move.destination, t0, t1);
        pending.erase(ready);
        if (saved && std::none_of(pending.begin(), pending.end(), [&](const auto& item) {
            return item.source.IsValue() && item.source.value.Def() == saved;
        })) saved = nullptr;
    }
    stats.bytes[size_t(O::AddPhi)] += context.CurrentBufferSize() - before;
}

}  // namespace swift::runtime::backend::riscv64
