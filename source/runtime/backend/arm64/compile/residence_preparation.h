#pragma once

#include "effects.h"

#include <array>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "allocation_preparation.h"
#include "constraints.h"
#include "residences.h"
#include "runtime/backend/arm64/compile/register_state.h"
#include "runtime/compile/analysis/block_index.h"
#include "write_successors.h"

namespace swift::runtime::compile {
class BlockAnalysisIndex;
}
namespace swift::runtime::compile {
class PseudoFlagsPlan;
}
namespace swift::runtime::backend::arm64 {

// Block-local physical residence preparation after ordinary allocation.
// Matched alias/consumer payloads end at submission; only compact publication
// selections and required residence/write inputs survive until block completion.
// Allocation owns physical locations, live sets and spill recipes; this pass
// submits copy/view constraints without generating code or leasing scratch.
// RegisterStatePreparation owns observation/fault/version facts, not physical allocation.
class ResidencePreparation final {
private:
    enum class PublicationIndexStage : u8 { Unseeded, Candidates, Selections };
    struct CopyCandidate {
        const ir::Inst* read{};
        const ir::Inst* narrow_extend{};
        const ir::Inst* extend{};
        bool signed_load{};
        std::optional<u16> source{};
        u16 target{};
        u8 width{};
        u32 last_use{};
    };
    struct SelectCandidate {
        const ir::Inst* producer{};
        const ir::Inst* extend{};
        std::span<const compile::BlockAnalysisIndex::Use> consumers;
        u16 target{};
        u32 last_use{};
    };
    struct PublicationViewCandidate {
        std::span<const compile::BlockAnalysisIndex::Use> consumers;
        u16 target{};
    };

public:
    struct PublicationSelection {
        enum class Kind : u8 { Select, Spill, Transfer, Copy };
        const ir::Inst* publication{};
        const ir::Inst* producer{};
        u16 source{UINT16_MAX}, target{};
        u8 width{};
        Kind kind{};
    };

    using View = AllocationPreparation::GPRView;
    using Location = AllocationPreparation::GPRLocation;

    ResidencePreparation(AllocationPreparation& allocation,
                         const FeatureSet& features,
                         RegisterStatePreparation& register_state,
                         GPRSMask uniforms = GPRSMask{})
            : allocation(allocation)
            , features(features)
            , uniforms(uniforms)
            , register_state(register_state) {}
    ResidencePreparation(const ResidencePreparation&) = delete;
    ResidencePreparation(ResidencePreparation&&) = delete;
    ResidencePreparation& operator=(const ResidencePreparation&) = delete;
    ResidencePreparation& operator=(ResidencePreparation&&) = delete;
    void Prepare(const ir::Block* block);
    // Final flag demand comes from target lowering; discovery still runs before
    // code emission and uses only semantic facts and the completed allocation.
    void PrepareNarrowReads(const std::function<ir::Flags(const ir::Inst*)>& set_flags);
    // Run after target memory combinations have supplied their constraints.
    void PrepareSpilledGPRPublications();
    // All target constraints must be submitted before this boundary.
    GPRBlockPreparation FinishConstraints();
    void RecordPhysicalWrite(const ir::Inst* inst, u16 home) {
        ASSERT(!finished);
        target_writes.push_back({inst->Id(), home});
    }
    // Explicit preparation boundary: const lookup never sorts or allocates.
    void IndexPublications();
    [[nodiscard]] const PublicationSelection* PublicationFor(const ir::Inst*) const;
    [[nodiscard]] bool IsDeadPublication(const ir::Inst*) const;
    [[nodiscard]] const GPRResidences& Residences() const { return residences; }
    [[nodiscard]] const compile::BlockAnalysisIndex& UseFacts() const {
        ASSERT(!finished && use_facts);
        return *use_facts;
    }
    [[nodiscard]] compile::BlockAnalysisIndex& PrepareUseFacts() {
        ASSERT(!finished && use_facts);
        return *use_facts;
    }
    [[nodiscard]] Location UseLocation(ir::Value value,
                                       const ir::Inst* consumer,
                                       View view = View::Value) const;
    [[nodiscard]] std::optional<u16> ValueHome(ir::Value value, const ir::Inst* consumer) const;
    [[nodiscard]] std::optional<u16> LowViewHome(const ir::Inst* value) const;
    [[nodiscard]] bool HasValueHome(const ir::Inst* value) const;
    [[nodiscard]] bool HasLowView(const ir::Inst* value) const;

