#pragma once

#include <functional>
#include <map>
#include <optional>
#include <unordered_set>
#include <vector>

#include "runtime/backend/reg_alloc.h"
#include "fixed_gpr_constraints.h"
#include "runtime/backend/arm64/jit/block_analysis_index.h"
#include "runtime/backend/arm64/jit/guest_state_map.h"

namespace swift::runtime::backend::arm64 {

// Target-specific recipes after ordinary allocation and fixed GPR preplanning.
// RegAlloc owns every physical location, live set and spill recipe; this pass
// submits copy/view constraints without generating code or leasing scratch.
// GuestStateMap owns observation/fault/version facts, not physical allocation.
class PinnedGPRAllocation final {
public:
    struct PinnedGPRCopy {
        ir::Inst* read{};
        ir::Inst* narrow_extend{};
        ir::Inst* extend{};
        bool signed_load{};
        std::vector<ir::Inst*> aliases{};
        std::vector<std::pair<ir::Inst*, ir::Inst*>> transferred_uses{};
        std::optional<u16> source{};
        u16 target{};
        u8 width{};
        u32 last_use{};
    };
    struct PinnedSelectPublication {
        ir::Inst* producer{};
        ir::Inst* extend{};
        ir::Inst* publication{};
        std::vector<ir::Inst*> aliases{};
        u16 target{};
        u32 last_use{};

        bool operator==(const PinnedSelectPublication&) const = default;
    };
    using PinnedGPRValueTransfer = FixedGPRTransfer;
    struct PinnedGPRPublicationView {
        ir::Inst* value{};
        ir::Inst* publication{};
        std::vector<ir::Inst*> aliases{};
        u16 target{};
        u32 last_use{};

        bool operator==(const PinnedGPRPublicationView&) const = default;
    };
    struct SpilledGPRPublication {
        ir::Inst* producer{};
        ir::Inst* publication{};
        u16 target{};

        bool operator==(const SpilledGPRPublication&) const = default;
    };

    struct Recipes {
        std::map<ir::Inst*, PinnedGPRCopy> copies{};
        std::map<ir::Inst*, PinnedSelectPublication> select_results{};
        std::map<ir::Inst*, PinnedSelectPublication> select_publications{};
        std::map<ir::Inst*, PinnedGPRValueTransfer> transfers{};
        std::map<ir::Inst*, PinnedGPRPublicationView> publication_views{};
        std::map<ir::Inst*, SpilledGPRPublication> spilled_publications{};
        std::unordered_set<ir::Inst*> dead_writes{};
    };

    using View = RegAlloc::GPRView;
    using Location = RegAlloc::GPRLocation;

    PinnedGPRAllocation(RegAlloc& allocation, const FeatureSet& features,
                        GuestStateMap& guest_state_map)
            : allocation(allocation), features(features), guest_state_map(guest_state_map) {}
    void Prepare(ir::Block* block);
    // Final flag demand comes from target lowering; discovery still runs before
    // code emission and uses only semantic facts and the completed allocation.
    void PrepareNarrowReads(const std::function<ir::Flags(ir::Inst*)>& set_flags);
    // Run after target memory combinations have supplied their constraints.
    void PrepareSpilledGPRPublications(ir::Block* block);
    [[nodiscard]] const Recipes& GetRecipes() const { return recipes; }
    [[nodiscard]] Location UseLocation(ir::Value value, const ir::Inst* consumer,
                                       View view = View::Value) const;
    [[nodiscard]] std::optional<u16> ValueHome(ir::Value value,
                                             const ir::Inst* consumer) const;
    [[nodiscard]] std::optional<u16> LowViewHome(ir::Inst* value) const;
    [[nodiscard]] bool HasValueHome(ir::Inst* value) const;
    [[nodiscard]] bool HasLowView(ir::Inst* value) const;

    [[nodiscard]] std::optional<GuestStateMap::FixedHomeValue> FixedHomeForUse(
            ir::Value value, const ir::Inst* consumer) const;
    [[nodiscard]] bool ValueFullyResident(ir::Inst* definition) const;

    // Target combines may constrain a result/view, but never edit the ordinary
    // allocation or infer guest-state safety through a materializing JIT API.
    void AssignValueHome(ir::Inst* value, u16 home);
    void AssignLowView(ir::Inst* value, u16 home);

    [[nodiscard]] bool IsDeadPinnedGPRWrite(ir::Inst* inst) const;
    [[nodiscard]] std::optional<PinnedGPRCopy> MatchPinnedGPRCopy(ir::Inst* inst) const;
    [[nodiscard]] std::optional<PinnedSelectPublication>
    MatchPinnedSelectPublication(ir::Inst* publication) const;
    [[nodiscard]] std::optional<PinnedGPRValueTransfer>
    MatchPinnedGPRValueTransfer(ir::Inst* publication) const;
    [[nodiscard]] std::optional<PinnedGPRPublicationView>
    MatchPinnedGPRPublicationView(ir::Inst* publication) const;
    [[nodiscard]] std::optional<SpilledGPRPublication>
    MatchSpilledGPRPublication(ir::Inst* publication) const;
    [[nodiscard]] bool IsZeroExtendElided(ir::Inst* inst) const {
        return zero_extends.contains(inst);
    }
    [[nodiscard]] bool IsSignExtendElided(ir::Inst* inst) const {
        return sign_extends.contains(inst);
    }
    void ElideZeroExtend(ir::Inst* inst) { zero_extends.insert(inst); }

private:
    void RegisterUseHome(ir::Inst* version, const ir::Inst* consumer,
                         GuestStateMap::FixedHomeValue location);
    void AssignTransferredValue(ir::Inst* value, u16 home, u32 begin, u32 end);
    [[nodiscard]] bool IsGPRMappedTo(ir::Value value, u16 home) const {
        return allocation.ValueType(value) == RegAlloc::GPR &&
               allocation.ValueGPR(value).id == home;
    }
    [[nodiscard]] bool MayFaultOrObserve(const ir::Inst& inst) const {
        return guest_state_map.MayFaultOrObserve(inst);
    }
    void PrepareDeadPinnedGPRWrites(ir::Block* block);
    void PreparePinnedGPRCopies(ir::Block* block);
    void PreparePinnedSelectPublications(ir::Block* block);
    void PreparePinnedGPRValueTransfers(ir::Block* block);
    void PreparePinnedGPRPublicationViews(ir::Block* block);

    RegAlloc& allocation;
    const FeatureSet features;
    GuestStateMap& guest_state_map;
    ir::Block* block{};
    BlockAnalysisIndex block_analysis{};
    Recipes recipes{};
    std::unordered_set<ir::Inst*> zero_extends{};
    std::unordered_set<ir::Inst*> sign_extends{};
};

}  // namespace swift::runtime::backend::arm64
