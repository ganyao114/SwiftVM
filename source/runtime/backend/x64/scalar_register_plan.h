#pragma once

#include <array>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "runtime/ir/block.h"

namespace swift::runtime::backend::x64 {

enum class ScalarPlanRejection { None, MissingBlock, FunctionPlan, ControlFlow, InvalidSSA };
enum class ScalarCachePhase : u8 { Read, Write };

struct ScalarCacheInterval {
    ir::Inst* producer{};
    u32 slot{};
    u32 definition_position{}, last_use{};
    // An audited emitter completes every physical input read before Write.
    // Opaque/raw/extra uses retain the value through the Write phase.
    ScalarCachePhase last_use_phase{ScalarCachePhase::Write};
    // Conservative IR uses, including raw-home/vector/helper operands.
    // This list is not a count of native Read cache hits.
    std::vector<u32> use_positions;
    // False only when every use is an explicitly audited scalar Read site.
    // Metadata at Home+16 and the existing 32-byte home layout remain intact.
    bool requires_canonical_home{true};
    // Each local control instruction occupies its own sentinel segment;
    // eligible body intervals have all uses in this definition's segment.
    u32 segment{};
};

// A static scalar cache plan. Raw-home/opaque consumers retain canonical homes;
// audited scalar-only intervals can omit their low 16-byte home publications.
// Slots name SysV callee-saved RBX/RBP/R15, preserved across returning helpers.
// A fault exit must recover architectural State without consulting this cache.
// Local CFG splits the plan into independent straight-line body segments.
// At pressure, a strictly earlier-ending candidate may replace the entire
// farthest-ending interval before emission; the displaced value is home-only
// for all its uses. Equal end positions retain the existing assignment.
// Cross-segment values remain home-only. Shared FunctionPlan bodies require
// constraints obtained after CFG/Phi validation. Standalone AddPhi and the
// unvalidated has_function_plan path remain conservative rejections.
class ScalarRegisterPlan {
public:
    static constexpr std::array<unsigned, 3> host_register_indices{3, 5, 15};
    static ScalarRegisterPlan Build(const ir::Block* block, bool has_function_plan = false,
                                    bool reserve_rbx = false);
    bool Enabled() const noexcept { return rejection == ScalarPlanRejection::None; }
    ScalarPlanRejection Rejection() const noexcept { return rejection; }
    std::optional<unsigned> Slot(ir::Inst* producer) const noexcept;
    bool RequiresCanonicalHome(ir::Inst* producer) const noexcept;
    const ScalarCacheInterval* Find(ir::Inst* producer) const noexcept;
    const std::vector<ScalarCacheInterval>& Intervals() const noexcept { return intervals; }
    // Starts occur at Write; a same-position endpoint is disjoint only when
    // every final occurrence was audited as a Read before that Write.
    bool ValidateNonOverlapping() const noexcept;
private:
    friend class Translator;
    // Only Translator can create these, after ConfigurePhis succeeds. Derived
    // edge sources/destinations and metadata terminal inputs receive no slot,
    // rather than merely retaining a home alongside a stale cache register.
    struct FunctionConstraints {
        const ir::Block* block{};
        std::unordered_set<ir::Inst*> home_only;
        std::unordered_set<ir::Inst*> boundaries;
    };
    static ScalarRegisterPlan BuildValidated(const ir::Block* block,
                                             const FunctionConstraints& constraints,
                                             bool reserve_rbx = false);
    static ScalarRegisterPlan BuildImpl(const ir::Block* block,
                                       const FunctionConstraints* constraints, bool reserve_rbx);
    ScalarPlanRejection rejection{ScalarPlanRejection::MissingBlock};
    std::vector<ScalarCacheInterval> intervals;
    std::unordered_map<ir::Inst*, u32> assignment_indices;
};

} // namespace swift::runtime::backend::x64
