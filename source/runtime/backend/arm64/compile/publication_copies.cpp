#include <algorithm>
#include <iterator>
#include "residence_preparation.h"
#include "runtime/backend/arm64/compile/allocation_preparation.h"
#include "runtime/backend/arm64/compile/register_contract.h"

#include "runtime/backend/arm64/helper_call_contract.h"

namespace swift::runtime::backend::arm64 {

namespace {

using ::swift::runtime::backend::IsFixedGPRHome;

bool IsMemoryAddressUse(const ir::Inst& inst, const ir::Inst* definition) {
    if (inst.GetOp() != ir::OpCode::LoadMemory && inst.GetOp() != ir::OpCode::StoreMemory) {
        return false;
    }
    const auto address = inst.GetArg<ir::Operand>(0);
    const auto left = address.GetLeft();
    const auto right = address.GetRight();
    return (left.IsValue() && left.value.Def() == definition) ||
           (right.IsValue() && right.value.Def() == definition);
}

bool IsLowAluAlias(const compile::BlockAnalysisIndex& index,
                   const ir::Inst* alias,
                   const ir::Inst* definition) {
    if (alias->GetOp() != ir::OpCode::BitExtract ||
        alias->GetArg<ir::Value>(0).Def() != definition || alias->GetArg<ir::Imm>(1).Get() != 0) {
        return false;
    }
    const u32 width = ir::GetValueSizeByte(alias->ReturnType());
    if (alias->GetArg<ir::Imm>(2).Get() != width * 8 ||
        (width != sizeof(u8) && width != sizeof(u16) && width != sizeof(u32)) ||
        alias->GetUses() != 1) {
        return false;
    }
    for (const auto& use : index.Uses(alias)) {
        auto& consumer = *use.consumer;
        const u32 uses = use.count;
        const bool low_alu =
                uses == 1 &&
                (consumer.GetOp() == ir::OpCode::Add || consumer.GetOp() == ir::OpCode::Sub) &&
                ir::GetValueSizeByte(consumer.ReturnType()) == width;
        const bool logical_zero_test =
                uses == 1 && consumer.GetOp() == ir::OpCode::Or &&
                ir::GetValueSizeByte(consumer.ReturnType()) == width && consumer.GetUses() == 0 &&
                consumer.GetArg<ir::Operand>(1).IsImm() &&
                consumer.GetArg<ir::Operand>(1).GetLeft().imm.Get() == 0 &&
                (index.HasPseudoUse(&consumer, ir::OpCode::BranchOnlyFlags) ||
                 index.HasPseudoUse(&consumer, ir::OpCode::SaveFlags));
        const bool select_value = uses == 1 && consumer.GetOp() == ir::OpCode::Select &&
                                  ir::GetValueSizeByte(consumer.ReturnType()) == width;
        return low_alu || logical_zero_test || select_value;
    }
    return false;
}

bool IsReusablePinnedLowAlias(const compile::BlockAnalysisIndex& index,
                              const ir::Inst* alias,
                              const ir::Inst* definition) {
    if (alias->GetOp() != ir::OpCode::BitExtract ||
        alias->GetArg<ir::Value>(0).Def() != definition || alias->GetArg<ir::Imm>(1).Get() != 0 ||
        alias->GetArg<ir::Imm>(2).Get() != 32 ||
        ir::GetValueSizeByte(alias->ReturnType()) != sizeof(u32)) {
        return false;
    }

    u32 uses = 0;
    for (const auto& use : index.Uses(alias)) {
        auto& consumer = *use.consumer;
        const u32 consumer_uses = use.count;
        uses += consumer_uses;
        const bool sign_extend = consumer_uses == 1 && consumer.GetOp() == ir::OpCode::SignExtend &&
                                 consumer.GetArg<ir::Value>(0).Def() == alias;
        const bool mul_left = consumer_uses == 1 && consumer.GetOp() == ir::OpCode::Mul &&
                              consumer.GetArg<ir::Value>(0).Def() == alias &&
                              ir::GetValueSizeByte(consumer.ReturnType()) == sizeof(u32);
        if (!sign_extend && !mul_left) {
            return false;
        }
    }
    return uses != 0 && uses == alias->GetUses();
}

bool IsU32AluUse(const ir::Inst& consumer, const ir::Inst* definition) {
    if (consumer.GetOp() != ir::OpCode::Add && consumer.GetOp() != ir::OpCode::Sub &&
        consumer.GetOp() != ir::OpCode::And && consumer.GetOp() != ir::OpCode::Or &&
        consumer.GetOp() != ir::OpCode::Xor) {
        return false;
    }
    u32 uses = 0;
    for (auto value : consumer.GetValues()) {
        uses += value.Def() == definition;
    }
    return uses == 1 && ir::GetValueSizeByte(consumer.ReturnType()) == sizeof(u32);
}

bool IsU32SelectUse(const ir::Inst& consumer, const ir::Inst* definition) {
    if (consumer.GetOp() != ir::OpCode::Select ||
        ir::GetValueSizeByte(consumer.ReturnType()) != sizeof(u32)) {
        return false;
    }
    return std::ranges::count_if(consumer.GetValues(),
                                 [&](ir::Value value) { return value.Def() == definition; }) == 1;
}

bool IsPinnedU64AliasUse(const compile::BlockAnalysisIndex& index, const ir::Inst& consumer) {
    if (consumer.GetOp() != ir::OpCode::Add && consumer.GetOp() != ir::OpCode::GetOperand) {
        return false;
    }
    if (ir::GetValueSizeByte(consumer.ReturnType()) != sizeof(u64)) {
        return false;
    }
    if (consumer.GetOp() == ir::OpCode::Add) {
        return true;
    }
    if (consumer.GetUses() == 0) {
        return false;
    }

    u32 memory_uses = 0;
    for (const auto& use : index.Uses(&consumer)) {
        if (!IsMemoryAddressUse(*use.consumer, &consumer)) return false;
        memory_uses += use.count;
    }
    return memory_uses == consumer.GetUses();
}

}  // namespace

template <typename Visitor>
bool ResidencePreparation::VisitCopyAliases(const CopyCandidate& candidate,
                                             const ir::Inst* publication,
                                             Visitor&& visit) const {
    // Matching proves the roles first: narrow/read aliases are BitExtracts;
    // their other accepted consumers are direct U32 uses. Every remaining
    // consumer of the outer extension is an alias except the publication.
    const auto extracts = [&](const ir::Inst* definition) {
        if (!definition) return true;
        for (const auto& use : UseFacts().Uses(definition)) {
            if (use.consumer->GetOp() != ir::OpCode::BitExtract) continue;
            for (u32 occurrence = 0; occurrence < use.count; ++occurrence)
                if (!visit(use.consumer)) return false;
        }
        return true;
    };
    if (!extracts(candidate.narrow_extend)) return false;
    if (candidate.read->GetOp() == ir::OpCode::Add && !extracts(candidate.read)) return false;
    for (const auto& use : UseFacts().Uses(candidate.extend)) {
        if (use.consumer == publication) continue;
        for (u32 occurrence = 0; occurrence < use.count; ++occurrence)
            if (!visit(use.consumer)) return false;
    }
    return true;
}

void ResidencePreparation::RegisterCopyUses(const CopyCandidate& candidate) {
    const auto record = [&](const ir::Inst* definition, const auto& use) {
        for (u32 occurrence = 0; occurrence < use.count; ++occurrence)
            RegisterUseHome(definition,
                            use.consumer,
                            RegisterStatePreparation::FixedHomeValue{
                                    .home = candidate.target,
                                    .width = sizeof(u32),
                                    .extension = {.known_zero_above = 32},
                            });
    };
    if (candidate.narrow_extend) {
        for (const auto& use : UseFacts().Uses(candidate.narrow_extend))
            if (use.consumer != candidate.extend &&
                use.consumer->GetOp() != ir::OpCode::BitExtract)
                record(candidate.narrow_extend, use);
    }
    if (candidate.source && candidate.read->GetUses() != 1) {
        for (const auto& use : UseFacts().Uses(candidate.read))
            if (use.consumer != candidate.extend) record(candidate.read, use);
    }
}

std::optional<ResidencePreparation::CopyCandidate> ResidencePreparation::MatchPinnedGPRCopy(
        const ir::Inst* inst) {
    if (!inst || inst->GetOp() != ir::OpCode::SetHostGPR || inst->GetArg<ir::Imm>(2).Get() != 0 ||
        IsDeadPublication(inst)) {
        return std::nullopt;
    }

    const u32 target = inst->GetArg<ir::Imm>(1).Get();
    auto published = inst->GetArg<ir::Value>(0);
    auto* extend = published.Def();
    if (!IsFixedGPRHome(target) || !extend || extend->GetOp() != ir::OpCode::ZeroExtend32To64 ||
        ir::GetValueSizeByte(published.Type()) != sizeof(u64)) {
        return std::nullopt;
    }

    auto source = extend->GetArg<ir::Value>(0);
    const ir::Inst* narrow_extend{};
    bool signed_load = false;
    if (source.Def() && (source.Def()->GetOp() == ir::OpCode::ZeroExtend32 ||
                         source.Def()->GetOp() == ir::OpCode::SignExtend)) {
        narrow_extend = source.Def();
        if (ir::GetValueSizeByte(source.Type()) > sizeof(u32)) {
            return std::nullopt;
        }
        source = narrow_extend->GetArg<ir::Value>(0);
        signed_load = narrow_extend->GetOp() == ir::OpCode::SignExtend;
        if (signed_load && ir::GetValueSizeByte(source.Type()) > sizeof(u16)) {
            return std::nullopt;
        }
    }
    auto* read = source.Def();
    const u32 source_width = ir::GetValueSizeByte(source.Type());
    if (!read || (source_width != sizeof(u8) && source_width != sizeof(u16) &&
                  source_width != sizeof(u32))) {
        return std::nullopt;
    }
    const bool load_source = read->GetOp() == ir::OpCode::LoadMemory;
    const bool add_source = read->GetOp() == ir::OpCode::Add && source_width == sizeof(u32) &&
                            allocation.ValueType(source) == AllocationPreparation::MEM &&
                            !read->HasPseudoOperations();
    std::optional<u16> source_index;
    if (!load_source && (signed_load || allocation.IsHostWriteCoalesced(inst->Id()))) {
        return std::nullopt;
    }
    if (!load_source && !add_source) {
        if (read->GetOp() != ir::OpCode::GetHostGPR || read->GetArg<ir::Imm>(1).Get() != 0 ||
            allocation.IsHostReadCoalesced(read->Id())) {
            return std::nullopt;
        }
        const u32 index = read->GetArg<ir::Imm>(0).Get();
        if (!IsFixedGPRHome(index)) {
            return std::nullopt;
        }
        source_index = static_cast<u16>(index);
    }

    u32 producer_last_use = inst->Id();
    PrepareUseFacts().PrepareUses();
    if (narrow_extend) {
        bool saw_extend = false;
        u32 narrow_uses = 0;
        for (const auto& use : UseFacts().Uses(narrow_extend)) {
            auto& scan = *use.consumer;
            for (u32 occurrence = 0; occurrence < use.count; ++occurrence) {
                ++narrow_uses;
                if (&scan == extend) {
                    saw_extend = true;
                    continue;
                }
                if (scan.Id() > inst->Id() &&
                    (IsU32AluUse(scan, narrow_extend) || IsU32SelectUse(scan, narrow_extend))) {
                    producer_last_use = std::max<u32>(producer_last_use, scan.Id());
                    continue;
                }
                const bool reusable_alias =
                        IsLowAluAlias(UseFacts(), &scan, narrow_extend) ||
                        (signed_load && IsReusablePinnedLowAlias(UseFacts(), &scan, narrow_extend));
                if (!reusable_alias || scan.Id() <= inst->Id()) {
                    return std::nullopt;
                }
            }
        }
        if (!saw_extend || narrow_uses != narrow_extend->GetUses(false)) {
            return std::nullopt;
        }
    }
    if (add_source) {
        bool saw_extend = false;
        u32 source_uses = 0;
        for (const auto& use : UseFacts().Uses(read)) {
            auto& scan = *use.consumer;
            for (u32 occurrence = 0; occurrence < use.count; ++occurrence) {
                ++source_uses;
                if (&scan == extend) {
                    saw_extend = true;
                    continue;
                }
                if (scan.Id() > inst->Id() && IsU32AluUse(scan, read)) {
                    producer_last_use = std::max<u32>(producer_last_use, scan.Id());
                    continue;
                }
                if (!IsLowAluAlias(UseFacts(), &scan, read) || scan.Id() <= inst->Id()) {
                    return std::nullopt;
                }
            }
        }
        if (!saw_extend || source_uses != read->GetUses()) {
            return std::nullopt;
        }
    } else if (read->GetUses() != 1) {
        if (!source_index || source_width != sizeof(u32)) {
            return std::nullopt;
        }
        bool saw_extend = false;
        u32 source_uses = 0;
        for (const auto& use : UseFacts().Uses(read)) {
            auto& scan = *use.consumer;
            for (u32 occurrence = 0; occurrence < use.count; ++occurrence) {
                ++source_uses;
                if (&scan == extend) {
                    saw_extend = true;
                    continue;
                }
                if (scan.Id() <= inst->Id() || !IsU32AluUse(scan, read)) {
                    return std::nullopt;
                }
                producer_last_use = std::max<u32>(producer_last_use, scan.Id());
            }
        }
        if (!saw_extend || source_uses != read->GetUses()) {
            return std::nullopt;
        }
    }

    bool saw_publication = false;
    u32 published_uses = 0;
    for (const auto& use : UseFacts().Uses(extend)) {
        auto& scan = *use.consumer;
        for (u32 occurrence = 0; occurrence < use.count; ++occurrence) {
            ++published_uses;
            if (&scan == inst && scan.GetArg<ir::Value>(0).Def() == extend) {
                saw_publication = true;
                continue;
            }
            const bool bitcast = scan.GetOp() == ir::OpCode::BitCast &&
                                 scan.GetArg<ir::Value>(0).Def() == extend;
            const bool low32_alias =
                    IsLowAluAlias(UseFacts(), &scan, extend) ||
                    (signed_load && IsReusablePinnedLowAlias(UseFacts(), &scan, extend));
            if ((!bitcast && !low32_alias) || scan.Id() <= inst->Id()) {
                return std::nullopt;
            }
        }
    }
    if (!saw_publication || published_uses != extend->GetUses()) {
        return std::nullopt;
    }

    CopyCandidate candidate{
            .read = read,
            .narrow_extend = narrow_extend,
            .extend = extend,
            .signed_load = signed_load,
            .source = source_index,
            .target = static_cast<u16>(target),
            .width = static_cast<u8>(source_width),
    };
    u32 last_use = producer_last_use;
    const bool aliases_safe = VisitCopyAliases(candidate, inst, [&](const ir::Inst* alias) {
        u32 alias_uses = 0;
        for (const auto& use : UseFacts().Uses(alias)) {
            auto& scan = *use.consumer;
            if (IsMemoryAddressUse(scan, alias)) {
                ++alias_uses;
                last_use = std::max<u32>(last_use, scan.Id());
            } else if (alias->GetOp() == ir::OpCode::BitExtract ||
                       alias->GetOp() == ir::OpCode::BitCast) {
                for (u32 occurrence = 0; occurrence < use.count; ++occurrence) {
                    {
                        if (alias->GetOp() == ir::OpCode::BitCast &&
                            (!source_index || *source_index != target ||
                             !IsPinnedU64AliasUse(UseFacts(), scan))) {
                            return false;
                        }
                        ++alias_uses;
                        last_use = std::max<u32>(last_use, scan.Id());
                    }
                }
            }
        }
        return alias_uses != 0 && alias_uses == alias->GetUses();
    });
    if (!aliases_safe) return std::nullopt;

    if (read->Id() >= extend->Id() || extend->Id() >= inst->Id()) {
        return std::nullopt;
    }
    if (source_index && !register_state.PrepareHomeSurvival(*source_index, read->Id(), inst->Id()))
        return std::nullopt;
    for (auto* scan : UseFacts().Between(read->Id(), inst->Id())) {
        if (MayFaultOrObserve(*scan) ||
            (scan->GetOp() == ir::OpCode::SetHostGPR &&
             (scan->GetArg<ir::Imm>(1).Get() == target ||
              (source_index && scan->GetArg<ir::Imm>(1).Get() == *source_index))))
            return std::nullopt;
    }
    for (auto* scan : UseFacts().Between(inst->Id(), last_use)) {
        if ((scan->GetOp() == ir::OpCode::SetHostGPR && scan->GetArg<ir::Imm>(1).Get() == target) ||
            (target <= 9 && HelperCallContract::InstructionClobbersGPR(*scan, target, features)))
            return std::nullopt;
    }
    candidate.last_use = last_use;
    return candidate;
}

void ResidencePreparation::PreparePinnedGPRCopies() {
    for (const auto position : PreparePublicationCandidates()) {
        const auto& inst = *UseFacts().Instructions()[position];
        auto candidate = MatchPinnedGPRCopy(&inst);
        if (!candidate) {
            continue;
        }
        if (!candidate->source &&
            allocation.PrepareFixedGPRHomes().Conflicts(
                    candidate->target, candidate->read->Id(), inst.Id(), candidate->read->Id()))
            continue;
        if (!candidate->source) {
            if (!allocation.GetGprs().Get(candidate->target)) continue;
            Vector<u32> component;
            if (!AppendRecolorValue(component, candidate->read) ||
                !AppendRecolorValue(component, candidate->extend) ||
                !AppendRecolorValue(component, candidate->narrow_extend) ||
                !VisitCopyAliases(*candidate, &inst, [&](const ir::Inst* alias) {
                    if (!alias->IsBitCastOperation() &&
                        !allocation.IsWidthChainCoalesced(alias->Id()) &&
                        !allocation.IsLow32CopyCoalesced(alias->Id()))
                        return true;
                    return AppendRecolorValue(component, alias);
                }))
                continue;
            if (!RecolorPublication(component, candidate->target, inst.Id(), candidate->last_use))
                continue;
            AssignValueHome(candidate->read, candidate->target);
            if (candidate->read->GetOp() == ir::OpCode::Add) {
                AssignLowView(candidate->read, candidate->target);
            }
        } else {
            AssignLowView(candidate->read, *candidate->source);
        }
        if (candidate->narrow_extend) {
            if (candidate->signed_load) {
                residences.Record(candidate->narrow_extend, GPRResidenceAction::SignExtend);
            } else {
                residences.Record(candidate->narrow_extend, GPRResidenceAction::ZeroExtend);
            }
        }
        residences.Record(candidate->extend, GPRResidenceAction::ZeroExtend);
        VisitCopyAliases(*candidate, &inst, [&](const ir::Inst* alias) {
            if (alias->GetOp() == ir::OpCode::BitExtract) {
                AssignLowView(alias, candidate->target);
            } else {
                AssignValueHome(alias, candidate->target);
            }
            return true;
        });
        RegisterCopyUses(*candidate);
        SelectPublication({&inst,
                           candidate->read,
                           candidate->source.value_or(UINT16_MAX),
                           candidate->target,
                           candidate->width,
                           PublicationSelection::Kind::Copy});
    }
}

}  // namespace swift::runtime::backend::arm64
