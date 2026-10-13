#include "translator.h"

#include <algorithm>
#include <stdexcept>

namespace swift::runtime::backend::loongarch64 {
void JitTranslator::ConfigurePhis() {
    using O = ir::OpCode;
    struct Edge { ir::Inst* source; bool taken; };
    std::vector<ir::Inst*> insts;
    std::unordered_map<ir::Inst*, u32> positions, targets;
    for (auto& inst : block->GetInstList()) { positions[&inst] = insts.size(); insts.push_back(&inst); }
    if (insts.empty()) return;
    std::vector<bool> leaders(insts.size() + 1); leaders.front() = leaders.back() = true;
    for (u32 i = 0; i < insts.size(); ++i) {
        const auto op = insts[i]->GetOp();
        if (op == O::Goto || op == O::NotGoto || !context.FallsThrough(insts[i])) leaders[i + 1] = true;
        if (op == O::BindLabel) { leaders[i] = true; targets[insts[i]->GetArg<ir::Value>(0).Def()] = i; }
    }
    for (auto* inst : insts) if (inst->GetOp() == O::Goto || inst->GetOp() == O::NotGoto) {
        if (!targets.contains(context.BranchTarget(inst))) throw std::runtime_error("LoongArch phi edge is not bound");
        targets[inst] = targets.at(context.BranchTarget(inst));
    }
    std::vector<u32> starts, nodes(insts.size());
    for (u32 i = 0; i < insts.size(); ++i) {
        if (leaders[i]) starts.push_back(i);
        nodes[i] = starts.size() - 1;
    }
    const u32 count = starts.size(); starts.push_back(insts.size());
    std::vector<std::vector<Edge>> incoming(count); incoming[0].push_back({nullptr, false});
    for (u32 b = 0; b < count; ++b) {
        auto* last = insts[starts[b + 1] - 1];
        if (b + 1 < count && context.FallsThrough(last)) incoming[b + 1].push_back({last, false});
        if (last->GetOp() == O::Goto || last->GetOp() == O::NotGoto)
            incoming[nodes[targets.at(last)]].push_back({last, true});
    }
    for (u32 b = 0; b < count; ++b) {
        u32 first = starts[b];
        if (insts[first]->GetOp() == O::BindLabel) ++first;
        u32 end = first;
        while (end < starts[b + 1] && insts[end]->GetOp() == O::AddPhi) ++end;
        for (u32 i = end; i < starts[b + 1]; ++i) if (insts[i]->GetOp() == O::AddPhi)
            throw std::runtime_error("LoongArch phi must be at a CFG entry");
        for (u32 i = first; i < end; ++i) {
            auto* phi = insts[i]; const u32 size = ir::GetValueSizeByte(phi->ReturnType());
            if (!size || size > 16) throw std::runtime_error("invalid LoongArch phi width");
            std::vector<ir::DataClass> inputs;
            for (const auto& param : phi->GetArg<ir::Params>(0)) {
                if (param.data.IsValue() && (!positions.contains(param.data.value.Def()) ||
                    ir::GetValueSizeByte(param.data.value.Type()) != size))
                    throw std::runtime_error("LoongArch phi has a foreign or mismatched input");
                if (!param.data.IsValue() && !param.data.IsImm()) throw std::runtime_error("invalid LoongArch phi input");
                inputs.push_back(param.data);
            }
            if (inputs.size() != incoming[b].size()) throw std::runtime_error("LoongArch phi input count differs from incoming edges");
            for (u32 p = 0; p < inputs.size(); ++p) {
                const auto edge = incoming[b][p]; PhiMove move{phi, inputs[p]};
                if (!edge.source) {
                    if (inputs[p].IsValue()) throw std::runtime_error("LoongArch entry phi needs a constant for host entry");
                    entry_phis.push_back(move);
                } else if (edge.taken) taken_phis[edge.source].push_back(move);
                else fallthrough_phis[insts[starts[b]]].push_back(move);
            }
        }
    }
}

void JitTranslator::EmitAddPhi(ir::Inst*) {}

void JitTranslator::EmitPhiMoves(const std::vector<PhiMove>& moves) {
    if (moves.empty()) return;
    context.FlushValues(); context.DiscardValues();
    auto& as = context.GetMasm();
    const auto temporary_size = u64(moves.size()) * 16;
    as.LI(t0, temporary_size); as.sub_d(sp, sp, t0);
    // Stage every source before committing any destination. This implements
    // parallel copies, including cycles and vector phis, without aliasing.
    for (u32 i = 0; i < moves.size(); ++i) {
        const auto& move = moves[i];
        if (move.source.IsValue()) {
            context.ReadPart(t0, move.source.value, 0);
            if (ir::GetValueSizeByte(move.source.value.Type()) > 8) context.ReadPart(t1, move.source.value, 1);
            else as.MV(t1, zero);
        } else { as.LI(t0, move.source.imm.Get()); as.MV(t1, zero); }
        const auto size = ir::GetValueSizeByte(move.destination->ReturnType());
        if (size < 8) context.Mask(t0, size * 8);
        context.Store(t0, sp, i * 16); context.Store(t1, sp, i * 16 + 8);
    }
    for (u32 i = 0; i < moves.size(); ++i) {
        context.Load(t0, sp, i * 16); context.Load(t1, sp, i * 16 + 8);
        context.WriteRawPart(moves[i].destination, t0, 0);
        context.WriteRawPart(moves[i].destination, t1, 1);
    }
    as.LI(t0, temporary_size); as.add_d(sp, sp, t0);
}
}  // namespace swift::runtime::backend::loongarch64
