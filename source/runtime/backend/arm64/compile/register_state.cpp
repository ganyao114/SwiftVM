#include "register_state.h"

#include <algorithm>

#include "effects.h"
#include "register_contract.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/compile/analysis/semantic_facts.h"

namespace swift::runtime::backend::arm64 {
void RegisterStatePreparation::BeginBlock(const ir::Block* next_block,
                                          const FeatureSet& next_features,
                                          compile::SemanticFacts& facts) {
    ASSERT(facts.Contains(next_block));
    if (function && !facts.MatchesInput(nullptr, function)) {
        function = nullptr;
        function_width_facts_ready = false;
        function_entry_width_facts.clear();
    }
    semantic_facts = &facts;
    block = next_block;
    features = next_features;
    ResetGPRWindows();
    successful_survival_scans = 0;
    block_entry_width_facts = {};
    if (function_width_facts_ready) {
        const auto id = semantic_facts->FindFunctionCFG()->FindBlock(block);
        if (id) block_entry_width_facts = function_entry_width_facts[*id];
    }
    fault_capture_values.clear();
    fault_width_captures.clear();
    fixed_home_uses.clear();
    fixed_home_use_counts.clear();
}

bool RegisterStatePreparation::ClobbersFixedHome(const ir::Inst& inst, u32 home) const {
    if (inst.GetOp() == ir::OpCode::XchgBarrier) {
        return (::swift::runtime::backend::FixedGPRClobbers(inst, features) & (1u << home)) != 0;
    }
    if (inst.GetOp() == ir::OpCode::SetHostGPR && inst.GetArg<ir::Imm>(1).Get() == home)
        return true;
    return home <= 9 && HelperCallContract::InstructionClobbersGPR(inst, home, features);
}

RegisterStatePreparation::ExtensionFacts RegisterStatePreparation::CurrentEntryExtensionFacts(
        u32 home) const {
    return home < block_entry_width_facts.size() ? block_entry_width_facts[home] : ExtensionFacts{};
}

void RegisterStatePreparation::ResetGPRWindows() {
    gpr_accesses = nullptr;
    gpr_windows_ready = false;
    if (clobber_projection) {
        clobber_projection->entries.clear();
        clobber_projection->homes = 0;
    }
}

void RegisterStatePreparation::PrepareGPRWindows() {
    if (gpr_windows_ready) return;
    // A failed preparation can be retried without exposing partial masks.
    if (clobber_projection) {
        clobber_projection->entries.clear();
        clobber_projection->homes = 0;
    }
    gpr_accesses = nullptr;
    if (const auto* summary = semantic_facts->FindSummary(block);
        summary && !summary->HasGPRWindowCandidates()) {
        gpr_windows_ready = true;
        return;
    }
    gpr_accesses = &semantic_facts->PrepareGuestStateRegisterAccesses(block);
    for (const auto* inst : gpr_accesses->GPRBarriers()) {
        u32 homes{};
        if (inst->GetOp() == ir::OpCode::XchgBarrier) {
            homes = ::swift::runtime::backend::FixedGPRClobbers(*inst, features);
        } else if (const auto helper = HelperCallContract::Resolve(*inst, features)) {
            for (u32 home = 0; home <= 9; ++home)
                if (helper->ClobbersGPR(home)) homes |= 1u << home;
        }
        if (homes) {
            if (!clobber_projection) clobber_projection = std::make_unique<ClobberProjection>();
            clobber_projection->entries.push_back({static_cast<u16>(inst->Id()), homes});
            clobber_projection->homes |= homes;
        }
    }
    if (clobber_projection) {
        std::ranges::sort(clobber_projection->entries, {}, &ClobberProjection::Entry::position);
    }
    gpr_windows_ready = true;
}

std::optional<u32> RegisterStatePreparation::FirstClobber(
        u32 home, u32 after, u32 before, std::optional<u32> ignored) const {
    ASSERT(home < 32 && gpr_windows_ready);
    const auto write = gpr_accesses
            ? gpr_accesses->FirstAccess(compile::GuestStateRegisterFile::GPR, home, false, after, ignored) : std::nullopt;
    const u32 limit = write ? std::min(*write, before) : before;
    if (clobber_projection && (clobber_projection->homes & (1u << home))) {
        const auto& entries = clobber_projection->entries;
        auto next = std::upper_bound(
                entries.begin(), entries.end(), after, [](u32 point, const auto& entry) {
                    return point < entry.position;
                });
        for (; next != entries.end() && next->position < limit; ++next) {
            if ((!ignored || next->position != *ignored) && (next->homes & (1u << home)))
                return next->position;
        }
    }
    return write && *write < before ? write : std::nullopt;
}

bool RegisterStatePreparation::PrepareHomeSurvival(u32 home, u32 after, u32 before) {
    ASSERT(block);
    if (before <= after || before - after <= 1) return true;
    // Keep the cheap scan until substantial requests justify a new shared
    // index. Already-prepared inputs can be reused immediately by a new view.
    constexpr u8 min_indexed_scans = 3;
    if (home < 32 && (gpr_windows_ready || successful_survival_scans >= min_indexed_scans ||
                      semantic_facts->FindGuestStateRegisterAccesses(block))) {
        PrepareGPRWindows();
        return !FirstClobber(home, after, before);
    }
    for (const auto& inst : block->GetInstList()) {
        if (inst.Id() > after && inst.Id() < before && ClobbersFixedHome(inst, home)) return false;
    }
    const auto& instructions = block->GetInstList();
    if (!instructions.empty() && instructions.back().Id() >= instructions.front().Id() &&
        instructions.back().Id() - instructions.front().Id() >= 63 &&
        successful_survival_scans < min_indexed_scans)
        ++successful_survival_scans;
    return true;
}

std::optional<u32> RegisterStatePreparation::PrepareFirstClobber(u32 home, u32 after) {
    ASSERT(block && home < 32);
    PrepareGPRWindows();
    return FirstClobber(home, after, UINT32_MAX);
}

bool RegisterStatePreparation::PreparePublicationWindow(
        u32 home, ir::Value early_value, u32 after, u32 before, const ir::Inst* ignored) {
    ASSERT(block);
    if (before <= after + 1) return true;
    ASSERT(home < 32);
    PrepareGPRWindows();
    const auto ignored_position = ignored ? std::optional<u32>{ignored->Id()} : std::nullopt;
    if (FirstClobber(home, after, before, ignored_position)) return false;
    const auto read = gpr_accesses
            ? gpr_accesses->FirstAccess(compile::GuestStateRegisterFile::GPR, home, true, after, ignored_position) : std::nullopt;
    if (read && *read < before) return false;
    // Home-only requests do not need observation payloads. Reuse the common
    // input's candidate stream across allocation attempts and target planning,
    // retaining this consumer's feature-bound boundary and capture proofs.
    const auto observers = semantic_facts->PrepareGuestStateObservations(block);
    const auto first = std::upper_bound(
            observers.begin(), observers.end(), after, [](u32 id, const ir::Inst* inst) {
                return id < inst->Id();
            });
    for (auto it = first; it != observers.end() && (*it)->Id() < before; ++it) {
        if (!TargetMayFaultOrObserve(**it, features) &&
            (*it)->GetOp() != ir::OpCode::GetUniformAddress &&
            (*it)->GetOp() != ir::OpCode::UniformBarrier)
            continue;
        if (*it == ignored ||
            FaultCaptureContains(**it, home, early_value.Def(),
                                 ir::GetValueSizeByte(early_value.Type()) <= sizeof(u32)))
            continue;
        // A full logical scalar has no dirty high container bits. Its original
        // IR version at the observation may also prove this publication safe,
        // independently of this attempt's physical captures and entry widths.
        if (ir::GetScalarIntegerBits(early_value.Type()) != 64 ||
            !semantic_facts->PrepareGuestStateGPRMatch(
                    block, home, 0, **it, early_value,
                    function ? compile::GuestStateEntryPolicy::Function
                             : compile::GuestStateEntryPolicy::Block))
            return false;
    }
    return true;
}

}  // namespace swift::runtime::backend::arm64
