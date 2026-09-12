#include "runtime/backend/reg_alloc.h"
#include "translator.h"

namespace swift::runtime::backend::arm64 {

namespace {

using ::swift::runtime::backend::IsFixedGPRHome;

std::optional<u32> LowViewLastUse(ir::Block* block,
                                  ir::Inst* alias,
                                  ir::Inst* value) {
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
    for (auto& consumer : block->GetInstList()) {
        const auto uses = std::ranges::count_if(
                consumer.GetValues(), [&](ir::Value input) { return input.Def() == alias; });
        if (!uses) {
            continue;
        }
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

std::optional<JitTranslator::PinnedGPRPublicationView> JitTranslator::MatchPinnedGPRPublicationView(
        ir::Inst* publication) const {
    if (!publication || publication->GetOp() != ir::OpCode::SetHostGPR ||
        publication->GetArg<ir::Imm>(2).Get() != 0 ||
        pinned_gprs.dead_pinned_gpr_writes.contains(publication)) {
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

    bool saw_publication = false;
    u32 ordinary_uses{};
    u32 last_use = publication->Id();
    std::vector<ir::Inst*> aliases;
    for (auto& consumer : cur_block->GetInstList()) {
        const auto uses = std::ranges::count_if(
                consumer.GetValues(), [&](ir::Value input) { return input.Def() == value; });
        if (!uses) {
            continue;
        }
        ordinary_uses += uses;
        if (&consumer == publication && uses == 1) {
            saw_publication = true;
            continue;
        }
        const auto alias_last_use = LowViewLastUse(
                cur_block, &consumer, value);
        if (consumer.Id() <= publication->Id() || uses != 1 ||
            !alias_last_use) {
            return std::nullopt;
        }
        aliases.push_back(&consumer);
        last_use = std::max(last_use, *alias_last_use);
    }
    if (!saw_publication || aliases.empty() || ordinary_uses != value->GetUses(false)) {
        return std::nullopt;
    }
    if (!guest_state_map.FixedHomeSurvives(
                target, publication->Id(), last_use)) {
        return std::nullopt;
    }
    return PinnedGPRPublicationView{
            .value = value,
            .publication = publication,
            .aliases = std::move(aliases),
            .target = static_cast<u16>(target),
            .last_use = last_use,
    };
}

void JitTranslator::PreparePinnedGPRPublicationViews(ir::Block* block) {
    for (auto& inst : block->GetInstList()) {
        auto candidate = MatchPinnedGPRPublicationView(&inst);
        if (!candidate || std::ranges::any_of(candidate->aliases, [&](ir::Inst* alias) {
                return pinned_gprs.fused_pin_gpr_reads.contains(alias);
            })) {
            continue;
        }
        for (auto* alias : candidate->aliases) {
            pinned_gprs.fused_pin_gpr_reads.emplace(alias, candidate->target);
        }
        pinned_gprs.pinned_gpr_publication_views.emplace(&inst, std::move(*candidate));
    }
}

}  // namespace swift::runtime::backend::arm64
