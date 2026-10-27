#include "audit.h"
#include "register_contract.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/compile/analysis/uses.h"
#include "runtime/compile/audit/verify.h"

#include <algorithm>
#include <array>
#include <bit>

namespace swift::runtime::backend::arm64 {
void AuditGPRAllocation(std::span<const EmissionCertificateInput> inputs,
                        compile::AllocationView allocation,
                        const UnitActions& actions,
                        const FeatureSet& features,
                        const ir::HIRFunction* function,
                        bool internal_edges) {
    ASSERT(actions.MatchesPreparation(allocation) || actions.MatchesAllocation(allocation));
    StackVector<const ir::Block*, 4> blocks;
    for (const auto& input : inputs) blocks.push_back(input.block);
    const compile::audit::IRReference reference{
            {blocks.data(), blocks.size()}, function, allocation.MapCount()};
    ASSERT_MSG(reference.Valid(), "invalid audit IR reference: {}",
               static_cast<unsigned>(reference.Failure()));
    std::array<std::vector<u32>, 32> clobbers;
    for (const auto& input : inputs) {
        ASSERT(input.block && actions.ForBlock(input.block));
        for (auto& inst : input.block->GetInstList()) {
            u32 mask = FixedGPRClobbers(inst, features);
            if (const auto helper = HelperCallContract::Resolve(inst, features)) {
                // Ordinary SSA residences are saved/restored by EmitHostCall
                // using this instruction's verified live mask. Pinned home
                // versions use the independent fixed-home/helper check above.
                mask = 0;
            }
            while (mask) {
                clobbers[std::countr_zero(mask)].push_back(inst.Id());
                mask &= mask - 1;
            }
            if (!allocation.HasGPRPreservationActions()) continue;
            for (const auto& copy : allocation.GPRPreservationsBefore(inst.Id())) {
                ASSERT(copy.width && copy.width <= 8 && copy.bit_offset + copy.width * 8 <= 64);
                ASSERT(copy.destination_kind == compile::AllocationView::GPR ||
                       copy.destination_kind == compile::AllocationView::MEM);
                if (copy.destination_kind == compile::AllocationView::MEM) {
                    ASSERT(copy.destination < compile::kMaxSpillSlots);
                    if (copy.width != 8 || copy.bit_offset) {
                        const auto* reload = allocation.SpillReloadAt(copy.value, inst.Id());
                        ASSERT(copy.scratch < 31 &&
                               (!allocation.DirtyGPR(inst.Id()).Get(copy.scratch) ||
                                (reload && reload->reg == copy.scratch)));
                    }
                } else
                    ASSERT(allocation.DirtyGPR(inst.Id()).Get(copy.destination));
            }
        }
        const auto verify_locations = [&](ir::Value value, const ir::Inst& consumer) {
            if (!value.Defined()) return;
            for (unsigned view = 0; view < 4; ++view) {
                const auto location = allocation.GPRLocationAt(
                        value, consumer.Id(), static_cast<compile::AllocationView::GPRView>(view));
                if (location.kind == compile::AllocationView::GPR) {
                    ASSERT_MSG(location.slot < 31 && (location.bits == 32 || location.bits == 64),
                               "invalid GPR location for SSA {} at IR {}: slot={} bits={}",
                               value.Id(),
                               consumer.Id(),
                               location.slot,
                               location.bits);
                    ASSERT(location.extension.known_zero_above <= 64);
                } else if (location.kind == compile::AllocationView::MEM) {
                    ASSERT(location.slot < compile::kMaxSpillSlots);
                    ASSERT(location.reload_register == UINT16_MAX || location.reload_register < 31);
                }
            }
        };
        // Audit the actual sealed operand edges, including their raw views,
        // Lambda and Params. No full operand snapshot belongs in emission data.
        for (const auto& inst : input.block->GetInstList()) {
            if (inst.HasValue()) verify_locations(ir::Value{&inst}, inst);
            inst.ForEachValue([&](ir::Value value) { verify_locations(value, inst); });
        }
    }
    for (auto& points : clobbers) std::sort(points.begin(), points.end());
    const auto verify_use =
            [&](ir::Value value, const ir::Inst* consumer, const ir::Block* block, u32 point) {
                if (!value.Defined() || value.Type() == ir::ValueType::VOID) return;
                const auto* owner_block = reference.Owner(value.Def());
                ASSERT_MSG(owner_block && reference.Dominates(owner_block, block),
                           "GPR SSA definition {} does not dominate use {}",
                           value.Id(),
                           point);
                ASSERT_MSG(owner_block == block || internal_edges,
                           "cross-block GPR SSA {} requires internal function edges",
                           value.Id());
                const auto location =
                        allocation.GPRLocationAt(value,
                                                 consumer ? consumer->Id() : UINT32_MAX,
                                                 compile::AllocationView::GPRView::WholeValue);
                if (location.kind == compile::AllocationView::GPR)
                    ASSERT(location.slot < 31 && (location.bits == 32 || location.bits == 64));
                if (location.kind != compile::AllocationView::GPR || location.pinned ||
                    allocation.GetGprs().Get(location.slot))
                    return;
                u32 birth = allocation.AllocationId(value);
                if (const auto* capture = allocation.PreservedGPRDefinitionAt(value.Id()))
                    birth = capture->transfer;
                const auto& points = clobbers[location.slot];
                const auto next = std::upper_bound(points.begin(), points.end(), birth);
                ASSERT_MSG(next == points.end() || *next >= point,
                           "GPR SSA {} in x{} crosses physical clobber {} before use {}",
                           value.Id(),
                           location.slot,
                           next == points.end() ? UINT32_MAX : *next,
                           point);
            };
    for (const auto& input : inputs) {
        for (auto& inst : input.block->GetInstList()) {
            if (inst.IsPseudoOperation() || inst.GetOp() == ir::OpCode::BindLabel) continue;
            inst.ForEachValue([&](ir::Value value) {
                verify_use(value, &inst, input.block, inst.Id());
            });
        }
        const auto* tail =
                input.block->IsEmptyBlock() ? nullptr : &input.block->GetInstList().back();
        compile::VisitImplicitUses(*input.block, tail ? tail->Id() : 0,
                [&](ir::Value value, u32 point) { verify_use(value, tail, input.block, point); });
    }
}
}  // namespace swift::runtime::backend::arm64
