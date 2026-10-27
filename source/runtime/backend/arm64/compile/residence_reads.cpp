#include <algorithm>
#include <iterator>
#include "register_contract.h"
#include "residence_preparation.h"

namespace swift::runtime::backend::arm64 {

std::optional<u16> MatchNarrowFixedGPRRead(
        const ir::Inst& read,
        compile::BlockAnalysisIndex& block_analysis,
        RegisterStatePreparation& register_state,
        const std::function<ir::Flags(const ir::Inst*)>& set_flags,
        bool allow_widen_extend) {
    if (read.GetOp() != ir::OpCode::GetHostGPR) return std::nullopt;
    const u32 home = read.GetArg<ir::Imm>(0).Get();
    const u32 width = ir::GetValueSizeByte(read.ReturnType());
    if (read.GetArg<ir::Imm>(1).Get() != 0 || !IsFixedGPRHome(home) || read.GetUses() == 0 ||
        width > sizeof(u32)) {
        return std::nullopt;
    }
    block_analysis.PrepareUses();
    const auto uses = block_analysis.Uses(&read);
    if (uses.size() != 1 || uses.front().count != read.GetUses()) return std::nullopt;
    auto* consumer = uses.front().consumer;
    for (auto value : consumer->GetValues()) {
        if (value.Def() == &read && ir::GetValueSizeByte(value.Type()) > width)
            return std::nullopt;  // A dirty X register cannot supply a widened W capture.
    }
    if (consumer->Id() <= read.Id() ||
        !register_state.PrepareHomeSurvival(home, read.Id(), consumer->Id())) {
        return std::nullopt;
    }
    const auto op = consumer->GetOp();
    const u32 result_width = consumer->HasValue() && consumer->ReturnType() != ir::ValueType::VOID
                                     ? ir::GetValueSizeByte(consumer->ReturnType())
                                     : 0;
    const bool direct_alu =
            (op == ir::OpCode::And || op == ir::OpCode::Xor) && result_width <= sizeof(u32) &&
            (home <= 5 || width == sizeof(u32) || (home >= 22 && !X86PinExtLevel3Requested()));
    const bool direct_or =
            op == ir::OpCode::Or && width == sizeof(u32) && result_width == sizeof(u32);
    const bool add_sub = op == ir::OpCode::Add || op == ir::OpCode::Sub;
    const bool caller_alu = home <= 9 && add_sub && result_width <= sizeof(u32) &&
                            (home <= 5 || width == sizeof(u32));
    const bool adjacent_flags =
            home >= 6 && home <= 9 && (width == sizeof(u8) || width == sizeof(u16)) &&
            consumer->Id() == read.Id() + 1 && add_sub && result_width == width &&
            True(set_flags(consumer) & ir::Flags::NZCV);
    const bool callee_sub = home >= 19 && op == ir::OpCode::Sub && result_width <= sizeof(u32);
    const bool zero_extend = (width == sizeof(u8) || width == sizeof(u16)) &&
                             op == ir::OpCode::ZeroExtend32 &&
                             consumer->GetArg<ir::Value>(0).Def() == &read;
    const bool sign_extend = home >= 19 && op == ir::OpCode::SignExtend &&
                             consumer->GetArg<ir::Value>(0).Def() == &read;
    // Early-only: a U32 read widened to U64 reads the same W home. Width
    // coalescing still owns the bridge and FinalizeFixedGPRReadConstraints
    // drops this constraint when a width chain adopts the read.
    const bool widen_extend = allow_widen_extend && width == sizeof(u32) &&
                              op == ir::OpCode::ZeroExtend32To64 &&
                              consumer->GetArg<ir::Value>(0).Def() == &read;
    const bool store = uses.front().count == 1 && op == ir::OpCode::StoreMemory &&
                       consumer->GetArg<ir::Value>(1).Def() == &read;
    if (!(direct_alu || direct_or || caller_alu || adjacent_flags || callee_sub || zero_extend ||
          sign_extend || widen_extend || store))
        return std::nullopt;
    return std::optional<u16>{static_cast<u16>(home)};
}

void ResidencePreparation::PrepareNarrowReads(
        const std::function<ir::Flags(const ir::Inst*)>& set_flags) {
    ASSERT(!finished);
    for (auto& read : block->GetInstList()) {
        const auto* capture = allocation.PreservedGPRDefinitionAt(read.Id());
        if (read.GetOp() != ir::OpCode::GetHostGPR ||
            (allocation.IsFixedGPRDefinitionElided(read.Id()) && !capture) ||
            allocation.IsHostReadCoalesced(read.Id()) || HasLowView(&read) ||
            (ValueFullyResident(&read) && !capture))
            continue;
        if (const auto home =
                    MatchNarrowFixedGPRRead(read, PrepareUseFacts(), register_state, set_flags)) {
            AssignLowView(&read, *home);
        }
    }
}

}  // namespace swift::runtime::backend::arm64

