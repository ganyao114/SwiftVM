#pragma once

#include <array>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include "base/common_funcs.h"
#include "runtime/common/types.h"
#include "runtime/include/config.h"
#include "runtime/ir/block.h"

namespace swift::runtime::ir {
class HIRFunction;
}

namespace swift::runtime::backend::arm64 {

class GuestStateMap final {
public:
    struct ExtensionFacts {
        u8 known_zero_above{};
        u8 sign_extended_from{};
        u8 sign_extended_to{};

        [[nodiscard]] bool KnownZeroAbove(u32 bits) const {
            return known_zero_above != 0 && known_zero_above <= bits;
        }

        [[nodiscard]] bool KnownSignExtended(u32 from, u32 to) const {
            const bool explicit_fact = sign_extended_from != 0 &&
                    sign_extended_from <= from && sign_extended_to >= to;
            const bool implied_by_zero = known_zero_above != 0 &&
                    known_zero_above < from;
            return explicit_fact || implied_by_zero;
        }

        bool operator==(const ExtensionFacts&) const = default;
    };

    struct FixedHomeValue {
        u16 home{};
        u8 width{};
        ExtensionFacts extension{};

        bool operator==(const FixedHomeValue&) const = default;
    };

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
    void RegisterFixedHomeUse(ir::Inst* version,
                              const ir::Inst* consumer,
                              FixedHomeValue location);
    [[nodiscard]] std::optional<FixedHomeValue> RegisteredFixedHomeForUse(
            ir::Value value,
            const ir::Inst* consumer) const;
    [[nodiscard]] bool ValueFullyResident(ir::Inst* definition) const;
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
    bool function_width_facts_ready{};
    WidthFacts block_entry_width_facts{};
    std::unordered_map<const ir::Block*, WidthFacts> function_entry_width_facts{};
    StackVector<FaultCaptureValue, 16> fault_capture_values{};
    StackVector<FaultWidthCapture, 8> fault_width_captures{};
    std::map<std::pair<ir::Inst*, const ir::Inst*>, FixedHomeValue> fixed_home_uses{};
    std::map<ir::Inst*, u32> fixed_home_use_counts{};
    std::set<std::pair<ir::Inst*, const ir::Inst*>> registered_fixed_home_uses{};
};

}  // namespace swift::runtime::backend::arm64
