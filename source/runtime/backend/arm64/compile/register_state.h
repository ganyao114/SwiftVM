#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include "base/common_funcs.h"
#include "runtime/backend/gpr_value_facts.h"
#include "runtime/common/types.h"
#include "runtime/include/config.h"
#include "runtime/ir/block.h"

namespace swift::runtime::ir {
class HIRFunction;
}
namespace swift::runtime::compile {
class SemanticFacts;
class GuestStateRegisterAccesses;
}

namespace swift::runtime::backend::arm64 {

// Target physical home/width/fault proofs, scoped to one preparation owner.
// Prepare methods may scan/build; const views never prepare or allocate.
class RegisterStatePreparation final {
public:
    using ExtensionFacts = backend::GPRWidthFacts;
    using FixedHomeValue = backend::FixedGPRValue;

    // One resident fixed-home use. Stored flat (sorted by
    // (definition, consumer) at the end of PrepareValueVersions) instead
    // of a node-allocated std::map: the entry count per block is small
    // and the map was a recurring host-compile allocator customer.
    struct FixedHomeUse {
        const ir::Inst* definition{};
        const ir::Inst* consumer{};
        FixedHomeValue location{};
    };
    [[nodiscard]] std::span<const FixedHomeUse> FixedHomeUses() const { return fixed_home_uses; }
    struct CoalescedWrite {
        const ir::Inst* publication{};
        u16 home{};
    };

    void BeginFunction(const ir::HIRFunction& function,
                       const FeatureSet& features,
                       compile::SemanticFacts& facts);
    [[nodiscard]] ExtensionFacts PrepareEntryExtensionFacts(const ir::Block* block, u32 home);
    [[nodiscard]] bool HasEntryWidthFacts(const ir::Block* block) const;
    void BeginBlock(const ir::Block* block, const FeatureSet& features,
                    compile::SemanticFacts& facts);
    // The preparation owner supplies a roster sorted by std::less<Inst*>.
    void PrepareValueVersions(std::span<const ir::Inst* const> ignored_publications,
                              std::span<const CoalescedWrite> coalesced_writes,
                              bool has_reused_publication);

    [[nodiscard]] bool PrepareHomeSurvival(u32 home, u32 after, u32 before);
    [[nodiscard]] std::optional<u32> PrepareFirstClobber(u32 home, u32 after);
    [[nodiscard]] bool PreparePublicationWindow(u32 home,
                                                ir::Value early_value,
                                                u32 after,
                                                u32 before,
                                                const ir::Inst* ignored = nullptr);
    [[nodiscard]] std::optional<FixedHomeValue> FixedHomeForUse(ir::Value value,
                                                                const ir::Inst* consumer) const;
    [[nodiscard]] u32 ResidentUseCount(const ir::Inst* definition) const;

private:
    using WidthFacts = std::array<ExtensionFacts, 30>;

    struct ActiveValue {
        const ir::Inst* version{};
        FixedHomeValue location{};
        u32 publication{};
    };

    struct ActiveState {
        // Identity/slot numbering belongs to the shared semantic input. Each
        // physical view owns only its active versions and keeps independent
        // entry, ignored-publication and early-clobber decisions.
        explicit ActiveState(std::span<const ir::Inst* const> input)
                : definitions(input), versions(input.size()) {}
        std::span<const ir::Inst* const> definitions;
        std::vector<StackVector<ActiveValue, 2>> versions;
        std::array<StackVector<const ir::Inst*, 4>, 30> homes{};

        [[nodiscard]] std::optional<size_t> SlotOf(const ir::Inst* def) const {
            if (!def) return std::nullopt;
            const auto found = std::lower_bound(
                    definitions.begin(), definitions.end(), def, std::less<const ir::Inst*>{});
            return found != definitions.end() && *found == def
                    ? std::optional{static_cast<size_t>(found - definitions.begin())}
                    : std::nullopt;
        }

        [[nodiscard]] const StackVector<ActiveValue, 2>* FindVersions(const ir::Inst* def) const {
            if (const auto slot = SlotOf(def)) {
                const auto& found = versions[*slot];
                return found.empty() ? nullptr : &found;
            }
            return nullptr;
        }

        [[nodiscard]] StackVector<ActiveValue, 2>& VersionsFor(const ir::Inst* def) {
            const auto slot = SlotOf(def);
            ASSERT_MSG(slot.has_value(), "guest-state definition missing from shared catalog");
            return versions[*slot];
        }
    };

    struct FaultCaptureValue {
        const ir::Inst* boundary{};
        const ir::Inst* version{};
        FixedHomeValue location{};
    };

    struct FaultWidthCapture {
        const ir::Inst* boundary{};
        WidthFacts extension_facts{};
    };

    [[nodiscard]] bool ClobbersFixedHome(const ir::Inst& inst, u32 home) const;
    void PrepareGPRWindows();
    void ResetGPRWindows();
    [[nodiscard]] std::optional<u32> FirstClobber(u32 home, u32 after, u32 before,
                                                std::optional<u32> ignored = {}) const;
    void BuildFunctionWidthFacts();
    void PrepareCurrentEntryWidthFacts(bool fault_capture_needed);
    [[nodiscard]] ExtensionFacts CurrentEntryExtensionFacts(u32 home) const;
    [[nodiscard]] ExtensionFacts ValueExtensionFacts(ir::Value value,
                                                     const ActiveState& active) const;
    [[nodiscard]] bool FaultCaptureContains(const ir::Inst& boundary,
                                            u32 home,
                                            const ir::Inst* version,
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

    const ir::Block* block{};
    const ir::HIRFunction* function{};
    compile::SemanticFacts* semantic_facts{};
    FeatureSet features{};
    // Static read/write positions belong to the shared sealed input. Only the
    // feature-resolved barrier projection belongs to this physical view.
    const compile::GuestStateRegisterAccesses* gpr_accesses{};
    struct ClobberProjection {
        struct Entry {
            u32 position;
            u32 homes;
        };
        std::vector<Entry> entries;
        u32 homes{};
    };
    std::unique_ptr<ClobberProjection> clobber_projection;
    bool gpr_windows_ready{};
    u8 successful_survival_scans{};
    bool function_width_facts_ready{};
    WidthFacts block_entry_width_facts{};
    std::vector<WidthFacts> function_entry_width_facts{};
    StackVector<FaultCaptureValue, 16> fault_capture_values{};
    StackVector<FaultWidthCapture, 8> fault_width_captures{};
    std::vector<FixedHomeUse> fixed_home_uses{};
    std::vector<std::pair<const ir::Inst*, u32>> fixed_home_use_counts{};
};

}  // namespace swift::runtime::backend::arm64
