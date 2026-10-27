#include <algorithm>
#include <iterator>
#include "register_contract.h"
#include "residence_preparation.h"

namespace swift::runtime::backend::arm64 {

namespace {

using ::swift::runtime::backend::IsFixedGPRHome;

u32 CountUses(const ir::Inst& consumer, const ir::Inst* definition) {
    return std::ranges::count_if(consumer.GetValues(),
                                 [&](ir::Value value) { return value.Def() == definition; });
}

bool IsLow32Alias(const ir::Inst& alias, const ir::Inst* definition) {
    return alias.GetOp() == ir::OpCode::BitExtract &&
           alias.GetArg<ir::Value>(0).Def() == definition && alias.GetArg<ir::Imm>(1).Get() == 0 &&
           alias.GetArg<ir::Imm>(2).Get() == 32 && alias.ReturnType() == ir::ValueType::U32;
}

bool IsAliasConsumer(const ir::Inst& consumer, const ir::Inst* alias) {
    const u32 uses = CountUses(consumer, alias);
    if (uses != 1) {
        return false;
    }
    if (consumer.GetOp() == ir::OpCode::ZeroExtend32To64) {
        return consumer.GetArg<ir::Value>(0).Def() == alias;
    }
    switch (consumer.GetOp()) {
        case ir::OpCode::Add:
        case ir::OpCode::Sub:
        case ir::OpCode::And:
        case ir::OpCode::Or:
        case ir::OpCode::Xor:
        case ir::OpCode::Select:
            return consumer.ReturnType() == ir::ValueType::U32;
        default:
            return false;
    }
}

}  // namespace

std::optional<ResidencePreparation::SelectCandidate>
ResidencePreparation::MatchPinnedSelectPublication(const ir::Inst* publication) {
    if (!publication || publication->GetOp() != ir::OpCode::SetHostGPR ||
        publication->GetArg<ir::Imm>(2).Get() != 0 || IsDeadPublication(publication) ||
        allocation.IsHostWriteCoalesced(publication->Id())) {
        return std::nullopt;
    }

    const u32 target = publication->GetArg<ir::Imm>(1).Get();
    auto* extend = publication->GetArg<ir::Value>(0).Def();
    if (!IsFixedGPRHome(target) || !extend || extend->GetOp() != ir::OpCode::ZeroExtend32To64 ||
        extend->ReturnType() != ir::ValueType::U64) {
        return std::nullopt;
    }
    auto* producer = extend->GetArg<ir::Value>(0).Def();
    if (!producer || producer->GetOp() != ir::OpCode::SelectZero ||
        (producer->ReturnType() != ir::ValueType::U32 &&
         producer->ReturnType() != ir::ValueType::U64) ||
        producer->Id() >= extend->Id() || extend->Id() >= publication->Id() ||
        producer->GetUses(false) != 1) {
        return std::nullopt;
    }

    if (!register_state.PreparePublicationWindow(
                target, ir::Value{producer}, producer->Id(), publication->Id(), extend)) {
        return std::nullopt;
    }

    PrepareUseFacts().PrepareUses();
    bool saw_publication = false;
    u32 extend_uses = 0;
    u32 last_use = publication->Id();
    const auto consumers = UseFacts().Uses(extend);
    for (const auto& use : consumers) {
        auto& consumer = *use.consumer;
        const u32 uses = use.count;
        extend_uses += uses;
        if (&consumer == publication && uses == 1) {
            saw_publication = true;
            continue;
        }
        if (consumer.Id() <= publication->Id() || uses != 1 || !IsLow32Alias(consumer, extend)) {
            return std::nullopt;
        }
    }
    if (!saw_publication || extend_uses != extend->GetUses(false)) {
        return std::nullopt;
    }

    for (const auto& use : consumers) {
        auto* alias = use.consumer;
        if (alias == publication) continue;
        u32 alias_uses = 0;
        for (const auto& use : UseFacts().Uses(alias)) {
            auto& consumer = *use.consumer;
            const u32 uses = use.count;
            alias_uses += uses;
            if (!IsAliasConsumer(consumer, alias)) {
                return std::nullopt;
            }
            last_use = std::max<u32>(last_use, consumer.Id());
        }
        if (alias_uses != alias->GetUses(false)) {
            return std::nullopt;
        }
    }

    if (!register_state.PrepareHomeSurvival(target, publication->Id(), last_use)) {
        return std::nullopt;
    }

    return SelectCandidate{
            .producer = producer,
            .extend = extend,
            .consumers = consumers,
            .target = static_cast<u16>(target),
            .last_use = last_use,
    };
}

void ResidencePreparation::PreparePinnedSelectPublications() {
    for (const auto position : PreparePublicationCandidates()) {
        const auto& inst = *UseFacts().Instructions()[position];
        auto candidate = MatchPinnedSelectPublication(&inst);
        if (!candidate ||
            allocation.PrepareFixedGPRHomes().Conflicts(candidate->target,
                                                        candidate->producer->Id(),
                                                        inst.Id(),
                                                        candidate->producer->Id()) ||
            HasValueHome(candidate->producer) || HasLowView(candidate->extend) ||
            std::ranges::any_of(candidate->consumers, [&](const auto& use) {
                return use.consumer != &inst && HasLowView(use.consumer);
            })) {
            continue;
        }
        if (!allocation.GetGprs().Get(candidate->target)) continue;
        Vector<u32> component;
        if (!AppendRecolorValue(component, candidate->producer) ||
            !AppendRecolorValue(component, candidate->extend))
            continue;
        bool component_valid = true;
        for (const auto& use : candidate->consumers) {
            auto* alias = use.consumer;
            if (alias == &inst) continue;
            if (allocation.IsWidthChainCoalesced(alias->Id()) ||
                allocation.IsLow32CopyCoalesced(alias->Id())) {
                if (!AppendRecolorValue(component, alias)) {
                    component_valid = false;
                    break;
                }
            }
        }
        if (!component_valid ||
            !RecolorPublication(component, candidate->target, inst.Id(), candidate->last_use))
            continue;
        AssignValueHome(candidate->producer, candidate->target);
        residences.Record(candidate->extend, GPRResidenceAction::ZeroExtend);
        for (const auto& use : candidate->consumers) {
            if (use.consumer != &inst) AssignLowView(use.consumer, candidate->target);
        }
        SelectPublication({&inst,
                           candidate->producer,
                           UINT16_MAX,
                           candidate->target,
                           0,
                           PublicationSelection::Kind::Select});
    }
}

}  // namespace swift::runtime::backend::arm64

