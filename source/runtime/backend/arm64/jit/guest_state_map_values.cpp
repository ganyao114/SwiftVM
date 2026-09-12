#include "runtime/backend/reg_alloc.h"
#include "guest_state_map.h"

#include <algorithm>

namespace swift::runtime::backend::arm64 {

namespace {

using ::swift::runtime::backend::IsFixedGPRHome;

bool ReadsFixedHomeValue(const ir::Inst& consumer, u32 width) {
    switch (consumer.GetOp()) {
        case ir::OpCode::SetHostGPR:
        case ir::OpCode::Add:
        case ir::OpCode::Select:
            return width >= sizeof(u32);
        case ir::OpCode::StoreMemory:
            return true;
        case ir::OpCode::ZeroExtend32:
        case ir::OpCode::ZeroExtend32To64:
        case ir::OpCode::SignExtend:
            return true;
        case ir::OpCode::Sub:
            return width <= sizeof(u32);
        case ir::OpCode::And:
        case ir::OpCode::Or:
        case ir::OpCode::Xor:
            return width == sizeof(u32);
        default:
            return false;
    }
}

bool SupportsResidentDefinitionElision(const ir::Inst& consumer) {
    return consumer.GetOp() != ir::OpCode::ZeroExtend32 &&
           consumer.GetOp() != ir::OpCode::ZeroExtend32To64 &&
           consumer.GetOp() != ir::OpCode::SignExtend;
}

bool MayClobberFixedHomes(ir::OpCode op) {
    return op == ir::OpCode::CallLambda ||
           op == ir::OpCode::CallLocation ||
           op == ir::OpCode::CallDynamic ||
           op == ir::OpCode::X87Op || op == ir::OpCode::Sse42Str;
}

ir::Inst* PhysicalWriteRoot(ir::Value value) {
    auto* root = value.Def();
    while (root) {
        const auto op = root->GetOp();
        const bool low_extract = op == ir::OpCode::BitExtract &&
                root->GetArg<ir::Imm>(1).Get() == 0;
        if (!root->IsBitCastOperation() &&
            op != ir::OpCode::ZeroExtend32To64 &&
            op != ir::OpCode::ZeroExtend32 &&
            op != ir::OpCode::SignExtend && !low_extract) {
            break;
        }
        value = root->GetArg<ir::Value>(0);
        root = value.Def();
    }
    return root;
}

struct EarlyClobber {
    ir::Inst* root{};
    u16 home{};
};

}  // namespace

void GuestStateMap::PublishValue(ir::Value value,
                                 u32 home,
                                 u32 publication,
                                 ExtensionFacts extension,
                                 ActiveState& active) {
    auto* version = value.Def();
    const u32 width = ir::GetValueSizeByte(value.Type());
    if (!version || (width != sizeof(u32) && width != sizeof(u64))) {
        return;
    }
    ActiveValue published{
            .version = version,
            .location = {
                    .home = static_cast<u16>(home),
                    .width = static_cast<u8>(width),
                    .extension = extension,
            },
            .publication = publication,
    };
    active.versions[version].push_back(published);
    active.homes[home].push_back(version);
    auto* alias_root = version;
    while (alias_root) {
        const auto op = alias_root->GetOp();
        if (op != ir::OpCode::ZeroExtend32 &&
            op != ir::OpCode::ZeroExtend32To64 &&
            op != ir::OpCode::SignExtend) {
            break;
        }
        const auto alias = alias_root->GetArg<ir::Value>(0);
        const u32 alias_width = ir::GetValueSizeByte(alias.Type());
        if (!alias.Def() || alias_width == 0 || alias_width > sizeof(u32)) {
            break;
        }
        ActiveValue alias_published{
                .version = alias.Def(),
                .location = {
                        .home = static_cast<u16>(home),
                        .width = static_cast<u8>(alias_width),
                        .extension = extension,
                },
                .publication = publication,
        };
        active.versions[alias.Def()].push_back(alias_published);
        active.homes[home].push_back(alias.Def());
        alias_root = alias.Def();
    }
}

GuestStateMap::ExtensionFacts GuestStateMap::ValueExtensionFacts(
        ir::Value value,
        const ActiveState& active) const {
    if (!value.Def()) {
        return {};
    }
    const u32 width = ir::GetValueSizeByte(value.Type());
    ExtensionFacts facts{};
    if (const auto found = active.versions.find(value.Def());
        found != active.versions.end()) {
        for (const auto& candidate : found->second) {
            const auto& candidate_facts = candidate.location.extension;
            if (candidate_facts.known_zero_above &&
                (!facts.known_zero_above ||
                 candidate_facts.known_zero_above < facts.known_zero_above)) {
                facts.known_zero_above = candidate_facts.known_zero_above;
            }
            if (candidate_facts.sign_extended_from &&
                (!facts.sign_extended_from ||
                 candidate_facts.sign_extended_from < facts.sign_extended_from)) {
                facts.sign_extended_from = candidate_facts.sign_extended_from;
                facts.sign_extended_to = candidate_facts.sign_extended_to;
            }
        }
    }
    switch (value.Def()->GetOp()) {
        case ir::OpCode::ZeroExtend32: {
            const u8 source_bits = static_cast<u8>(ir::GetValueSizeByte(
                    value.Def()->GetArg<ir::Value>(0).Type()) * 8);
            facts.known_zero_above = source_bits;
            break;
        }
        case ir::OpCode::ZeroExtend32To64: {
            auto source_facts = ValueExtensionFacts(
                    value.Def()->GetArg<ir::Value>(0), active);
            if (!source_facts.known_zero_above ||
                source_facts.known_zero_above > 32) {
                source_facts.known_zero_above = 32;
            }
            facts = source_facts;
            break;
        }
        case ir::OpCode::SignExtend: {
            facts.sign_extended_from = static_cast<u8>(ir::GetValueSizeByte(
                    value.Def()->GetArg<ir::Value>(0).Type()) * 8);
            facts.sign_extended_to = static_cast<u8>(width * 8);
            break;
        }
        case ir::OpCode::LoadImm: {
            const u64 immediate = value.Def()->GetArg<ir::Imm>(0).Get();
            facts.known_zero_above = immediate <= UINT8_MAX
                    ? 8
                    : immediate <= UINT16_MAX
                            ? 16
                            : immediate <= UINT32_MAX ? 32 : 0;
            break;
        }
        default:
            if (value.Def()->IsBitCastOperation()) {
                facts = ValueExtensionFacts(
                        value.Def()->GetArg<ir::Value>(0), active);
            }
            break;
    }
    if (width == sizeof(u32)) {
        if (!facts.known_zero_above || facts.known_zero_above > 32) {
            facts.known_zero_above = 32;
        }
        if (facts.sign_extended_to > 32) {
            facts.sign_extended_to = 32;
        }
        if (facts.sign_extended_from >= facts.sign_extended_to) {
            facts.sign_extended_from = 0;
            facts.sign_extended_to = 0;
        }
    }
    return facts;
}

void GuestStateMap::InvalidateHome(u32 home, ActiveState& active) const {
    if (home >= active.homes.size()) {
        return;
    }
    auto versions = std::move(active.homes[home]);
    active.homes[home].clear();
    for (auto* version : versions) {
        const auto found = active.versions.find(version);
        if (found == active.versions.end()) {
            continue;
        }
        found->second.erase(
                std::remove_if(found->second.begin(), found->second.end(),
                               [&](const auto& value) {
                                   return value.location.home == home;
                               }),
                found->second.end());
        if (found->second.empty()) {
            active.versions.erase(found);
        }
    }
}

void GuestStateMap::BuildValueVersions(
        const std::unordered_set<ir::Inst*>& ignored_publications,
        std::span<const CoalescedWrite> coalesced_writes,
        bool has_reused_publication) {
    ASSERT(block);
    fixed_home_uses.clear();
    fixed_home_use_counts.clear();
    registered_fixed_home_uses.clear();
    fault_capture_values.clear();
    fault_width_captures.clear();
    if (!has_reused_publication) {
        return;
    }
    const bool capture_fault_captures = NeedsFaultCaptures();
    PrepareCurrentEntryWidthFacts(capture_fault_captures);
    StackVector<EarlyClobber, 8> early_clobbers;
    for (const auto& write : coalesced_writes) {
        if (!write.publication ||
            write.publication->GetOp() != ir::OpCode::SetHostGPR) {
            continue;
        }
        auto* root = PhysicalWriteRoot(
                write.publication->GetArg<ir::Value>(0));
        if (root && root->Id() < write.publication->Id()) {
            early_clobbers.push_back({root, write.home});
        }
    }
    std::ranges::sort(early_clobbers, {}, [](const auto& clobber) {
        return clobber.root->Id();
    });
    std::size_t next_clobber{};
    ActiveState active;
    WidthFacts width_facts = block_entry_width_facts;
    for (auto& inst : block->GetInstList()) {
        for (auto value : inst.GetValues()) {
            if (!value.Def() || value.Type() == ir::ValueType::VOID) {
                continue;
            }
            const u32 width = ir::GetValueSizeByte(value.Type());
            if (width != sizeof(u8) && width != sizeof(u16) &&
                width != sizeof(u32) && width != sizeof(u64)) {
                continue;
            }
            if (!ReadsFixedHomeValue(inst, width)) {
                continue;
            }
            const ActiveValue* newest{};
            const auto versions = active.versions.find(value.Def());
            if (versions != active.versions.end()) {
                for (const auto& candidate : versions->second) {
                    if (candidate.location.width == width &&
                        (!newest || candidate.publication > newest->publication)) {
                        newest = &candidate;
                    }
                }
            }
            if (newest) {
                const bool narrow_compare = inst.GetOp() == ir::OpCode::Sub &&
                        width < sizeof(u32);
                if (narrow_compare &&
                    !newest->location.extension.KnownZeroAbove(width * 8)) {
                    continue;
                }
                fixed_home_uses.insert_or_assign(
                        std::make_pair(value.Def(), &inst), newest->location);
                if (SupportsResidentDefinitionElision(inst)) {
                    ++fixed_home_use_counts[value.Def()];
                }
            }
        }

        const bool ignored = ignored_publications.contains(&inst);
        const bool publication = !ignored &&
                inst.GetOp() == ir::OpCode::SetHostGPR &&
                inst.GetArg<ir::Imm>(2).Get() == 0;
        const auto published_value = publication
                ? inst.GetArg<ir::Value>(0)
                : ir::Value{};
        const ExtensionFacts publication_extension = publication
                ? ValueExtensionFacts(published_value, active)
                : ExtensionFacts{};
        if (capture_fault_captures && MayFaultOrObserve(inst)) {
            CaptureFaultCapture(inst, active, width_facts);
        }
        while (next_clobber < early_clobbers.size() &&
               early_clobbers[next_clobber].root == &inst) {
            InvalidateHome(early_clobbers[next_clobber].home, active);
            ++next_clobber;
        }
        if (!ignored) {
            if (inst.GetOp() == ir::OpCode::SetHostGPR) {
                const u32 home = inst.GetArg<ir::Imm>(1).Get();
                const u32 offset = inst.GetArg<ir::Imm>(2).Get();
                const u32 width = ir::GetValueSizeByte(
                        inst.GetArg<ir::Value>(0).Type());
                InvalidateHome(home, active);
                if (home < width_facts.size()) {
                    if (offset == 0 &&
                        (width == sizeof(u32) || width == sizeof(u64))) {
                        width_facts[home] = publication_extension;
                    } else if (offset + width > sizeof(u32)) {
                        width_facts[home] = {};
                    } else {
                        const u8 end_bits = static_cast<u8>((offset + width) * 8);
                        if (width_facts[home].known_zero_above &&
                            width_facts[home].known_zero_above < end_bits) {
                            width_facts[home].known_zero_above = end_bits;
                        }
                        width_facts[home].sign_extended_from = 0;
                        width_facts[home].sign_extended_to = 0;
                    }
                }
            } else if (MayClobberFixedHomes(inst.GetOp())) {
                for (u32 home = 0; home < active.homes.size(); ++home) {
                    if (!active.homes[home].empty() &&
                        ClobbersFixedHome(inst, home)) {
                        InvalidateHome(home, active);
                    }
                }
                for (u32 home = 0; home < width_facts.size(); ++home) {
                    if (ClobbersFixedHome(inst, home)) {
                        width_facts[home] = {};
                    }
                }
            }
        }
        if (ignored || inst.GetOp() != ir::OpCode::SetHostGPR ||
            inst.GetArg<ir::Imm>(2).Get() != 0) {
            if (inst.GetOp() == ir::OpCode::GetHostGPR &&
                inst.GetArg<ir::Imm>(1).Get() == 0) {
                const u32 home = inst.GetArg<ir::Imm>(0).Get();
                if (IsFixedGPRHome(home)) {
                    PublishValue(ir::Value{&inst}, home, inst.Id(),
                                 CurrentEntryExtensionFacts(home), active);
                }
            }
            continue;
        }
        const u32 home = inst.GetArg<ir::Imm>(1).Get();
        if (IsFixedGPRHome(home)) {
            PublishValue(published_value, home, inst.Id(),
                         publication_extension, active);
        }
    }
}

std::optional<GuestStateMap::FixedHomeValue> GuestStateMap::FixedHomeForUse(
        ir::Value value,
        const ir::Inst* consumer) const {
    if (!value.Def() || !consumer) {
        return std::nullopt;
    }
    const auto found = fixed_home_uses.find({value.Def(), consumer});
    return found == fixed_home_uses.end()
            ? std::nullopt
            : std::optional<FixedHomeValue>{found->second};
}

void GuestStateMap::RegisterFixedHomeUse(ir::Inst* version,
                                         const ir::Inst* consumer,
                                         FixedHomeValue location) {
    if (!version || !consumer) {
        return;
    }
    const auto key = std::make_pair(version, consumer);
    if (registered_fixed_home_uses.insert(key).second &&
        !fixed_home_uses.contains(key)) {
        ++fixed_home_use_counts[version];
    }
    fixed_home_uses.insert_or_assign(key, location);
}

std::optional<GuestStateMap::FixedHomeValue>
GuestStateMap::RegisteredFixedHomeForUse(
        ir::Value value,
        const ir::Inst* consumer) const {
    if (!value.Def() || !consumer) {
        return std::nullopt;
    }
    const auto key = std::make_pair(value.Def(), consumer);
    if (!registered_fixed_home_uses.contains(key)) {
        return std::nullopt;
    }
    return fixed_home_uses.at(key);
}

bool GuestStateMap::ValueFullyResident(ir::Inst* definition) const {
    if (!definition || definition->GetUses(false) == 0) {
        return false;
    }
    const auto found = fixed_home_use_counts.find(definition);
    return found != fixed_home_use_counts.end() &&
           found->second == definition->GetUses(false);
}

}  // namespace swift::runtime::backend::arm64
