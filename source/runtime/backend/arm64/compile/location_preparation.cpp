#include "location_preparation.h"

#include <algorithm>
#include <array>
#include <type_traits>
#include "runtime/common/svm_config.h"
#include "runtime/common/variant_util.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::backend::arm64 {
namespace {
bool SupportsDeferredLocation(const ir::Terminal& terminal) {
    return VisitVariant<bool>(terminal, [](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        return std::is_same_v<T, ir::terminal::Invalid> ||
               std::is_same_v<T, ir::terminal::ReturnToDispatch> ||
               std::is_same_v<T, ir::terminal::PopRSBHint>;
    });
}

const ir::Inst* PendingLocation(const ir::Block& block) {
    for (auto it = block.GetInstList().rbegin(); it != block.GetInstList().rend(); ++it) {
        if (it->GetOp() == ir::OpCode::SetLocation) return &*it;
        if (it->GetOp() != ir::OpCode::PopRSB && it->GetOp() != ir::OpCode::CallReturn) break;
    }
    return nullptr;
}
}  // namespace

std::vector<u16> PrepareDeferredLocations(const ir::HIRFunction& function,
                                          compile::SemanticFacts& facts,
                                          compile::AllocationView allocation,
                                          const FeatureSet& features) {
    ASSERT(facts.MatchesInput(nullptr, &function));
    if (!features.indirect_l1) return {};
    struct Candidate {
        u16 instruction;
        u8 reg;
        bool required;
    };
    std::vector<Candidate> candidates;
    std::array<u8, 32> occurrences{};
    for (const auto& member : function.GetHIRBlocksRPO()) {
        const auto* block = member.GetBlock();
        if (!SupportsDeferredLocation(block->GetTerminal())) continue;
        const auto& summary = facts.PrepareSummary(block);
        if (!summary.HasLocationWrites()) continue;
        const auto* inst = PendingLocation(*block);
        if (!inst) continue;
        const auto lambda = inst->GetArg<ir::Lambda>(0);
        if (!lambda.IsValue()) continue;
        const auto location =
                allocation.GPRLocationAt(lambda.GetValue(),
                                         block->GetInstList().back().Id(),
                                         compile::AllocationView::GPRView::WholeValue);
        ASSERT(location.kind == compile::AllocationView::GPR ||
               location.kind == compile::AllocationView::MEM);
        const auto reg = location.kind == compile::AllocationView::GPR ? location.slot
                                                                       : location.reload_register;
        ASSERT(reg == UINT16_MAX || reg < occurrences.size());
        // A plain spill scratch can depend on a forwarded pending write. Do
        // not lease a register or guess its identity while grouping targets.
        // Calls/returns still require deferred publication with that scratch;
        // an unproved optional sharing opportunity uses ordinary publication.
        candidates.push_back({inst->Id(), static_cast<u8>(reg), summary.HasGuestCallOrPop()});
        if (reg < occurrences.size() && occurrences[reg] < 2) ++occurrences[reg];
    }
    std::vector<u16> deferred;
    for (const auto& candidate : candidates)
        if (candidate.required ||
            (candidate.reg < occurrences.size() && occurrences[candidate.reg] >= 2))
            deferred.push_back(candidate.instruction);
    std::sort(deferred.begin(), deferred.end());
    return deferred;
}
}  // namespace swift::runtime::backend::arm64
