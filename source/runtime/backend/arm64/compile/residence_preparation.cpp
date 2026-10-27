#include "residence_preparation.h"

#include <algorithm>
#include <numeric>
#include "runtime/backend/arm64/compile/register_contract.h"
#include "runtime/common/perf_stats.h"
#include "runtime/compile/analysis/semantic_facts.h"

#include "runtime/common/svm_config.h"
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::arm64 {
static_assert(sizeof(ResidencePreparation::PublicationSelection) == 24);

void GPRResidences::Reset(const ir::Block* owner) {
    block = owner;
    rows.clear();
}

GPRResidenceAction* GPRResidences::Prepare(const ir::Inst* inst) {
    // A matched source can belong to another Function block. Its selection
    // cannot authorize a local action, even if its numeric ID happens to fit.
    if (!inst || !block || inst->GetBlock() != block) return nullptr;
    const auto found = std::lower_bound(
            rows.begin(), rows.end(), inst->Id(), [](const auto& row, u32 id) {
                return row.instruction < id;
            });
    if (found != rows.end() && found->instruction == inst->Id()) return &*found;
    return &*rows.insert(found, {inst->Id()});
}

void GPRResidences::Record(const ir::Inst* inst, GPRResidenceAction::Flag flag) {
    if (auto* row = Prepare(inst)) row->bits |= flag;
}

void GPRResidences::RecordResultHome(const ir::Inst* inst, u16 home) {
    ASSERT(home < 31);
    if (auto* row = Prepare(inst); row && row->result_home == UINT8_MAX)
        row->result_home = static_cast<u8>(home);
}

const GPRResidenceAction* GPRResidences::Find(const ir::Inst* inst) const {
    if (!inst || !block || inst->GetBlock() != block) return nullptr;
    const auto found = std::lower_bound(
            rows.begin(), rows.end(), inst->Id(), [](const auto& row, u32 id) {
                return row.instruction < id;
            });
    return found != rows.end() && found->instruction == inst->Id() ? &*found : nullptr;
}

void ResidencePreparation::Prepare(const ir::Block* current) {
    ASSERT(!allocation.GPRLocationsFrozen());
    finished = false;
    block = current;
    use_facts = &allocation.PrepareFacts(block).PrepareIndex(block);
    selections.clear();
    publication_index.clear();
    dead_publications.clear();
    publication_stage = PublicationIndexStage::Unseeded;
    target_writes.clear();
    residences.Reset(block);
    const auto& skip = GetSvmConfig().skip_prep;
    const auto want = [&](const char* name) { return skip.find(name) == std::string::npos; };
    const bool seed_candidates = want("gprcopies") || want("selectpub") || want("transfers") ||
                                 want("pubviews") || want("spillpub");
    {
        PerfScope2 perf_dead{GetPerfStats2().builder_dead};
        PrepareDeadPinnedGPRWrites(seed_candidates);
        if (seed_candidates) publication_stage = PublicationIndexStage::Candidates;
        for (const auto& use : register_state.FixedHomeUses()) {
            allocation.RegisterGPRUseHome(
                    use.definition->Id(), use.consumer->Id(), use.location, false);
        }
    }
    if (want("gprcopies")) PreparePinnedGPRCopies();
    if (want("selectpub")) PreparePinnedSelectPublications();
    if (want("transfers")) PrepareFixedGPRTransfers();
    if (want("pubviews")) PreparePinnedGPRPublicationViews();
}

GPRBlockPreparation ResidencePreparation::FinishConstraints() {
    ASSERT(!finished);
    // Preserve the old copy/publication, select/producer and spill/publication
    // ordering. Writes enter only here, after every physical edit has finished.
    using Kind = PublicationSelection::Kind;
    const auto priority = [](Kind kind) {
        return kind == Kind::Copy ? 0 : kind == Kind::Select ? 1 : 2;
    };
    const auto key = [](const PublicationSelection& row) {
        return row.kind == Kind::Select ? row.producer : row.publication;
    };
    // Publication lookup has no consumers beyond this boundary. Reuse its
    // index instead of constructing three temporary pointer vectors. Filter
    // and visit once, retaining first insertion for each family/key pair.
    ASSERT(selections.size() <= UINT32_MAX);
    publication_stage = PublicationIndexStage::Unseeded;
    publication_index.clear();
    for (u32 i = 0; i < selections.size(); ++i) {
        const auto& row = selections[i];
        if (row.kind == Kind::Select || row.kind == Kind::Spill ||
            (row.kind == Kind::Copy && row.source == UINT16_MAX))
            publication_index.push_back(i);
    }
    std::sort(publication_index.begin(), publication_index.end(), [&](u32 a, u32 b) {
        const auto& left = selections[a];
        const auto& right = selections[b];
        if (left.kind != right.kind) return priority(left.kind) < priority(right.kind);
        if (key(left) != key(right)) return std::less<const ir::Inst*>{}(key(left), key(right));
        return a < b;
    });
    const ir::Inst* previous{};
    auto previous_kind = Kind::Transfer;
    for (const auto index : publication_index) {
        const auto& row = selections[index];
        if (row.kind == previous_kind && key(row) == previous) continue;
        previous_kind = row.kind;
        previous = key(row);
        RecordPhysicalWrite(row.producer, row.target);
        if (row.kind == Kind::Select) residences.RecordResultHome(row.producer, row.target);
    }
    for (auto& inst : block->GetInstList()) {
        if (ValueFullyResident(&inst)) residences.Record(&inst, GPRResidenceAction::Resident);
    }
    finished = true;
    // A rejected recolor may have borrowed capacity only for its temporary
    // proof witness. Empty pending results must not retain that scratch buffer.
    if (target_writes.empty()) target_writes = decltype(target_writes){};
    GPRBlockPreparation draft{std::move(residences), std::move(target_writes)};
    selections.clear();
    publication_index.clear();
    dead_publications.clear();
    publication_stage = PublicationIndexStage::Unseeded;
    use_facts = nullptr;
    return draft;
}

void ResidencePreparation::AssignValueHome(const ir::Inst* value, u16 home) {
    ASSERT(!finished);
    allocation.AssignGPRValueHome(value->Id(), home);
}

void ResidencePreparation::AssignTransferredValue(const ir::Inst* value,
                                                  u16 home,
                                                  u32 begin,
                                                  u32 end) {
    allocation.AssignGPRTransferredHome(value->Id(), home, begin, end);
}

void ResidencePreparation::AssignLowView(const ir::Inst* value, u16 home) {
    ASSERT(!finished);
    allocation.AssignGPRLowView(value->Id(), home);
}

std::optional<u16> ResidencePreparation::ValueHome(ir::Value value,
                                                   const ir::Inst* consumer) const {
    return value.Defined() && consumer ? allocation.GPRValueHome(value, consumer->Id())
                                       : std::nullopt;
}

std::optional<u16> ResidencePreparation::LowViewHome(const ir::Inst* value) const {
    return value ? allocation.GPRLowView(value->Id()) : std::nullopt;
}

bool ResidencePreparation::HasValueHome(const ir::Inst* value) const {
    return value && allocation.HasGPRValueHome(value->Id());
}

bool ResidencePreparation::HasLowView(const ir::Inst* value) const {
    return LowViewHome(value).has_value();
}

void ResidencePreparation::RegisterUseHome(const ir::Inst* version,
                                           const ir::Inst* consumer,
                                           RegisterStatePreparation::FixedHomeValue location) {
    allocation.RegisterGPRUseHome(version->Id(), consumer->Id(), location, true);
}

std::optional<RegisterStatePreparation::FixedHomeValue> ResidencePreparation::FixedHomeForUse(
        ir::Value value, const ir::Inst* consumer) const {
    return value.Defined() && consumer ? allocation.GPRUseHome(value.Id(), consumer->Id())
                                       : std::nullopt;
}

bool ResidencePreparation::ValueFullyResident(const ir::Inst* definition) const {
    return definition && definition->GetUses(false) != 0 &&
           register_state.ResidentUseCount(definition) +
                           allocation.AdditionalGPRResidentUses(definition->Id()) ==
                   definition->GetUses(false);
}

ResidencePreparation::Location ResidencePreparation::UseLocation(ir::Value value,
                                                                 const ir::Inst* consumer,
                                                                 View view) const {
    ASSERT(!finished);
    return allocation.GPRLocationAt(value, consumer ? consumer->Id() : UINT32_MAX, view);
}

void ResidencePreparation::SelectPublication(PublicationSelection row) {
    ASSERT(!finished && publication_stage != PublicationIndexStage::Selections &&
           row.publication && row.producer);
    selections.push_back(row);
}

std::span<const u32> ResidencePreparation::PreparePublicationCandidates() {
    ASSERT(!finished && publication_stage != PublicationIndexStage::Selections);
    if (publication_stage == PublicationIndexStage::Unseeded) {
        // Normally seeded in the existing dead-write/version visit. A caller
        // can still explicitly request spills when all configured families
        // were skipped; build the roster only at that actual request.
        ASSERT(publication_index.empty());
        const auto instructions = UseFacts().Instructions();
        for (u32 position = 0; position < instructions.size(); ++position) {
            const auto* inst = instructions[position];
            if (IsPublicationCandidate(*inst) && !IsDeadPublication(inst))
                publication_index.push_back(position);
        }
        publication_stage = PublicationIndexStage::Candidates;
    }
    return publication_index;
}

void ResidencePreparation::IndexPublications() {
    ASSERT(!finished);
    if (publication_stage == PublicationIndexStage::Selections) return;
    ASSERT(selections.size() <= UINT32_MAX);
    publication_index.resize(selections.size());
    std::iota(publication_index.begin(), publication_index.end(), 0u);
    std::sort(publication_index.begin(), publication_index.end(), [&](u32 a, u32 b) {
        const auto& left = selections[a];
        const auto& right = selections[b];
        if (left.publication != right.publication)
            return std::less<const ir::Inst*>{}(left.publication, right.publication);
        if (left.kind != right.kind) return left.kind < right.kind;
        return a < b;  // first insertion for the same publication and family
    });
    publication_stage = PublicationIndexStage::Selections;
}

const ResidencePreparation::PublicationSelection* ResidencePreparation::PublicationFor(
        const ir::Inst* publication) const {
    ASSERT(!finished && publication_stage == PublicationIndexStage::Selections);
    const auto found = std::lower_bound(publication_index.begin(),
                                        publication_index.end(),
                                        publication,
                                        [&](u32 index, const ir::Inst* key) {
                                            return std::less<const ir::Inst*>{}(
                                                    selections[index].publication, key);
                                        });
    return found != publication_index.end() && selections[*found].publication == publication
                   ? &selections[*found]
                   : nullptr;
}

bool ResidencePreparation::IsDeadPublication(const ir::Inst* publication) const {
    return std::binary_search(dead_publications.begin(),
                              dead_publications.end(),
                              publication,
                              std::less<const ir::Inst*>{});
}

}  // namespace swift::runtime::backend::arm64
