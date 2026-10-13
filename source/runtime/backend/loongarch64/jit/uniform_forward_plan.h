#pragma once

#include <optional>
#include <vector>
#include "register_plan.h"

namespace swift::runtime::backend::loongarch64 {

struct UniformForwardStats {
    bool enabled{}, accepted{}, spill_audit{};
    u32 loads_seen{}, stores_retained{}, barriers{}, iterations{}, cfg_nodes{}, joins{};
    u32 candidates{}, loads_forwarded{}, rejected_sources{}, allocation_rejections{}, allocation_rounds{};
    u32 source_spill_rejections{}, spill_growth_rejections{};
    u32 baseline_spilled_values{}, peak_spilled_values{}, final_spilled_values{};
};

// Read forwarding only. Stores retain their identity, view width, position and
// operands, including at retirement, fault and asynchronous observation exits.
// Apply/Reject operate only on the caller's private emitted clone, before any
// liveness, allocation or compact home IDs are installed.
class UniformForwardPlan {
public:
    static UniformForwardPlan Build(ir::Block* block, const RegisterPlan::Targets& targets = {},
                                    const RegisterPlan::Stops& stops = {},
                                    const RegisterPlan::Exits& exits = {},
                                    std::optional<u32> uniform_extent = std::nullopt);
    void Apply();
    // Keep sources in registers (or rematerialize them) rather than trading a
    // state read for a new spill. Rebuild allocation after any rejected load.
    template<class Allocator> bool RejectSpilledSources(const Allocator& allocation) {
        bool changed{};
        for (auto& replacement : replacements) if (replacement.active &&
                !allocation.Register(replacement.source.Def()) &&
                !allocation.Rematerialized(replacement.source.Def())) {
            Restore(replacement); ++stats.allocation_rejections;
            ++stats.source_spill_rejections; changed = true;
        }
        return changed;
    }
    void SetBaselineSpills(u32 spills) {
        stats.spill_audit = true; stats.baseline_spilled_values = spills;
    }
    void RecordAllocatedSpills(u32 spills) {
        stats.final_spilled_values = spills;
        if (spills > stats.peak_spilled_values) stats.peak_spilled_values = spills;
    }
    // A retained source can still evict another value. Keep the aggregate
    // spill count within the unmodified block's allocation budget too.
    bool RejectSpillGrowth(u32 spills);
    void RejectAll();
    void RecordAllocationRound() { ++stats.allocation_rounds; }
    const UniformForwardStats& Stats() const { return stats; }

private:
    struct Replacement {
        ir::Inst* load{};
        ir::Value source;
        u32 first_byte{}, size{};
        ir::Uniform uniform;
        ir::ValueType type{};
        bool active{};
    };
    void Restore(Replacement& replacement);
    std::vector<Replacement> replacements;
    UniformForwardStats stats{};
};

}  // namespace swift::runtime::backend::loongarch64
