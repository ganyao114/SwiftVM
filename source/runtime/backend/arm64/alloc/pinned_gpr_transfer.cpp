#include "runtime/backend/reg_alloc.h"
#include "pinned_gpr_allocation.h"

namespace swift::runtime::backend::arm64 {

namespace {

using ::swift::runtime::backend::IsFixedGPRHome;

bool IsMemoryOperation(ir::OpCode op) {
    return op == ir::OpCode::LoadMemory || op == ir::OpCode::StoreMemory ||
           op == ir::OpCode::LoadMemoryTSO || op == ir::OpCode::StoreMemoryTSO;
}

u32 CountAddressUses(ir::Inst& consumer, ir::Inst* definition) {
    if (!IsMemoryOperation(consumer.GetOp())) {
        return 0;
    }
    const auto address = consumer.GetArg<ir::Operand>(0);
    u32 uses{};
    if (address.GetLeft().IsValue() && address.GetLeft().value.Def() == definition) {
        ++uses;
    }
    if (address.GetRight().IsValue() && address.GetRight().value.Def() == definition) {
        ++uses;
    }
    return uses;
}

bool IsFullWidthAlias(ir::Inst& inst, ir::Inst* definition) {
    if (inst.GetOp() != ir::OpCode::BitCast ||
        ir::GetValueSizeByte(inst.ReturnType()) != sizeof(u64)) {
        return false;
    }
    const auto source = inst.GetArg<ir::Value>(0);
    return source.Def() == definition && ir::GetValueSizeByte(source.Type()) == sizeof(u64);
}

}  // namespace

std::optional<FixedGPRTransfer> MatchFixedGPRTransfer(
        ir::Inst* publication, const BlockAnalysisIndex& block_analysis,
        const GuestStateMap& guest_state_map) {
    if (!publication || publication->GetOp() != ir::OpCode::SetHostGPR ||
        publication->GetArg<ir::Imm>(2).Get() != 0) return std::nullopt;

    const auto published = publication->GetArg<ir::Value>(0);
    auto* read = published.Def();
    if (!read || read->GetOp() != ir::OpCode::GetHostGPR || read->GetArg<ir::Imm>(1).Get() != 0 ||
        ir::GetValueSizeByte(read->ReturnType()) != sizeof(u64) ||
        ir::GetValueSizeByte(published.Type()) != sizeof(u64) || read->Id() >= publication->Id()) {
        return std::nullopt;
    }

    const u32 source = read->GetArg<ir::Imm>(0).Get();
    const u32 target = publication->GetArg<ir::Imm>(1).Get();
    if (!IsFixedGPRHome(source) || !IsFixedGPRHome(target) || source == target) {
        return std::nullopt;
    }

    if (!guest_state_map.FixedHomeSurvives(
                source, read->Id(), publication->Id())) {
        return std::nullopt;
    }

    StackVector<ir::Inst*, 8> values{read};
    std::vector<ir::Inst*> aliases{};
    u32 last_use = publication->Id();
    bool has_transferred_use = false;
    for (size_t index = 0; index < values.size(); ++index) {
        auto* definition = values[index];
        u32 ordinary_uses{};
        for (const auto& use : block_analysis.Uses(definition)) {
            auto& consumer = *use.consumer;
            const u32 uses = use.count;
            ordinary_uses += uses;
            if (&consumer == publication && definition == read && uses == 1) {
                continue;
            }
            if (uses == 1 && IsFullWidthAlias(consumer, definition)) {
                values.push_back(&consumer);
                aliases.push_back(&consumer);
                continue;
            }
            if (consumer.Id() <= publication->Id() ||
                CountAddressUses(consumer, definition) != uses) {
                return std::nullopt;
            }
            has_transferred_use = true;
            last_use = std::max<u32>(last_use, consumer.Id());
        }
        if (ordinary_uses != definition->GetUses(false)) {
            return std::nullopt;
        }
    }
    if (!has_transferred_use) {
        return std::nullopt;
    }

    if (!guest_state_map.FixedHomeSurvives(
                target, publication->Id(), last_use)) {
        return std::nullopt;
    }

    return FixedGPRTransfer{
            .read = read,
            .publication = publication,
            .aliases = std::move(aliases),
            .source = static_cast<u16>(source),
            .target = static_cast<u16>(target),
            .last_use = last_use,
    };
}

std::optional<PinnedGPRAllocation::PinnedGPRValueTransfer>
PinnedGPRAllocation::MatchPinnedGPRValueTransfer(ir::Inst* publication) const {
    ASSERT(block);
    if (!publication || recipes.dead_writes.contains(publication) ||
        allocation.NeedsFixedGPRPublication(publication->Id()) ||
        allocation.IsHostWriteCoalesced(publication->Id())) return std::nullopt;
    return MatchFixedGPRTransfer(publication, block_analysis, guest_state_map);
}

void PinnedGPRAllocation::PreparePinnedGPRValueTransfers(ir::Block* block) {
    for (auto& inst : block->GetInstList()) {
        auto candidate = MatchPinnedGPRValueTransfer(&inst);
        if (!candidate) {
            continue;
        }
        AssignLowView(candidate->read, candidate->source);
        AssignTransferredValue(candidate->read, candidate->target,
                               inst.Id() + 1, candidate->last_use);
        for (auto* alias : candidate->aliases) {
            AssignTransferredValue(alias, candidate->target,
                                   inst.Id() + 1, candidate->last_use);
        }
        recipes.transfers.emplace(&inst, std::move(*candidate));
    }
}

}  // namespace swift::runtime::backend::arm64