    [[nodiscard]] std::optional<RegisterStatePreparation::FixedHomeValue> FixedHomeForUse(
            ir::Value value, const ir::Inst* consumer) const;
    [[nodiscard]] bool ValueFullyResident(const ir::Inst* definition) const;

    // Target combines may constrain a result/view, but never edit the ordinary
    // allocation or infer guest-state safety through a materializing JIT API.
    void AssignValueHome(const ir::Inst* value, u16 home);
    void AssignLowView(const ir::Inst* value, u16 home);

    void ElideZeroExtend(const ir::Inst* inst) {
        ASSERT_MSG(!finished, "zero extension changed after plan finish");
        residences.Record(inst, GPRResidenceAction::ZeroExtend);
    }

private:
    [[nodiscard]] bool AppendRecolorValue(Vector<u32>& values, const ir::Inst*) const;
    [[nodiscard]] bool RecolorPublication(std::span<const u32> component,
                                          u16 home,
                                          u32 publication,
                                          u32 last_use);
    template <typename Visitor>
    bool VisitCopyAliases(const CopyCandidate&, const ir::Inst* publication, Visitor&&) const;
    void RegisterCopyUses(const CopyCandidate&);
    [[nodiscard]] bool IsDeadPinnedGPRWrite(const ir::Inst* inst) const;
    [[nodiscard]] std::optional<CopyCandidate> MatchPinnedGPRCopy(const ir::Inst* inst);
    [[nodiscard]] std::optional<SelectCandidate> MatchPinnedSelectPublication(
            const ir::Inst* publication);
    [[nodiscard]] std::optional<FixedGPRTransfer> MatchTransfer(const ir::Inst* publication,
                                                               FixedGPRTransferUses& aliases);
    [[nodiscard]] std::optional<PublicationViewCandidate> MatchPinnedGPRPublicationView(
            const ir::Inst* publication);
    [[nodiscard]] std::optional<PublicationSelection> MatchSpilledGPRPublication(
            const ir::Inst* publication);
    void RegisterUseHome(const ir::Inst* version,
                         const ir::Inst* consumer,
                         RegisterStatePreparation::FixedHomeValue location);
    void AssignTransferredValue(const ir::Inst* value, u16 home, u32 begin, u32 end);
    [[nodiscard]] bool IsGPRMappedTo(ir::Value value, u16 home) const {
        return allocation.ValueType(value) == AllocationPreparation::GPR &&
               allocation.ValueGPR(value).id == home;
    }
    [[nodiscard]] bool MayFaultOrObserve(const ir::Inst& inst) const {
        return TargetMayFaultOrObserve(inst, features);
    }
    void SelectPublication(PublicationSelection);
    [[nodiscard]] static bool IsPublicationCandidate(const ir::Inst&);
    [[nodiscard]] std::span<const u32> PreparePublicationCandidates();
    void PrepareDeadPinnedGPRWrites(bool seed_candidates);
    void PreparePinnedGPRCopies();
    void PreparePinnedSelectPublications();
    void PrepareFixedGPRTransfers();
    void PreparePinnedGPRPublicationViews();

    AllocationPreparation& allocation;
    const FeatureSet features;
    const GPRSMask uniforms;
    RegisterStatePreparation& register_state;
    const ir::Block* block{};
    compile::BlockAnalysisIndex* use_facts{};
    GPRWriteSuccessors write_successors;
    std::vector<PublicationSelection> selections;
    // Sequential uses: candidate roster positions, selected-publication
    // lookup, then the final physical-write order. No extra owning buffer.
    std::vector<u32> publication_index;
    std::vector<const ir::Inst*> dead_publications;
    PublicationIndexStage publication_stage{};
    GPRResidences residences;
    bool finished{};
    std::vector<compile::PhysicalGPRWrite> target_writes{};
};

}  // namespace swift::runtime::backend::arm64
