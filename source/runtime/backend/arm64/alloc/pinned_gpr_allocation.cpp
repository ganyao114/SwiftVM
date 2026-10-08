#include "pinned_gpr_allocation.h"

#include "runtime/common/svm_config.h"

namespace swift::runtime::backend::arm64 {

void PinnedGPRAllocation::Prepare(ir::Block* current) {
    block = current;
    block_analysis.Build(block);
    recipes = {};
    allocation.BeginGPRLocationAnalysis();
    zero_extends.clear();
    sign_extends.clear();
    const auto& skip = GetSvmConfig().skip_prep;
    const auto want = [&](const char* name) {
        return skip.find(name) == std::string::npos;
    };
    PrepareDeadPinnedGPRWrites(block);
    for (const auto& [use, home] : guest_state_map.FixedHomeUses()) {
        allocation.RegisterGPRUseHome(use.first->Id(), use.second->Id(), home, false);
    }
    if (want("gprcopies")) PreparePinnedGPRCopies(block);
    if (want("selectpub")) PreparePinnedSelectPublications(block);
    if (want("transfers")) PreparePinnedGPRValueTransfers(block);
    if (want("pubviews")) PreparePinnedGPRPublicationViews(block);
}

void PinnedGPRAllocation::AssignValueHome(ir::Inst* value, u16 home) {
    allocation.AssignGPRValueHome(value->Id(), home);
}

void PinnedGPRAllocation::AssignTransferredValue(ir::Inst* value, u16 home,
                                               u32 begin, u32 end) {
    allocation.AssignGPRTransferredHome(value->Id(), home, begin, end);
}

void PinnedGPRAllocation::AssignLowView(ir::Inst* value, u16 home) {
    allocation.AssignGPRLowView(value->Id(), home);
}

std::optional<u16> PinnedGPRAllocation::ValueHome(
        ir::Value value, const ir::Inst* consumer) const {
    return value.Defined() && consumer
            ? allocation.GPRValueHome(value, consumer->Id()) : std::nullopt;
}

std::optional<u16> PinnedGPRAllocation::LowViewHome(ir::Inst* value) const {
    return value ? allocation.GPRLowView(value->Id()) : std::nullopt;
}

bool PinnedGPRAllocation::HasValueHome(ir::Inst* value) const {
    return value && allocation.HasGPRValueHome(value->Id());
}

bool PinnedGPRAllocation::HasLowView(ir::Inst* value) const {
    return LowViewHome(value).has_value();
}

void PinnedGPRAllocation::RegisterUseHome(ir::Inst* version,
                                        const ir::Inst* consumer,
                                        GuestStateMap::FixedHomeValue location) {
    allocation.RegisterGPRUseHome(version->Id(), consumer->Id(), location, true);
}

std::optional<GuestStateMap::FixedHomeValue> PinnedGPRAllocation::FixedHomeForUse(
        ir::Value value, const ir::Inst* consumer) const {
    return value.Defined() && consumer
            ? allocation.GPRUseHome(value.Id(), consumer->Id()) : std::nullopt;
}

bool PinnedGPRAllocation::ValueFullyResident(ir::Inst* definition) const {
    return definition && definition->GetUses(false) != 0 &&
           guest_state_map.ResidentUseCount(definition) +
                   allocation.AdditionalGPRResidentUses(definition->Id()) ==
           definition->GetUses(false);
}

PinnedGPRAllocation::Location PinnedGPRAllocation::UseLocation(
        ir::Value value, const ir::Inst* consumer, View view) const {
    return allocation.GPRLocationAt(value, consumer ? consumer->Id() : UINT32_MAX, view);
}

}  // namespace swift::runtime::backend::arm64