namespace swift::runtime::backend::arm64 {

namespace {

using ::swift::runtime::backend::IsFixedGPRHome;

std::optional<u32> LowViewLastUse(const compile::BlockAnalysisIndex& index,
                                  const ir::Inst* alias,
                                  const ir::Inst* value) {
    if (alias->GetOp() != ir::OpCode::BitExtract || alias->GetArg<ir::Value>(0).Def() != value ||
        alias->GetArg<ir::Imm>(1).Get() != 0) {
        return std::nullopt;
    }
    const u32 width = ir::GetValueSizeByte(alias->ReturnType());
    if ((width != sizeof(u8) && width != sizeof(u16) && width != sizeof(u32)) ||
        alias->GetArg<ir::Imm>(2).Get() != width * 8) {
        return std::nullopt;
    }
    u32 ordinary_uses{};
    u32 last_use = alias->Id();
    for (const auto& use : index.Uses(alias)) {
        auto& consumer = *use.consumer;
        const u32 uses = use.count;
        const auto op = consumer.GetOp();
        if (uses != 1 ||
            (op != ir::OpCode::Add && op != ir::OpCode::Sub && op != ir::OpCode::Select) ||
            ir::GetValueSizeByte(consumer.ReturnType()) != width) {
            return std::nullopt;
        }
        ordinary_uses += uses;
        last_use = std::max<u32>(last_use, consumer.Id());
    }
    return ordinary_uses != 0 && ordinary_uses == alias->GetUses(false)
                   ? std::optional<u32>{last_use}
                   : std::nullopt;
}

}  // namespace

std::optional<ResidencePreparation::PublicationViewCandidate>
ResidencePreparation::MatchPinnedGPRPublicationView(const ir::Inst* publication) {
    if (!publication || publication->GetOp() != ir::OpCode::SetHostGPR ||
        publication->GetArg<ir::Imm>(2).Get() != 0 || IsDeadPublication(publication)) {
        return std::nullopt;
    }
    const u32 target = publication->GetArg<ir::Imm>(1).Get();
    const auto published = publication->GetArg<ir::Value>(0);
    auto* value = published.Def();
    if (!IsFixedGPRHome(target) || !value || value->Id() >= publication->Id() ||
        ir::GetValueSizeByte(published.Type()) != sizeof(u64) ||
        ir::GetValueSizeByte(value->ReturnType()) != sizeof(u64) ||
        value->GetOp() == ir::OpCode::GetHostGPR ||
        value->GetOp() == ir::OpCode::ZeroExtend32To64) {
        return std::nullopt;
    }

    PrepareUseFacts().PrepareUses();
    bool saw_publication = false;
    u32 ordinary_uses{};
    u32 last_use = publication->Id();
    const auto consumers = UseFacts().Uses(value);
    for (const auto& use : consumers) {
        auto& consumer = *use.consumer;
        const u32 uses = use.count;
        ordinary_uses += uses;
        if (&consumer == publication && uses == 1) {
            saw_publication = true;
            continue;
        }
        const auto alias_last_use = LowViewLastUse(UseFacts(), &consumer, value);
        if (consumer.Id() <= publication->Id() || uses != 1 || !alias_last_use) {
            return std::nullopt;
        }
        last_use = std::max(last_use, *alias_last_use);
    }
    if (!saw_publication || consumers.size() <= 1 || ordinary_uses != value->GetUses(false)) {
        return std::nullopt;
    }
    if (!register_state.PrepareHomeSurvival(target, publication->Id(), last_use)) {
        return std::nullopt;
    }
    return PublicationViewCandidate{
            .consumers = consumers,
            .target = static_cast<u16>(target),
    };
}

void ResidencePreparation::PreparePinnedGPRPublicationViews() {
    for (const auto position : PreparePublicationCandidates()) {
        const auto& inst = *UseFacts().Instructions()[position];
        auto candidate = MatchPinnedGPRPublicationView(&inst);
        if (!candidate || std::ranges::any_of(candidate->consumers, [&](const auto& use) {
                return use.consumer != &inst && HasLowView(use.consumer);
            })) {
            continue;
        }
        for (const auto& use : candidate->consumers) {
            if (use.consumer != &inst) AssignLowView(use.consumer, candidate->target);
        }
    }
}

}  // namespace swift::runtime::backend::arm64

