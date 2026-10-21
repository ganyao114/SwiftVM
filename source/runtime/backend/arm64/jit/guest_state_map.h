#pragma once

#include <array>
#include <utility>
#include <vector>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include "base/common_funcs.h"
#include "runtime/common/types.h"
#include "runtime/backend/gpr_value_facts.h"
#include "runtime/include/config.h"
#include "runtime/ir/block.h"

namespace swift::runtime::ir {
class HIRFunction;
}

namespace swift::runtime::backend::arm64 {

class GuestStateMap final {
public:
    using ExtensionFacts = backend::GPRWidthFacts;
    using FixedHomeValue = backend::FixedGPRValue;

    // One resident fixed-home use. Stored flat (sorted by
    // (definition, consumer) at the end of BuildValueVersions) instead
    // of a node-allocated std::map: the entry count per block is small
    // and the map was a recurring host-compile allocator customer.
    struct FixedHomeUse {
        ir::Inst* definition{};
        const ir::Inst* consumer{};
        FixedHomeValue location{};
    };
    [[nodiscard]] std::span<const FixedHomeUse> FixedHomeUses() const {
        return fixed_home_uses;
    }
    struct CoalescedWrite {
        ir::Inst* publication{};
        u16 home{};
    };

    void AnalyzeFunction(ir::HIRFunction* function,
                         const FeatureSet& features);
    [[nodiscard]] ExtensionFacts EntryExtensionFacts(
            const ir::Block* block,
            u32 home);
    void Analyze(ir::Block* block, const FeatureSet& features);
    void BuildValueVersions(
            const std::unordered_set<ir::Inst*>& ignored_publications,
            std::span<const CoalescedWrite> coalesced_writes,
            bool has_reused_publication);

    [[nodiscard]] bool FixedHomeSurvives(u32 home,
                                         u32 after,
                                         u32 before) const;
    [[nodiscard]] std::optional<u32> FirstFixedHomeClobber(u32 home, u32 after) const;
    [[nodiscard]] bool PublicationWindowSafe(
            u32 home,
            ir::Value early_value,
            u32 after,
            u32 before,
            const ir::Inst* ignored = nullptr) const;
    [[nodiscard]] std::optional<FixedHomeValue> FixedHomeForUse(
            ir::Value value,
            const ir::Inst* consumer) const;
    [[nodiscard]] u32 ResidentUseCount(ir::Inst* definition) const;
    [[nodiscard]] bool MayFaultOrObserve(const ir::Inst& inst) const;
    [[nodiscard]] static bool MayFaultOrObserve(ir::OpCode op);

private:
    using WidthFacts = std::array<ExtensionFacts, 30>;

    struct ActiveValue {
        ir::Inst* version{};
        FixedHomeValue location{};
        u32 publication{};
    };

    struct ActiveState {
        // Per-definition version lists. When the block's ids are a dense
        // span (the ReId/RPO norm), in-block definitions index
        // dense_versions by slot (Id - first_id), validated against the
        // owner pointer so an out-of-block definition with a colliding
        // id can never alias a slot; everything else keeps the hash map.
        std::vector<StackVector<ActiveValue, 2>> dense_versions{};
        std::vector<ir::Inst*> dense_owners{};
        u32 dense_first_id{};
        bool dense{};
        std::unordered_map<ir::Inst*, StackVector<ActiveValue, 2>> versions{};
        std::array<StackVector<ir::Inst*, 4>, 30> homes{};

        void InitDense(ir::Block* owner_block) {
            dense = false;
            dense_owners.clear();
            dense_versions.clear();
            if (!owner_block || owner_block->GetInstList().empty()) return;
            dense_first_id = owner_block->GetInstList().front().Id();
            for (auto& inst : owner_block->GetInstList()) {
                if (inst.Id() != dense_first_id + dense_owners.size()) {
                    dense_owners.clear();
                    return;
                }
                dense_owners.push_back(&inst);
            }
            dense_versions.resize(dense_owners.size());
            dense = true;
        }

        [[nodiscard]] std::optional<size_t> SlotOf(const ir::Inst* def) const {
            if (!dense || !def || def->Id() < dense_first_id) {
                return std::nullopt;
            }
            const size_t slot = def->Id() - dense_first_id;
            if (slot >= dense_owners.size() || dense_owners[slot] != def) {
                return std::nullopt;
            }
            return slot;
        }

        [[nodiscard]] const StackVector<ActiveValue, 2>* FindVersions(
                ir::Inst* def) const {
            if (const auto slot = SlotOf(def)) {
                const auto& found = dense_versions[*slot];
                return found.empty() ? nullptr : &found;
            }
            const auto found = versions.find(def);
            return found == versions.end() ? nullptr : &found->second;
        }

        [[nodiscard]] StackVector<ActiveValue, 2>& VersionsFor(ir::Inst* def) {
            if (const auto slot = SlotOf(def)) return dense_versions[*slot];
            return versions[def];
        }
    };

    struct FaultCaptureValue {
        const ir::Inst* boundary{};
        ir::Inst* version{};
        FixedHomeValue location{};
    };

    struct FaultWidthCapture {
        const ir::Inst* boundary{};
        WidthFacts extension_facts{};
    };

    [[nodiscard]] bool ClobbersFixedHome(const ir::Inst& inst,
                                         u32 home) const;
    void BuildFixedHomeClobberIndex() const;
    void InvalidateFixedHomeClobberIndex();
    void BuildFunctionWidthFacts();
    void PrepareCurrentEntryWidthFacts(bool fault_capture_needed);
    [[nodiscard]] ExtensionFacts CurrentEntryExtensionFacts(u32 home) const;
    [[nodiscard]] ExtensionFacts ValueExtensionFacts(
            ir::Value value,
            const ActiveState& active) const;
    [[nodiscard]] bool FaultCaptureContains(const ir::Inst& boundary,
                                             u32 home,
                                             ir::Inst* version,
                                             bool require_zero_above_32) const;
    [[nodiscard]] bool NeedsFaultCaptures() const;
    void CaptureFaultCapture(const ir::Inst& boundary,
                              const ActiveState& active,
                              const WidthFacts& width_facts);
    void PublishValue(ir::Value value,
                      u32 home,
                      u32 publication,
                      ExtensionFacts extension,
                      ActiveState& active);
    void InvalidateHome(u32 home, ActiveState& active) const;

    ir::Block* block{};
    ir::HIRFunction* function{};
    FeatureSet features{};
    // IR and instruction IDs remain final until the next Analyze(). Allocate
    // only after several successful scans of a substantial block.
    struct FixedHomeClobberIndex {
        std::array<std::vector<u16>, 32> writes, reads;
        std::vector<ir::Inst*> observers;
    };
    mutable std::unique_ptr<FixedHomeClobberIndex> fixed_home_clobbers;
    // The index object (and its vector capacities) is retained across
    // Analyze calls; built tracks whether its contents describe block.
    mutable bool fixed_home_clobbers_built{};
    mutable u8 successful_survival_scans{};
    bool function_width_facts_ready{};
    WidthFacts block_entry_width_facts{};
    std::unordered_map<const ir::Block*, WidthFacts> function_entry_width_facts{};
    StackVector<FaultCaptureValue, 16> fault_capture_values{};
    StackVector<FaultWidthCapture, 8> fault_width_captures{};
    std::vector<FixedHomeUse> fixed_home_uses{};
    std::vector<std::pair<ir::Inst*, u32>> fixed_home_use_counts{};
};

}  // namespace swift::runtime::backend::arm64
