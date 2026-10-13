#pragma once

#include "scalar_allocation_plan.h"

namespace swift::runtime::backend::loongarch64 {

enum class SparseHomeReason : u32 {
    Spill = 1u << 0,
    Vector = 1u << 1,
    HelperInstruction = 1u << 2,
    HelperInput = 1u << 3,
    ProducerPseudo = 1u << 4,
    RawContainer = 1u << 5,
    PhiDestination = 1u << 6,
};

struct SparseHomesStats {
    u32 instructions{}, required_homes{}, home_count{1};
    // Counts overlap: one home can be required by several access protocols.
    u32 spill_homes{}, vector_homes{}, helper_homes{}, pseudo_homes{}, raw_homes{}, phi_homes{};
    u64 dense_bytes{16}, compact_bytes{16};
};

// An offline inventory of the homes required by the current emitters. The
// source IR and its public IDs are never mutated. A future emitted clone may
// install HomeId() only after all cloning/re-ID work has finished.
//
// This plan does not reuse slots: raw/helper accesses can outlive scalar
// register liveness. Unassigned scalars retain homes, so it is also safe with
// a default/empty ScalarAllocationPlan (canonical-home mode).
class SparseHomesPlan {
public:
    static constexpr u32 kStride = 16;
    static SparseHomesPlan Build(ir::Block* block, const ScalarAllocationPlan& allocation,
                                 const RegisterPlan::Exits& exits = {},
                                 const ir::Terminal* terminal = nullptr);

    // Feature-independent conservative classifier. Every guarded native path
    // that can call a typed semantic helper is included; unknown opcodes are
    // helpers until explicitly audited. Ordinary guest-address C ABI calls do
    // not consume SSA homes.
    static bool MayCallTypedHelper(ir::Inst* inst);

    bool HasHome(ir::Inst* inst) const { return homes.contains(inst); }
    std::optional<u16> HomeId(ir::Inst* inst) const;
    // Missing homes, foreign values and invalid halves fail closed. In
    // particular, no sentinel instruction ID is ever converted into slot 0.
    u32 HomeOffset(ir::Inst* inst, u32 part = 0) const;
    u32 Reasons(ir::Inst* inst) const;
    const LiveSet& RequiredSet() const { return required; }
    u32 HomeCount() const { return stats.home_count; }
    const SparseHomesStats& Stats() const { return stats; }

private:
    LiveSet required;
    std::unordered_map<ir::Inst*, u16> homes;
    std::unordered_map<ir::Inst*, u32> reasons;
    SparseHomesStats stats{};
};

}  // namespace swift::runtime::backend::loongarch64
