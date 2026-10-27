#include "vector_preparation.h"

#include <algorithm>
#include <optional>
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/workspace.h"

namespace swift::runtime::backend::arm64 {
VectorActions PrepareVector(const ir::Block* block,
                            compile::AllocationView allocation,
                            compile::BlockAnalysisIndex& index,
                            compile::CompileWorkspace& workspace) {
    if (!index.HasFPRInstructions()) return {};
    const auto nodes = index.Instructions();
    ASSERT(nodes.size() <= size_t{UINT16_MAX} + 1);
    const bool publications = index.Summary().HasLow64FPRWrites();
    std::optional<compile::LoweringCandidateScope> candidates;
    std::array<bool, 32> zero_high{};
    u32 load_homes{};
    bool conversion_uses{};
    size_t position{};
    for (const auto& inst : block->GetInstList()) {
        if (position >= nodes.size() || nodes[position] != &inst) return {};
        const auto op = inst.GetOp();
        if (detail::IsVectorOpaqueBarrier(op)) zero_high.fill(false);
        u16 roles{};
        if (op == ir::OpCode::SetHostFPR) {
            if (publications) {
                roles |= detail::SelectScalarPublication(
                        inst, position + 1 < nodes.size() ? nodes[position + 1] : nullptr, index);
                if (roles & detail::PublicationLoad)
                    load_homes |= u32{1} << inst.GetArg<ir::Imm>(1).Get();
            }
            if (detail::SelectResidentConversion(inst, zero_high, index)) {
                roles |= detail::Conversion;
                conversion_uses |= inst.GetArg<ir::Value>(0).Def()->GetUses(false) == 2;
            }
        } else if (op == ir::OpCode::GetHostFPR && detail::IsResidentMemoryRead(inst))
            roles |= detail::MemoryRead;
        else if (op == ir::OpCode::VecExtract64 && detail::IsResidentExtract(inst))
            roles |= detail::ExtractStore;
        if (roles) {
            if (!candidates) candidates.emplace(workspace);
            candidates->Entries().push_back({static_cast<u16>(position), roles});
        }
        ++position;
    }
    if (position != nodes.size() || !candidates) return {};

    std::vector<detail::ScalarFPROwner> owners;
    if (load_homes) {
        for (const auto* inst : nodes) {
            if (!inst->HasValue() || inst->IsBitCastOperation() ||
                !ir::IsFloatValueType(inst->ReturnType()))
                continue;
            const ir::Value value{inst};
            if (allocation.ValueType(value) != compile::AllocationView::FPR) continue;
            const auto home = allocation.ValueFPR(value).id;
            if (home >= 32 || !(load_homes & (u32{1} << home))) continue;
            index.PrepareResolvedUses();
            owners.push_back({inst->Id(),
                              index.LastInstructionUse(inst),
                              home,
                              inst->GetUses(false) > index.DirectUseCount(inst)});
        }
        std::sort(owners.begin(), owners.end(), [](const auto& left, const auto& right) {
            return left.home < right.home;
        });
    }
    if (conversion_uses) index.PrepareUses();
    const auto& entries = candidates->Entries();
    return {detail::LowerScalarFPRPublications(index, entries, owners),
            detail::LowerResidentScalarFPR(index, entries)};
}
}  // namespace swift::runtime::backend::arm64
