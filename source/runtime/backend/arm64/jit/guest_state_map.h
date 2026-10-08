#pragma once

#include <array>
#include <map>
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

    [[nodiscard]] const auto& FixedHomeUses() const { return fixed_home_uses; }
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
        std::unordered_map<ir::Inst*, StackVector<ActiveValue, 2>> versions{};
        std::array<StackVector<ir::Inst*, 4>, 30> homes{};
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
    using FixedHomeClobberIndex = std::array<std::vector<u16>, 32>;
    mutable std::unique_ptr<FixedHomeClobberIndex> fixed_home_clobbers;
    mutable u8 successful_survival_scans{};
    bool function_width_facts_ready{};
    WidthFacts block_entry_width_facts{};
    std::unordered_map<const ir::Block*, WidthFacts> function_entry_width_facts{};
    StackVector<FaultCaptureValue, 16> fault_capture_values{};
    StackVector<FaultWidthCapture, 8> fault_width_captures{};
    std::map<std::pair<ir::Inst*, const ir::Inst*>, FixedHomeValue> fixed_home_uses{};
    std::map<ir::Inst*, u32> fixed_home_use_counts{};
};

}  // namespace swift::runtime::backend::arm64
