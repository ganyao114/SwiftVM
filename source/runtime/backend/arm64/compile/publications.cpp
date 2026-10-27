#include "internal.h"
#include "register_contract.h"

namespace swift::runtime::backend::arm64 {
void TargetPreparation::PreparePublications(const ir::Block* block) {
    gpr_builder.IndexPublications();
    for (auto& publication : block->GetInstList()) {
        auto* inst = &publication;
        if (inst->GetOp() != ir::OpCode::SetHostGPR) continue;
        const auto target = inst->GetArg<ir::Imm>(1).Get();
        const auto byte_offset = inst->GetArg<ir::Imm>(2).Get();
        const auto width = ir::GetValueSizeByte(inst->GetArg<ir::Value>(0).Type());
        ASSERT_MSG(target < 31 && byte_offset < 8 && width <= 8 && byte_offset + width <= 8,
                   "invalid GPR publication at IR {}",
                   inst->Id());
        PublicationAction action{.instruction = inst->Id()};
        const auto* selected = gpr_builder.PublicationFor(inst);
        using Kind = ResidencePreparation::PublicationSelection::Kind;
        if (selected && selected->kind == Kind::Select) {
        } else if (selected && selected->kind == Kind::Spill) {
        } else if (memory_selections.IsLoadPublication(inst)) {
        } else if (gpr_builder.IsDeadPublication(inst)) {
        } else if (selected && selected->kind == Kind::Transfer) {
            action.kind = PublicationAction::Transfer;
            action.source = static_cast<u8>(selected->source);
            action.target = selected->target;
        } else if (selected && selected->kind == Kind::Copy) {
            if (selected->source != UINT16_MAX) {
                action.kind = PublicationAction::Copy;
                action.source = static_cast<u8>(selected->source);
                action.target = selected->target;
                action.SetView(static_cast<u8>(selected->width * 8));
            }
        } else {
            const auto value = inst->GetArg<ir::Value>(0);
            // PrepareMemoryUpdates just ran the identical matches for
            // every instruction in this block; reuse its results keyed
            // by the update instruction instead of re-matching (each
            // match re-walks operands and queries preservation
            // conflicts).
            const MemoryTransferAction* biased{};
            const MemoryTransferAction* preindex{};
            if (auto* update = value.Def()) {
                biased = memory_selections.Transfer(update, MemoryTransferAction::Biased);
                preindex = memory_selections.Transfer(update, MemoryTransferAction::PreIndex);
            }
            const bool coalesced = allocation.IsHostWriteCoalesced(inst->Id());
            if (biased && memory_selections.PublicationForUpdate(value.Def()) == inst->Id()) {
                action.kind = PublicationAction::BiasedUpdate;
                action.target = biased->home;
                action.parameter = static_cast<u16>(-biased->offset);
            } else if (preindex &&
                       memory_selections.PublicationForUpdate(value.Def()) == inst->Id()) {
                // The memory instruction performs the publication.
            } else if (coalesced && PrepareCoalescedHostWriteProof(inst)) {
                // The producer already writes the planned home.
            } else {
                ASSERT_MSG(!coalesced || features.ra_coalesce_live,
                           "SetHostGPR coalescing proof diverged at IR {}",
                           inst->Id());
                action.kind = PublicationAction::Value;
                action.target = target;
                action.SetView(static_cast<u8>(width * 8), static_cast<u8>(byte_offset * 8));
                const bool fused = value.Def() && IsGPRZeroExtendElided(value.Def());
                const auto residence = FixedGPRHomeForUse(value, inst);
                if (fused) action.properties |= PublicationAction::FusedSource;
                if (!fused && residence) action.source = static_cast<u8>(residence->home);
                if (IsFixedGPRHome(action.target) &&
                    (action.Width() == 32 || fused ||
                     (value.Def() && value.Def()->GetOp() == ir::OpCode::ZeroExtend32To64)))
                    action.properties |= PublicationAction::ClearHigh;
                if (residence && !residence->extension.KnownZeroAbove(32))
                    action.properties |= PublicationAction::AdjustResident;
                if (!coalesced && !fused && !residence)
                    action.properties |= PublicationAction::AdoptsSpill;
            }
        }
        publications.push_back(action);
    }
}

}  // namespace swift::runtime::backend::arm64
