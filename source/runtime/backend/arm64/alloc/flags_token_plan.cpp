#include "flags_token_plan.h"
#include "gpr_block_plan.h"
#include <array>

namespace swift::runtime::backend::arm64 {
namespace {
ir::Value Resolve(ir::Value value) {
    while (value.Defined() && value.Def()->IsBitCastOperation())
        value = value.Def()->GetArg<ir::Value>(0);
    return value;
}
bool Shares(const RegAlloc& allocation, ir::Value a, ir::Value b, u32 point) {
    if (allocation.PreservedGPRDefinitionAt(a.Id()) || allocation.PreservedGPRDefinitionAt(b.Id())) {
        const auto x = allocation.GPRLocationAt(a, point, RegAlloc::GPRView::WholeValue);
        const auto y = allocation.GPRLocationAt(b, point, RegAlloc::GPRView::WholeValue);
        return x.kind == RegAlloc::GPR && y.kind == RegAlloc::GPR && x.slot == y.slot;
    }
    return allocation.ValueType(a) == RegAlloc::GPR && allocation.ValueType(b) == RegAlloc::GPR &&
           allocation.ValueGPR(a).id == allocation.ValueGPR(b).id;
}
bool Wrapper(const RegAlloc& allocation, ir::Inst* producer, ir::Inst* wrapper, u32 point) {
    if (ir::GetValueSizeByte(producer->ReturnType()) != 4 || !wrapper ||
        wrapper->GetOp() != ir::OpCode::ZeroExtend32To64) return false;
    const auto source = Resolve(wrapper->GetArg<ir::Value>(0));
    return source.Def() == producer && Shares(allocation, source, ir::Value{wrapper}, point);
}
bool SavesParity(ir::Inst* producer, const BlockAnalysisIndex& analysis, const PseudoFlagsPlan* demands) {
    if (demands) return demands->SavesParity(producer);
    bool saves = false;
    analysis.ForEachPseudoUse(producer, [&](const ir::Inst& pseudo) {
        saves |= pseudo.GetOp() == ir::OpCode::SaveFlags &&
                True(pseudo.GetArg<ir::Flags>(1) & ir::Flags::Parity);
    });
    return saves;
}
bool CapturesParity(ir::Inst* producer, const BlockAnalysisIndex& analysis, const PseudoFlagsPlan* demands) {
    if (SavesParity(producer, analysis, demands)) return true;
    // Only funnel OR results can be captured by a later parity OR, possibly
    // through an AND mask. This is demand selection, not fusion discovery.
    if (producer->GetOp() != ir::OpCode::Or) return false;
    for (const auto& use : analysis.Uses(producer)) {
        if (use.consumer->GetOp() == ir::OpCode::Or && SavesParity(use.consumer, analysis, demands)) return true;
        if (use.consumer->GetOp() == ir::OpCode::And)
            for (const auto& next : analysis.Uses(use.consumer))
                if (next.consumer->GetOp() == ir::OpCode::Or && SavesParity(next.consumer, analysis, demands)) return true;
    }
    return false;
}
}

FlagsTokenPlan BuildFlagsTokenPlan(const BlockAnalysisIndex& analysis,
        const RegAlloc& allocation, const FeatureSet& features,
        const std::map<ir::Inst*, GPRPublicationAction>* prepared_publications,
        const PseudoFlagsPlan* demands) {
    struct Candidate { ir::Inst* producer; ir::Inst* publication; u16 home; };
    std::vector<Candidate> candidates;
    // Only coalesced publications can retain a result; ordinary blocks avoid
    // constructing the physical suffix indices altogether.
    const auto select = [&](ir::Inst* inst) {
        if (inst->GetOp() != ir::OpCode::SetHostGPR || inst->GetArg<ir::Imm>(2).Get() != 0 ||
            !allocation.IsHostWriteCoalesced(inst->Id())) return;
        const auto home = inst->GetArg<ir::Imm>(1).Get();
        if (!IsFixedGPRHome(home)) return;
        auto stored = Resolve(inst->GetArg<ir::Value>(0));
        if (!stored.Defined()) return;
        auto* producer = stored.Def();
        if (producer->GetOp() == ir::OpCode::ZeroExtend32To64) {
            auto* source = Resolve(producer->GetArg<ir::Value>(0)).Def();
            // Keep the source candidate even if its point-dependent wrapper
            // tie is rejected below. The extension itself can also publish.
            if (source && ir::GetValueSizeByte(source->ReturnType()) == 4) producer = source;
        }
        const auto width = ir::GetValueSizeByte(producer->ReturnType());
        if ((width != 4 && width != 8) || producer->Id() >= inst->Id() || !analysis.Contains(producer) ||
            !CapturesParity(producer, analysis, demands)) return;
        candidates.push_back({producer, inst, static_cast<u16>(home)});
    };
    if (prepared_publications) {
        for (const auto& [inst, action] : *prepared_publications) select(inst);
    } else {
        for (auto* inst : analysis.Instructions()) select(inst);
    }
    FlagsTokenPlan plan;
    if (candidates.empty()) return plan;
    std::array<std::vector<ir::Inst*>, 31> publications, definitions;
    std::array<u32, 31> last_clobber{};
    bool preserved = false;
    for (auto* inst : analysis.Instructions()) {
        if (inst->GetOp() == ir::OpCode::SetHostGPR) {
            const auto home = inst->GetArg<ir::Imm>(1).Get();
            if (home < 31) publications[home].push_back(inst);
        }
        const auto clobbers = FixedGPRClobbers(*inst, features);
        for (unsigned home = 0; home < 31; ++home)
            if (clobbers & (1u << home)) last_clobber[home] = inst->Id();
        if (!inst->HasValue() || inst->IsBitCastOperation()) continue;
        preserved |= allocation.PreservedGPRDefinitionAt(inst->Id()) != nullptr;
        const ir::Value value{inst};
        if (allocation.ValueType(value) == RegAlloc::GPR)
            definitions[allocation.ValueGPR(value).id].push_back(inst);
    }
    plan.first_id = analysis.Instructions().front()->Id();
    plan.point_dependent = preserved;
    const auto after = [](const auto& nodes, u32 id) {
        return std::upper_bound(nodes.begin(), nodes.end(), id,
                [](u32 point, const ir::Inst* node) { return point < node->Id(); });
    };
    for (const auto& candidate : candidates) {
        auto* producer = candidate.producer;
        const auto id = producer->Id();
        const auto home = candidate.home;
        const auto& writes = publications[home];
        const auto publication = after(writes, id);
        if (publication == writes.end() || *publication != candidate.publication ||
            std::next(publication) != writes.end() || last_clobber[home] > id) continue;
        std::vector<u32> points{id};
        if (preserved) {
            // A funnel PF consumer can capture its earlier result, directly
            // or through an AND mask. Plan those capture sites explicitly.
            for (const auto& use : analysis.Uses(producer)) {
                if (use.consumer->GetOp() == ir::OpCode::Or) points.push_back(use.consumer->Id());
                if (use.consumer->GetOp() == ir::OpCode::And)
                    for (const auto& next : analysis.Uses(use.consumer))
                        if (next.consumer->GetOp() == ir::OpCode::Or) points.push_back(next.consumer->Id());
            }
            std::sort(points.begin(), points.end());
            points.erase(std::unique(points.begin(), points.end()), points.end());
        }
        for (const auto point : points) {
            auto stored = Resolve(candidate.publication->GetArg<ir::Value>(0));
            if (stored.Def() != producer && !Wrapper(allocation, producer, stored.Def(), point)) continue;
            bool safe = true;
            ir::Inst* wrapper = nullptr;
            const auto check = [&](ir::Inst* other) {
                if (!wrapper && Wrapper(allocation, producer, other, point)) { wrapper = other; return; }
                if (other != wrapper && Shares(allocation, ir::Value{producer}, ir::Value{other}, point)) safe = false;
            };
            if (preserved) {
                for (auto* node : analysis.Instructions())
                    if (node->Id() > id && node->HasValue() && !node->IsBitCastOperation()) check(node);
            } else if (allocation.ValueType(ir::Value{producer}) == RegAlloc::GPR) {
                const auto& owners = definitions[allocation.ValueGPR(ir::Value{producer}).id];
                for (auto it = after(owners, id); it != owners.end() && safe; ++it) check(*it);
            }
            if (!safe) continue;
            if (preserved) plan.point_homes[std::pair{id, point}] |= 1u << home;
            else {
                if (plan.homes.empty()) plan.homes.resize(analysis.Instructions().back()->Id() - plan.first_id + 1);
                plan.homes[id - plan.first_id] |= 1u << home;
            }
        }
    }
    return plan;
}
}
