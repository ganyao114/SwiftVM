#include "runtime/backend/reg_alloc.h"
#include "translator.h"

#include "runtime/backend/arm64/helper_call_contract.h"

namespace swift::runtime::backend::arm64 {

namespace {

using ::swift::runtime::backend::IsFixedGPRHome;

bool IsMemoryAddressUse(ir::Inst& inst, ir::Inst* definition) {
    if (inst.GetOp() != ir::OpCode::LoadMemory && inst.GetOp() != ir::OpCode::StoreMemory) {
        return false;
    }
    const auto address = inst.GetArg<ir::Operand>(0);
    const auto left = address.GetLeft();
    const auto right = address.GetRight();
    return (left.IsValue() && left.value.Def() == definition) ||
           (right.IsValue() && right.value.Def() == definition);
}

bool IsLowAluAlias(ir::Block* block, ir::Inst* alias, ir::Inst* definition) {
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
    for (auto& consumer : block->GetInstList()) {
        u32 uses = 0;
        for (auto value : consumer.GetValues()) {
            uses += value.Def() == alias;
        }
        if (!uses) {
            continue;
        }
        const bool low_alu =
                uses == 1 &&
                (consumer.GetOp() == ir::OpCode::Add || consumer.GetOp() == ir::OpCode::Sub) &&
                ir::GetValueSizeByte(consumer.ReturnType()) == width;
        const bool logical_zero_test =
                uses == 1 && consumer.GetOp() == ir::OpCode::Or &&
                ir::GetValueSizeByte(consumer.ReturnType()) == width && consumer.GetUses() == 0 &&
                consumer.GetArg<ir::Operand>(1).IsImm() &&
                consumer.GetArg<ir::Operand>(1).GetLeft().imm.Get() == 0 &&
                (!consumer.GetPseudoOperations(ir::OpCode::BranchOnlyFlags).empty() ||
                 !consumer.GetPseudoOperations(ir::OpCode::SaveFlags).empty());
        const bool select_value = uses == 1 && consumer.GetOp() == ir::OpCode::Select &&
                                  ir::GetValueSizeByte(consumer.ReturnType()) == width;
        return low_alu || logical_zero_test || select_value;
    }
    return false;
}

bool IsReusablePinnedLowAlias(ir::Block* block, ir::Inst* alias, ir::Inst* definition) {
    if (alias->GetOp() != ir::OpCode::BitExtract ||
        alias->GetArg<ir::Value>(0).Def() != definition || alias->GetArg<ir::Imm>(1).Get() != 0 ||
        alias->GetArg<ir::Imm>(2).Get() != 32 ||
        ir::GetValueSizeByte(alias->ReturnType()) != sizeof(u32)) {
        return false;
    }

    u32 uses = 0;
    for (auto& consumer : block->GetInstList()) {
        u32 consumer_uses = 0;
        for (auto value : consumer.GetValues()) {
            consumer_uses += value.Def() == alias;
        }
        if (!consumer_uses) {
            continue;
        }
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

bool IsU32AluUse(ir::Inst& consumer, ir::Inst* definition) {
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

bool IsU32SelectUse(ir::Inst& consumer, ir::Inst* definition) {
    if (consumer.GetOp() != ir::OpCode::Select ||
        ir::GetValueSizeByte(consumer.ReturnType()) != sizeof(u32)) {
        return false;
    }
    return std::ranges::count_if(consumer.GetValues(),
                                 [&](ir::Value value) { return value.Def() == definition; }) == 1;
}

bool IsPinnedU64AliasUse(ir::Block* block, ir::Inst& consumer) {
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
    for (auto& scan : block->GetInstList()) {
        for (auto used : scan.GetValues()) {
            if (used.Def() != &consumer) {
                continue;
            }
            if (!IsMemoryAddressUse(scan, &consumer)) {
                return false;
            }
            ++memory_uses;
        }
    }
    return memory_uses == consumer.GetUses();
}

}  // namespace

std::optional<JitTranslator::PinnedGPRCopy> JitTranslator::MatchPinnedGPRCopy(
        ir::Inst* inst) const {
    if (!inst || inst->GetOp() != ir::OpCode::SetHostGPR || inst->GetArg<ir::Imm>(2).Get() != 0 ||
        pinned_gprs.dead_pinned_gpr_writes.contains(inst)) {
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
    ir::Inst* narrow_extend{};
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
                            context.IsSpilled(source) && read->GetPseudoOperations().empty();
    std::optional<u16> source_index;
    if (!load_source && (signed_load || context.IsHostWriteCoalesced(inst->Id()))) {
        return std::nullopt;
    }
    if (!load_source && !add_source) {
        if (read->GetOp() != ir::OpCode::GetHostGPR || read->GetArg<ir::Imm>(1).Get() != 0 ||
            context.IsHostReadCoalesced(read->Id())) {
            return std::nullopt;
        }
        const u32 index = read->GetArg<ir::Imm>(0).Get();
        if (!IsFixedGPRHome(index)) {
            return std::nullopt;
        }
        source_index = static_cast<u16>(index);
    }

    std::vector<ir::Inst*> aliases;
    std::vector<std::pair<ir::Inst*, ir::Inst*>> transferred_uses;
    u32 producer_last_use = inst->Id();
    if (narrow_extend) {
        bool saw_extend = false;
        u32 narrow_uses = 0;
        for (auto& scan : cur_block->GetInstList()) {
            for (auto used : scan.GetValues()) {
                if (used.Def() != narrow_extend) {
                    continue;
                }
                ++narrow_uses;
                if (&scan == extend) {
                    saw_extend = true;
                    continue;
                }
                if (scan.Id() > inst->Id() &&
                    (IsU32AluUse(scan, narrow_extend) || IsU32SelectUse(scan, narrow_extend))) {
                    transferred_uses.emplace_back(narrow_extend, &scan);
                    producer_last_use = std::max<u32>(producer_last_use, scan.Id());
                    continue;
                }
                const bool reusable_alias =
                        IsLowAluAlias(cur_block, &scan, narrow_extend) ||
                        (signed_load && IsReusablePinnedLowAlias(cur_block, &scan, narrow_extend));
                if (!reusable_alias || scan.Id() <= inst->Id()) {
                    return std::nullopt;
                }
                aliases.push_back(&scan);
            }
        }
        if (!saw_extend || narrow_uses != narrow_extend->GetUses(false)) {
            return std::nullopt;
        }
    }
    if (add_source) {
        bool saw_extend = false;
        u32 source_uses = 0;
        for (auto& scan : cur_block->GetInstList()) {
            for (auto used : scan.GetValues()) {
                if (used.Def() != read) {
                    continue;
                }
                ++source_uses;
                if (&scan == extend) {
                    saw_extend = true;
                    continue;
                }
                if (scan.Id() > inst->Id() && IsU32AluUse(scan, read)) {
                    producer_last_use = std::max<u32>(producer_last_use, scan.Id());
                    continue;
                }
                if (!IsLowAluAlias(cur_block, &scan, read) || scan.Id() <= inst->Id()) {
                    return std::nullopt;
                }
                aliases.push_back(&scan);
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
        for (auto& scan : cur_block->GetInstList()) {
            for (auto used : scan.GetValues()) {
                if (used.Def() != read) {
                    continue;
                }
                ++source_uses;
                if (&scan == extend) {
                    saw_extend = true;
                    continue;
                }
                if (scan.Id() <= inst->Id() || !IsU32AluUse(scan, read)) {
                    return std::nullopt;
                }
                transferred_uses.emplace_back(read, &scan);
                producer_last_use = std::max<u32>(producer_last_use, scan.Id());
            }
        }
        if (!saw_extend || source_uses != read->GetUses()) {
            return std::nullopt;
        }
    }

    bool saw_publication = false;
    u32 published_uses = 0;
    for (auto& scan : cur_block->GetInstList()) {
        for (auto used : scan.GetValues()) {
            if (used.Def() != extend) {
                continue;
            }
            ++published_uses;
            if (&scan == inst && scan.GetArg<ir::Value>(0).Def() == extend) {
                saw_publication = true;
                continue;
            }
            const bool bitcast = scan.GetOp() == ir::OpCode::BitCast &&
                                 scan.GetArg<ir::Value>(0).Def() == extend;
            const bool low32_alias =
                    IsLowAluAlias(cur_block, &scan, extend) ||
                    (signed_load && IsReusablePinnedLowAlias(cur_block, &scan, extend));
            if ((!bitcast && !low32_alias) || scan.Id() <= inst->Id()) {
                return std::nullopt;
            }
            aliases.push_back(&scan);
        }
    }
    if (!saw_publication || published_uses != extend->GetUses()) {
        return std::nullopt;
    }

    u32 last_use = producer_last_use;
    for (auto* alias : aliases) {
        u32 alias_uses = 0;
        for (auto& scan : cur_block->GetInstList()) {
            if (IsMemoryAddressUse(scan, alias)) {
                ++alias_uses;
                last_use = std::max<u32>(last_use, scan.Id());
            } else if (alias->GetOp() == ir::OpCode::BitExtract ||
                       alias->GetOp() == ir::OpCode::BitCast) {
                for (auto used : scan.GetValues()) {
                    if (used.Def() == alias) {
                        if (alias->GetOp() == ir::OpCode::BitCast &&
                            (!source_index || *source_index != target ||
                             !IsPinnedU64AliasUse(cur_block, scan))) {
                            return std::nullopt;
                        }
                        ++alias_uses;
                        last_use = std::max<u32>(last_use, scan.Id());
                    }
                }
            }
        }
        if (alias_uses == 0 || alias_uses != alias->GetUses()) {
            return std::nullopt;
        }
    }

    if (read->Id() >= extend->Id() || extend->Id() >= inst->Id()) {
        return std::nullopt;
    }
    for (auto& scan : cur_block->GetInstList()) {
        if (read->Id() < scan.Id() && scan.Id() < inst->Id() &&
            (MayFaultOrObserve(scan) ||
             (scan.GetOp() == ir::OpCode::SetHostGPR &&
              (scan.GetArg<ir::Imm>(1).Get() == target ||
               (source_index && scan.GetArg<ir::Imm>(1).Get() == *source_index))))) {
            return std::nullopt;
        }
        if (inst->Id() < scan.Id() && scan.Id() < last_use &&
            ((scan.GetOp() == ir::OpCode::SetHostGPR && scan.GetArg<ir::Imm>(1).Get() == target) ||
             (target <= 9 &&
              HelperCallContract::InstructionClobbersGPR(scan, target, context.GetFeatures())))) {
            return std::nullopt;
        }
    }
    return PinnedGPRCopy{
            .read = read,
            .narrow_extend = narrow_extend,
            .extend = extend,
            .signed_load = signed_load,
            .aliases = std::move(aliases),
            .transferred_uses = std::move(transferred_uses),
            .source = source_index,
            .target = static_cast<u16>(target),
            .width = static_cast<u8>(source_width),
            .last_use = last_use,
    };
}

std::optional<XRegister> JitTranslator::ResolvePinnedGPRValue(ir::Value value) const {
    if (!value.Def()) {
        return std::nullopt;
    }
    auto pinned = pinned_gprs.pinned_gpr_values.find(value.Def());
    if (pinned == pinned_gprs.pinned_gpr_values.end()) {
        return std::nullopt;
    }
    return XRegister(pinned->second);
}

std::optional<WRegister> JitTranslator::ResolvePinnedGPRWUse(ir::Value value,
                                                             const ir::Inst* consumer) const {
    if (!value.Def() || !consumer) {
        return std::nullopt;
    }
    const u32 width = ir::GetValueSizeByte(value.Type());
    const auto residence = guest_state_map.RegisteredFixedHomeForUse(
            value, consumer);
    if (residence && width <= sizeof(u32) && residence->width == width) {
        return WRegister(residence->home);
    }
    const auto pinned = pinned_gprs.fused_pin_gpr_reads.find(value.Def());
    if (pinned != pinned_gprs.fused_pin_gpr_reads.end()) {
        return WRegister(pinned->second);
    }
    const auto inferred = guest_state_map.FixedHomeForUse(value, consumer);
    return inferred && width <= sizeof(u32) && inferred->width == width
            ? std::optional<WRegister>{WRegister(inferred->home)}
            : std::nullopt;
}

std::optional<Register> JitTranslator::ResolvePinnedGPRUse(ir::Value value,
                                                           const ir::Inst* consumer) const {
    if (const auto pinned = ResolvePinnedGPRValue(value)) {
        return Register{*pinned};
    }
    if (const auto pinned = ResolvePinnedGPRWUse(value, consumer)) {
        return Register{*pinned};
    }
    const auto residence = guest_state_map.FixedHomeForUse(value, consumer);
    if (residence && residence->width == sizeof(u64)) {
        return Register{XRegister(residence->home)};
    }
    return std::nullopt;
}

void JitTranslator::PreparePinnedGPRCopies(ir::Block* block) {
    fused_pin_zext32.clear();
    checked_pin_zext32_publications.clear();
    fused_pin_sign_extends.clear();
    for (auto& inst : block->GetInstList()) {
        auto candidate = MatchPinnedGPRCopy(&inst);
        if (!candidate) {
            continue;
        }
        if (!candidate->source) {
            pinned_gprs.pinned_gpr_values.emplace(candidate->read, candidate->target);
            if (candidate->read->GetOp() == ir::OpCode::Add) {
                pinned_gprs.fused_pin_gpr_reads.emplace(candidate->read, candidate->target);
            }
        } else {
            pinned_gprs.fused_pin_gpr_reads.emplace(candidate->read, *candidate->source);
        }
        if (candidate->narrow_extend) {
            if (candidate->signed_load) {
                fused_pin_sign_extends.insert(candidate->narrow_extend);
            } else {
                fused_pin_zext32.insert(candidate->narrow_extend);
            }
        }
        fused_pin_zext32.insert(candidate->extend);
        for (auto* alias : candidate->aliases) {
            if (alias->GetOp() == ir::OpCode::BitExtract) {
                pinned_gprs.fused_pin_gpr_reads.emplace(alias, candidate->target);
            } else {
                pinned_gprs.pinned_gpr_values.emplace(alias, candidate->target);
            }
        }
        for (auto [value, consumer] : candidate->transferred_uses) {
            guest_state_map.RegisterFixedHomeUse(
                    value,
                    consumer,
                    GuestStateMap::FixedHomeValue{
                            .home = candidate->target,
                            .width = sizeof(u32),
                            .extension = {.known_zero_above = 32},
                    });
        }
        pinned_gprs.pinned_gpr_copies.emplace(&inst, *candidate);
    }
}

}  // namespace swift::runtime::backend::arm64
