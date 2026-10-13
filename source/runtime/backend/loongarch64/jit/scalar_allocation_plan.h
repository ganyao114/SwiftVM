#pragma once

#include <optional>
#include <span>
#include <unordered_map>
#include "register_plan.h"

namespace swift::runtime::backend::loongarch64 {

struct ScalarAllocationStats {
    u32 values{}, register_values{}, spilled_values{}, interference_edges{}, max_pressure{}, phi_coalesced{};
    u32 rematerialized_values{};
    // Unique scalar input/definition sites; these are allocation estimates,
    // not emitted instruction counters or a dynamic execution profile.
    u64 input_sites{}, definition_sites{}, spill_input_sites{}, spill_definition_sites{};
    u64 rematerialized_input_sites{};
    u32 split_values{}, fragments{}, register_fragments{}, home_fragments{};
    u64 split_transfer_stores{}, split_transfer_loads{};
};

struct ScalarSplitMove {
    ir::Inst* value{};
    std::optional<u32> from, to;
};

struct ScalarLiveFragment {
    ir::Inst* first{};
    ir::Inst* last{};
    std::optional<u32> reg;
};

// The whole-CFG coloring defines the stable placement on CFG edges. Optional
// splitting gives an SSA value different placements inside a basic block,
// with explicit parallel transfers before each placement change.
// Register indices name scalar_registers[0..5] in defines.h. All vector/FP
// containers keep their existing dense 16-byte homes and helper ABI.
class ScalarAllocationPlan {
public:
    static constexpr u32 kRegisterCount = 6;
    static ScalarAllocationPlan Build(ir::Block* block, const RegisterPlan& live,
                                      const RegisterPlan::Targets& targets = {},
                                      u32 register_count = kRegisterCount,
                                      bool rematerialize_constants = false,
                                      bool split_live_ranges = false,
                                      const RegisterPlan::Stops& stops = {},
                                      const RegisterPlan::Exits& exits = {});

    static bool Eligible(ir::Inst* value);
    std::optional<u32> Register(ir::Inst* value) const;
    std::optional<u32> RegisterAt(ir::Inst* value, ir::Inst* at) const;
    // Emit all source-register stores before any destination-register loads.
    // A register-to-register transfer also stages through its own SSA home,
    // so cycles and aliases never overwrite another move's unread input.
    std::span<const ScalarSplitMove> MovesBefore(ir::Inst* at) const;
    std::span<const ScalarLiveFragment> Fragments(ir::Inst* value) const;
    bool NeedsSplitHome(ir::Inst* value) const { return split_homes.contains(value); }
    bool SplittingEnabled() const { return splitting; }
    bool Rematerialized(ir::Inst* value) const { return immediates.contains(value); }
    std::optional<u64> Immediate(ir::Inst* value) const;
    bool Spilled(ir::Inst* value) const { return Eligible(value) && !Register(value) && !Rematerialized(value); }
    // Producer-written pseudos are definitions of their producer instruction;
    // phi destinations are simultaneous definitions of their CFG entry.
    const LiveSet& Definitions(ir::Inst* at) const { return definitions.at(at); }
    // Exact scalar operands, including flags' implicit producer rereads. Used
    // to publish canonical inputs before a typed helper/raw-container read.
    const LiveSet& Inputs(ir::Inst* at) const { return inputs.at(at); }
    bool Interferes(ir::Inst* left, ir::Inst* right) const;
    const ScalarAllocationStats& Stats() const { return stats; }

private:
    void Split(ir::Block* block, const RegisterPlan& live,
               const RegisterPlan::Targets& targets, const RegisterPlan::Stops& stops,
               const RegisterPlan::Exits& exits, u32 register_count);
    std::unordered_map<ir::Inst*, u32> registers;
    std::unordered_map<ir::Inst*, u64> immediates;
    std::unordered_map<ir::Inst*, LiveSet> interference, definitions, inputs;
    std::unordered_map<ir::Inst*, std::unordered_map<ir::Inst*, u32>> point_registers;
    std::unordered_map<ir::Inst*, std::vector<ScalarSplitMove>> split_moves;
    std::unordered_map<ir::Inst*, std::vector<ScalarLiveFragment>> fragments;
    LiveSet split_homes;
    bool splitting{};
    ScalarAllocationStats stats{};
};

}  // namespace swift::runtime::backend::loongarch64