namespace swift::runtime::backend::arm64 {

namespace {

using ::swift::runtime::backend::IsFixedGPRHome;

bool IsMemoryOperation(ir::OpCode op) {
    return op == ir::OpCode::LoadMemory || op == ir::OpCode::StoreMemory ||
           op == ir::OpCode::LoadMemoryTSO || op == ir::OpCode::StoreMemoryTSO;
}

u32 CountAddressUses(const ir::Inst& consumer, const ir::Inst* definition) {
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

bool IsFullWidthAlias(const ir::Inst& inst, const ir::Inst* definition) {
    if (inst.GetOp() != ir::OpCode::BitCast ||
        ir::GetValueSizeByte(inst.ReturnType()) != sizeof(u64)) {
        return false;
    }
    const auto source = inst.GetArg<ir::Value>(0);
    return source.Def() == definition && ir::GetValueSizeByte(source.Type()) == sizeof(u64);
}

}  // namespace

std::optional<FixedGPRTransfer> MatchFixedGPRTransfer(const ir::Inst* publication,
                                                      compile::BlockAnalysisIndex& block_analysis,
                                                      RegisterStatePreparation& register_state,
                                                      FixedGPRTransferUses& aliases) {
    aliases.Reset();
    if (!publication || publication->GetOp() != ir::OpCode::SetHostGPR ||
        publication->GetArg<ir::Imm>(2).Get() != 0)
        return std::nullopt;

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

    if (!register_state.PrepareHomeSurvival(source, read->Id(), publication->Id())) {
        return std::nullopt;
    }

    block_analysis.PrepareUses();
    u32 last_use = publication->Id();
    bool has_transferred_use = false;
    // The root is implicit; appended aliases are both the breadth-first
    // queue and the caller's final borrowed view. Re-fetch after each append
    // because growth can invalidate a previous span into the shared buffer.
    for (size_t index = 0; index <= aliases.Aliases().size(); ++index) {
        auto* definition = read;
        if (index != 0)
            definition = block_analysis.Instructions()[aliases.Aliases()[index - 1].position];
        u32 ordinary_uses{};
        for (const auto& use : block_analysis.Uses(definition)) {
            auto& consumer = *use.consumer;
            const u32 uses = use.count;
            ordinary_uses += uses;
            if (&consumer == publication && definition == read && uses == 1) {
                continue;
            }
            if (uses == 1 && IsFullWidthAlias(consumer, definition)) {
                const auto position = block_analysis.InstructionPosition(&consumer);
                ASSERT(position && *position <= UINT16_MAX);
                aliases.Append(static_cast<u16>(*position));
                continue;
            }
            u32 full_width_uses = 0;
            for (auto value : consumer.GetValues())
                if (value.Def() == definition && ir::GetValueSizeByte(value.Type()) == sizeof(u64))
                    ++full_width_uses;
            if (consumer.Id() <= publication->Id() || full_width_uses != uses) {
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

    if (!register_state.PrepareHomeSurvival(target, publication->Id(), last_use)) {
        return std::nullopt;
    }

    return FixedGPRTransfer{
            .read = read,
            .aliases = aliases.Aliases(),
            .source = static_cast<u16>(source),
            .target = static_cast<u16>(target),
            .last_use = last_use,
    };
}

std::optional<FixedGPRTransfer> ResidencePreparation::MatchTransfer(
        const ir::Inst* publication, FixedGPRTransferUses& aliases) {
    ASSERT(block);
    if (!publication || IsDeadPublication(publication) ||
        allocation.NeedsFixedGPRPublication(publication->Id()) ||
        allocation.IsHostWriteCoalesced(publication->Id()))
        return std::nullopt;
    return MatchFixedGPRTransfer(publication, PrepareUseFacts(), register_state, aliases);
}

void ResidencePreparation::PrepareFixedGPRTransfers() {
    for (const auto position : PreparePublicationCandidates()) {
        const auto& inst = *UseFacts().Instructions()[position];
        FixedGPRTransferUses aliases{allocation.Workspace()};
        auto candidate = MatchTransfer(&inst, aliases);
        if (!candidate) {
            continue;
        }
        AssignLowView(candidate->read, candidate->source);
        AssignTransferredValue(
                candidate->read, candidate->target, inst.Id() + 1, candidate->last_use);
        // Record the transfer residence while its recipe is available.
        // The builder-only API does not publish physical constraints.
        if (allocation.TargetPlanning())
            allocation.PlanFixedGPRDefinition(candidate->read->Id(),
                                              {.home = candidate->source,
                                               .transferred_home = candidate->target,
                                               .end = inst.Id(),
                                               .transfer = inst.Id(),
                                               .last_use = candidate->last_use,
                                               .publication = inst.Id(),
                                               .owner = candidate->read->Id(),
                                               .width = 8,
                                               .elided = true,
                                               .version = true});
        for (const auto& entry : candidate->aliases) {
            auto* alias = UseFacts().Instructions()[entry.position];
            AssignTransferredValue(alias, candidate->target, inst.Id() + 1, candidate->last_use);
        }
        SelectPublication({&inst,
                           candidate->read,
                           candidate->source,
                           candidate->target,
                           0,
                           PublicationSelection::Kind::Transfer});
    }
}

}  // namespace swift::runtime::backend::arm64
