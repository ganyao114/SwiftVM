#pragma once

#include "scalar_allocation_plan.h"
#include "wide.h"

namespace swift::runtime::backend::loongarch64 {

enum class SparseHomeReason : u32 {
    Spill = 1u << 0,
    Vector = 1u << 1,
    HelperInstruction = 1u << 2,
    HelperInput = 1u << 3,
    ProducerPseudo = 1u << 4,
    RawContainer = 1u << 5,
    PhiDestination = 1u << 6,
    Split = 1u << 7,
};

// Low-only writes depend on the prologue's zero high half. They must never
// reuse a slot that carried a full raw payload, including narrow BitCasts.
// Unproven protocols (notably Local storage and VOID helper ownhomes) remain
// private rather than relying on SSA liveness to describe their lifetime.
enum class SparseHomePayload { ZeroHigh, Full128, Isolated };

struct SparseHomesStats {
    u32 instructions{}, required_homes{}, home_count{1};
    // Counts overlap: one home can be required by several access protocols.
    u32 spill_homes{}, vector_homes{}, helper_homes{}, pseudo_homes{}, raw_homes{}, phi_homes{};
    u32 split_homes{}, paired_homes{}, reused_homes{}, interference_edges{}, cfg_nodes{};
    u32 zero_high_homes{}, full_payload_homes{}, isolated_homes{};
    bool reuse_requested{}, reuse_active{}, budget_rejected{};
    u64 dense_bytes{16}, compact_bytes{16};
    u64 unique_bytes{16};
    // The reuse layout reserves slot 0. Savings can be negative for tiny
    // blocks with no reusable values; report that cost instead of hiding it.
    s64 saved_bytes{};
};

// An offline inventory of the homes required by the current emitters. The
// source IR and its public IDs are never mutated. A future emitted clone may
// install HomeId() only after all cloning/re-ID work has finished.
//
// Optional slot reuse uses all-type whole-CFG liveness, physical definitions,
// parallel phi copies, split staging, and paired LASX publication lifetimes.
// Unassigned scalars retain homes, so an empty ScalarAllocationPlan is also
// supported. The default inventory keeps the original unique-ID layout.
class SparseHomesPlan {
public:
    static constexpr u32 kStride = 16;
    static SparseHomesPlan Build(ir::Block* block, const ScalarAllocationPlan& allocation,
                                 const RegisterPlan::Exits& exits = {},
                                 const ir::Terminal* terminal = nullptr,
                                 bool reuse = false,
                                 const RegisterPlan::Targets& targets = {},
                                 const RegisterPlan::Stops& stops = {},
                                 std::span<const WideVectorPair> wide = {});

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
    std::optional<SparseHomePayload> Payload(ir::Inst* inst) const;
    const LiveSet& RequiredSet() const { return required; }
    u32 HomeCount() const { return stats.home_count; }
    const SparseHomesStats& Stats() const { return stats; }

private:
    LiveSet required;
    std::unordered_map<ir::Inst*, u16> homes;
    std::unordered_map<ir::Inst*, u32> reasons;
    std::unordered_map<ir::Inst*, SparseHomePayload> payloads;
    SparseHomesStats stats{};
};

}  // namespace swift::runtime::backend::loongarch64
