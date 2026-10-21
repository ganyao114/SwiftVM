#include "guest_state_map.h"

#include <algorithm>
#include <bit>
#include "runtime/backend/reg_alloc.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/arm64/alloc/target_analysis_traits.h"

namespace swift::runtime::backend::arm64 {

void GuestStateMap::Analyze(ir::Block* next_block,
                            const FeatureSet& next_features) {
    block = next_block;
    features = next_features;
    InvalidateFixedHomeClobberIndex();
    successful_survival_scans = 0;
    block_entry_width_facts = {};
    if (const auto found = function_entry_width_facts.find(block);
        found != function_entry_width_facts.end()) {
        block_entry_width_facts = found->second;
    }
    fault_capture_values.clear();
    fault_width_captures.clear();
    fixed_home_uses.clear();
    fixed_home_use_counts.clear();
}

bool GuestStateMap::ClobbersFixedHome(const ir::Inst& inst,
                                      u32 home) const {
    if (inst.GetOp() == ir::OpCode::XchgBarrier) {
        return (::swift::runtime::backend::FixedGPRClobbers(
                        inst, features) & (1u << home)) != 0;
    }
    if (inst.GetOp() == ir::OpCode::SetHostGPR &&
        inst.GetArg<ir::Imm>(1).Get() == home) {
        return true;
    }
    return home <= 9 && HelperCallContract::InstructionClobbersGPR(inst, home,
                                                                   features);
}

GuestStateMap::ExtensionFacts GuestStateMap::CurrentEntryExtensionFacts(
        u32 home) const {
    return home < block_entry_width_facts.size()
            ? block_entry_width_facts[home]
            : ExtensionFacts{};
}

void GuestStateMap::InvalidateFixedHomeClobberIndex() {
    // Clear contents but keep the storage: the next block's build then
    // reuses the 65 vectors' capacity instead of regrowing them from
    // empty on every Analyze (a top host-compile allocator customer).
    if (fixed_home_clobbers) {
        for (auto& ids : fixed_home_clobbers->writes) ids.clear();
        for (auto& ids : fixed_home_clobbers->reads) ids.clear();
        fixed_home_clobbers->observers.clear();
    }
    fixed_home_clobbers_built = false;
}

void GuestStateMap::BuildFixedHomeClobberIndex() const {
    if (!fixed_home_clobbers) {
        fixed_home_clobbers = std::make_unique<FixedHomeClobberIndex>();
    }
    auto* index = fixed_home_clobbers.get();
    // Contents are empty after invalidation; clearing again also covers
    // a previous build abandoned mid-way by an exception.
    for (auto& ids : index->writes) ids.clear();
    for (auto& ids : index->reads) ids.clear();
    index->observers.clear();
    for (auto& inst : block->GetInstList()) {
        if (MayFaultOrObserve(inst) || inst.GetOp() == ir::OpCode::GetUniformAddress ||
            inst.GetOp() == ir::OpCode::UniformBarrier) index->observers.push_back(&inst);
        if (inst.GetOp() == ir::OpCode::GetHostGPR) {
            const auto home = inst.GetArg<ir::Imm>(0).Get();
            if (home < 32) index->reads[home].push_back(inst.Id());
        }
        u32 mask{};
        if (inst.GetOp() == ir::OpCode::XchgBarrier) {
            mask = ::swift::runtime::backend::FixedGPRClobbers(inst, features);
        } else if (inst.GetOp() == ir::OpCode::SetHostGPR) {
            const auto home = inst.GetArg<ir::Imm>(1).Get();
            if (home < 32) mask = 1u << home;
        } else if (const auto helper = HelperCallContract::Resolve(inst, features)) {
            // Resolve an exact helper contract once, rather than once per
            // register and interval. Only caller-saved guest homes apply here.
            for (u32 home = 0; home <= 9; ++home)
                if (helper->ClobbersGPR(home)) mask |= 1u << home;
        }
        while (mask) {
            const auto home = std::countr_zero(mask);
            index->writes[home].push_back(inst.Id());
            mask &= mask - 1;
        }
    }
    for (auto& ids : index->writes) std::sort(ids.begin(), ids.end());
    fixed_home_clobbers_built = true;
}

bool GuestStateMap::FixedHomeSurvives(u32 home,
                                      u32 after,
                                      u32 before) const {
    ASSERT(block);
    if (before <= after || before - after <= 1) return true;
    // Wait for three complete scans before paying to resolve contracts and
    // collect clobbers for every home. Two-query transfers keep scanning.
    constexpr u8 min_indexed_scans = 3;
    if (home < 32 && (fixed_home_clobbers_built || successful_survival_scans >= min_indexed_scans)) {
        if (!fixed_home_clobbers_built) BuildFixedHomeClobberIndex();
        const auto& ids = fixed_home_clobbers->writes[home];
        const auto next = std::upper_bound(ids.begin(), ids.end(), after);
        return next == ids.end() || *next >= before;
    }
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() > after && inst.Id() < before &&
            ClobbersFixedHome(inst, home)) {
            return false;
        }
    }
    // Small blocks and early rejections keep their cheap linear path.
    // Several successful whole-block traversals amortize the index build.
    const auto& instructions = block->GetInstList();
    if (!instructions.empty() && instructions.back().Id() >= instructions.front().Id() &&
        instructions.back().Id() - instructions.front().Id() >= 63 &&
        successful_survival_scans < min_indexed_scans)
        ++successful_survival_scans;
    return true;
}

std::optional<u32> GuestStateMap::FirstFixedHomeClobber(u32 home, u32 after) const {
    ASSERT(block && home < 32);
    if (!fixed_home_clobbers_built) BuildFixedHomeClobberIndex();
    const auto& ids = fixed_home_clobbers->writes[home];
    const auto next = std::upper_bound(ids.begin(), ids.end(), after);
    return next == ids.end() ? std::nullopt : std::optional<u32>{*next};
}

bool GuestStateMap::PublicationWindowSafe(
        u32 home,
        ir::Value early_value,
        u32 after,
        u32 before,
        const ir::Inst* ignored) const {
    ASSERT(block);
    if (before <= after + 1) return true;
    ASSERT(home < 32);
    if (!fixed_home_clobbers_built) BuildFixedHomeClobberIndex();
    const auto& clobbers = fixed_home_clobbers->writes[home];
    for (auto it = std::upper_bound(clobbers.begin(), clobbers.end(), after);
         it != clobbers.end() && *it < before; ++it) {
        if (!ignored || *it != ignored->Id()) return false;
    }
    const auto& reads = fixed_home_clobbers->reads[home];
    for (auto it = std::upper_bound(reads.begin(), reads.end(), after);
         it != reads.end() && *it < before; ++it) {
        if (!ignored || *it != ignored->Id()) return false;
    }
    const auto& observers = fixed_home_clobbers->observers;
    const auto first = std::upper_bound(observers.begin(), observers.end(), after,
            [](u32 id, const ir::Inst* inst) { return id < inst->Id(); });
    for (auto it = first; it != observers.end() && (*it)->Id() < before; ++it) {
        if (*it != ignored && !FaultCaptureContains(
                    **it, home, early_value.Def(),
                    ir::GetValueSizeByte(early_value.Type()) <= sizeof(u32))) {
            return false;
        }
    }
    return true;
}

bool GuestStateMap::MayFaultOrObserve(const ir::Inst& inst) const {
    return TargetMayFaultOrObserve(inst, features);
}

bool GuestStateMap::MayFaultOrObserve(ir::OpCode op) {
    return TargetMayFaultOrObserve(op);
}

}  // namespace swift::runtime::backend::arm64
