#include "fpr_preparation.h"

#include <algorithm>
#include <optional>
#include "runtime/backend/arm64/pshufd_direct.h"
#include "runtime/compile/allocate/writer.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/compile/workspace.h"

namespace swift::runtime::backend::arm64 {
FPRContract PrepareFPRContract(const ir::Block* block,
                               compile::AllocationWriter& allocation,
                               const FeatureSet& features,
                               bool scalar_insert,
                               bool scalar_tie,
                               compile::BlockAnalysisIndex& analysis,
                               compile::CompileWorkspace& workspace) {
    const auto nodes = analysis.Instructions();
    ASSERT(nodes.size() <= size_t{UINT16_MAX} + 1);
    std::optional<compile::LoweringCandidateScope> candidates;
    u32 owner_homes{};
    u16 demands{};
    size_t position{};
    for (const auto& inst : block->GetInstList()) {
        if (position >= nodes.size() || nodes[position] != &inst)
            return {.valid = false, .rejected = &inst};
        const auto roles =
                detail::SelectFPRProof(inst, allocation, features, scalar_insert, scalar_tie);
        if (roles) {
            if (!candidates) candidates.emplace(workspace);
            candidates->Entries().push_back({static_cast<u16>(position), roles});
            demands |= roles;
            if (roles & detail::FPRProofOwners) {
                const auto home = inst.GetOp() == ir::OpCode::SetHostFPR
                                          ? inst.GetArg<ir::Imm>(1).Get()
                                          : allocation.ValueFPR(ir::Value{&inst}).id;
                if (home < 32) owner_homes |= u32{1} << home;
            }
        }
        ++position;
    }
    if (position != nodes.size())
        return {.valid = false,
                .rejected = block->GetInstList().empty() ? nullptr : &block->GetInstList().front()};
    if (!candidates) return {};
    if (demands & detail::FPRProofResolved)
        analysis.PrepareResolvedUses();
    else if (demands & detail::FPRProofDirect)
        analysis.PrepareUses();
    const compile::GuestStateRegisterAccesses* registers{};
    std::optional<std::span<const ir::Inst* const>> observations;
    if (analysis.HasFPRWrites() &&
        (demands & (detail::FPRProofRegisters | detail::FPRProofObservations))) {
        auto& facts = allocation.PrepareFacts(block);
        if (demands & detail::FPRProofRegisters)
            registers = &facts.PrepareGuestStateRegisterAccesses(block);
        if (demands & detail::FPRProofObservations)
            observations = facts.PrepareGuestStateObservations(block);
    }
    std::vector<detail::FPRProofOwner> owners;
    if (owner_homes) {
        for (size_t i = 0; i < nodes.size(); ++i) {
            const auto* inst = nodes[i];
            if (!inst->HasValue() || inst->IsBitCastOperation()) continue;
            const ir::Value value{inst};
            if (allocation.ValueType(value) != compile::AllocationView::FPR) continue;
            const auto home = allocation.ValueFPR(value).id;
            if (home < 32 && (owner_homes & (u32{1} << home)))
                owners.push_back({static_cast<u16>(i), home});
        }
        std::sort(owners.begin(), owners.end(), [](const auto& left, const auto& right) {
            return left.home != right.home ? left.home < right.home
                                           : left.position < right.position;
        });
    }
    return detail::VerifyFPRContract(allocation,
                                     features,
                                     scalar_insert,
                                     scalar_tie,
                                     analysis,
                                     registers,
                                     observations,
                                     candidates->Entries(),
                                     owners,
                                     workspace);
}

void SelectPshufdDirect(compile::BlockAnalysisIndex& analysis,
                        compile::AllocationWriter& allocation) {
    if (allocation.FPRCoalescingDisabled()) return;
    for (auto* constant : analysis.Instructions()) {
        if (constant->GetOp() != ir::OpCode::VecLoadConst ||
            !DecodePshufdDirectControl(constant->GetArg<ir::Imm>(0).Get(),
                                       constant->GetArg<ir::Imm>(1).Get()))
            continue;
        analysis.PrepareUses();
        const auto uses = analysis.Uses(constant);
        u32 local_uses = 0;
        bool exact = !uses.empty();
        for (const auto& use : uses) {
            const auto* consumer = use.consumer;
            local_uses += use.count;
            exact &= use.count == 1 && consumer->GetOp() == ir::OpCode::VecShuffle32Indexed &&
                     consumer->GetArg<ir::Value>(1).Def() == constant &&
                     consumer->GetArg<ir::Value>(0).Def() != constant;
        }
        if (!exact || local_uses != constant->GetUses()) continue;
        allocation.MarkPshufdDirect(constant->Id());
        for (const auto& use : uses) allocation.MarkPshufdDirect(use.consumer->Id());
    }
}
}  // namespace swift::runtime::backend::arm64
