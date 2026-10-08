#include "runtime/backend/reg_alloc.h"
#include "pinned_gpr_allocation.h"

namespace swift::runtime::backend::arm64 {

namespace {

using ::swift::runtime::backend::IsFixedGPRHome;

bool SupportsDirectPublication(ir::OpCode op) {
    using O = ir::OpCode;
    return op == O::LoadImm || op == O::LoadMemory || op == O::Add ||
           op == O::Sub || op == O::And || op == O::GetOperand ||
           op == O::SignExtend || op == O::BitExtract;
}

}  // namespace

std::optional<PinnedGPRAllocation::SpilledGPRPublication>
PinnedGPRAllocation::MatchSpilledGPRPublication(ir::Inst* publication) const {
    if (!publication || publication->GetOp() != ir::OpCode::SetHostGPR ||
        publication->GetArg<ir::Imm>(2).Get() != 0 ||
        recipes.dead_writes.contains(publication) ||
        allocation.IsHostWriteCoalesced(publication->Id())) {
        return std::nullopt;
    }

    const auto value = publication->GetArg<ir::Value>(0);
    auto* producer = value.Def();
    const u32 width = ir::GetValueSizeByte(value.Type());
    const u32 target = publication->GetArg<ir::Imm>(1).Get();
    if (!producer || !SupportsDirectPublication(producer->GetOp()) ||
        (width != sizeof(u32) && width != sizeof(u64)) ||
        ir::GetValueSizeByte(producer->ReturnType()) != width ||
        !IsFixedGPRHome(target) || allocation.ValueType(value) != RegAlloc::MEM ||
        producer->GetUses(false) != 1) {
        return std::nullopt;
    }

    auto& instructions = this->block->GetInstList();
    const auto producer_it = instructions.iterator_to(*producer);
    const auto publication_it = instructions.iterator_to(*publication);
    if (producer->Id() >= publication->Id() ||
        !guest_state_map.PublicationWindowSafe(
                target, value, producer->Id(), publication->Id())) {
        return std::nullopt;
    }
    for (auto it = std::next(producer_it); it != publication_it; ++it) {
        if (it == instructions.end() || it->GetOp() == ir::OpCode::Goto ||
            it->GetOp() == ir::OpCode::NotGoto ||
            it->GetOp() == ir::OpCode::BindLabel ||
            (backend::FixedGPRClobbers(*it, features, true) &
             (1u << target))) {
            return std::nullopt;
        }
        for (auto input : it->GetValues()) {
            if (input.Defined() && IsGPRMappedTo(input, target)) {
                return std::nullopt;
            }
        }
        if (it->HasValue() &&
            IsGPRMappedTo(ir::Value{it.operator->()}, target)) {
            return std::nullopt;
        }
    }

    return SpilledGPRPublication{
            .producer = producer,
            .publication = publication,
            .target = static_cast<u16>(target),
    };
}

void PinnedGPRAllocation::PrepareSpilledGPRPublications(ir::Block* block) {
    for (auto& inst : block->GetInstList()) {
        auto candidate = MatchSpilledGPRPublication(&inst);
        if (!candidate || HasValueHome(candidate->producer)) {
            continue;
        }
        AssignValueHome(candidate->producer, candidate->target);
        recipes.spilled_publications.emplace(&inst, *candidate);
    }
}

}  // namespace swift::runtime::backend::arm64
