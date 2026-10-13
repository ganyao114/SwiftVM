#pragma once

#include <optional>
#include <span>
#include <unordered_map>
#include "register_plan.h"

namespace swift::runtime::backend::loongarch64 {

struct ScalarAllocationStats {
    u32 values{}, register_values{}, spilled_values{}, interference_edges{}, max_pressure{}, phi_coalesced{};
    // Unique scalar input/definition sites; these are allocation estimates,
    // not emitted instruction counters or a dynamic execution profile.
    u64 input_sites{}, definition_sites{}, spill_input_sites{}, spill_definition_sites{};
};

// A whole-CFG coloring, not an eviction cache: every eligible SSA value has
// one immutable callee-saved register index or its existing canonical home.
// Register indices name scalar_registers[0..5] in defines.h. All vector/FP
// containers keep their existing dense 16-byte homes and helper ABI.
class ScalarAllocationPlan {
public:
    static constexpr u32 kRegisterCount = 6;
    static ScalarAllocationPlan Build(ir::Block* block, const RegisterPlan& live,
                                      const RegisterPlan::Targets& targets = {},
                                      u32 register_count = kRegisterCount);

    static bool Eligible(ir::Inst* value);
    std::optional<u32> Register(ir::Inst* value) const;
    bool Spilled(ir::Inst* value) const { return Eligible(value) && !Register(value); }
    // Producer-written pseudos are definitions of their producer instruction;
    // phi destinations are simultaneous definitions of their CFG entry.
    const LiveSet& Definitions(ir::Inst* at) const { return definitions.at(at); }
    // Exact scalar operands, including flags' implicit producer rereads. Used
    // to publish canonical inputs before a typed helper/raw-container read.
    const LiveSet& Inputs(ir::Inst* at) const { return inputs.at(at); }
    bool Interferes(ir::Inst* left, ir::Inst* right) const;
    const ScalarAllocationStats& Stats() const { return stats; }

private:
    std::unordered_map<ir::Inst*, u32> registers;
    std::unordered_map<ir::Inst*, LiveSet> interference, definitions, inputs;
    ScalarAllocationStats stats{};
};

}  // namespace swift::runtime::backend::loongarch64