namespace swift::runtime::backend::arm64 {

namespace {

using ::swift::runtime::backend::IsFixedGPRHome;

bool SupportsDirectPublication(ir::OpCode op) {
    using O = ir::OpCode;
    return op == O::LoadImm || op == O::LoadMemory || op == O::Add || op == O::Sub ||
           op == O::And || op == O::GetOperand || op == O::SignExtend || op == O::BitExtract;
}

}  // namespace

std::optional<ResidencePreparation::PublicationSelection>
ResidencePreparation::MatchSpilledGPRPublication(const ir::Inst* publication) {
    if (!publication || publication->GetOp() != ir::OpCode::SetHostGPR ||
        publication->GetArg<ir::Imm>(2).Get() != 0 || IsDeadPublication(publication) ||
        allocation.IsHostWriteCoalesced(publication->Id())) {
        return std::nullopt;
    }

    const auto value = publication->GetArg<ir::Value>(0);
    auto* producer = value.Def();
    const u32 width = ir::GetValueSizeByte(value.Type());
    const u32 target = publication->GetArg<ir::Imm>(1).Get();
    if (!producer || !SupportsDirectPublication(producer->GetOp()) ||
        (width != sizeof(u32) && width != sizeof(u64)) ||
        ir::GetValueSizeByte(producer->ReturnType()) != width || !IsFixedGPRHome(target) ||
        allocation.ValueType(value) != AllocationPreparation::MEM ||
        producer->GetUses(false) != 1) {
        return std::nullopt;
    }
    // Function values can be spilled in a predecessor block. This local
    // recolor cannot adopt them, and must not walk another block's list.
    if (!UseFacts().Contains(producer)) return std::nullopt;

    auto& instructions = this->block->GetInstList();
    const auto producer_it = instructions.iterator_to(*producer);
    const auto publication_it = instructions.iterator_to(*publication);
    if (producer->Id() >= publication->Id() ||
        allocation.PrepareFixedGPRHomes().Conflicts(
                target, producer->Id(), publication->Id(), producer->Id()) ||
        !register_state.PreparePublicationWindow(target, value, producer->Id(), publication->Id())) {
        return std::nullopt;
    }
    for (auto it = std::next(producer_it); it != publication_it; ++it) {
        if (it == instructions.end() || it->GetOp() == ir::OpCode::Goto ||
            it->GetOp() == ir::OpCode::NotGoto || it->GetOp() == ir::OpCode::BindLabel ||
            (backend::FixedGPRClobbers(*it, features, true) & (1u << target))) {
            return std::nullopt;
        }
        for (auto input : it->GetValues()) {
            if (input.Defined() && IsGPRMappedTo(input, target)) {
                return std::nullopt;
            }
        }
        if (it->HasValue() && IsGPRMappedTo(ir::Value{it.operator->()}, target)) {
            return std::nullopt;
        }
    }

    return PublicationSelection{
            .publication = publication,
            .producer = producer,
            .target = static_cast<u16>(target),
            .kind = PublicationSelection::Kind::Spill,
    };
}

void ResidencePreparation::PrepareSpilledGPRPublications() {
    ASSERT(!finished);
    for (const auto position : PreparePublicationCandidates()) {
        const auto& inst = *UseFacts().Instructions()[position];
        auto candidate = MatchSpilledGPRPublication(&inst);
        if (!candidate || HasValueHome(candidate->producer)) {
            continue;
        }
        if (!allocation.GetGprs().Get(candidate->target)) continue;
        Vector<u32> component;
        if (!AppendRecolorValue(component, candidate->producer)) continue;
        if (!RecolorPublication(component, candidate->target, inst.Id(), inst.Id())) continue;
        AssignValueHome(candidate->producer, candidate->target);
        SelectPublication(*candidate);
    }
}

}  // namespace swift::runtime::backend::arm64
